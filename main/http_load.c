#include <stdbool.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "heap_probe.h"
#include "http_load.h"
#include "mem_snapshot.h"

static const char *TAG = "load";

/* TLS 핸드셰이크가 이 태스크에서 가장 깊게 들어간다. 8192 로 시작했으나 Phase 3
 * 워터마크 실측이 부하 중 사용량 3,256(41%)을 보여 줬다. 5120 으로 내려도 마진이
 * 1,864(관측 사용량의 1.6배) 남는다.
 * ⚠️ 워터마크는 관측된 최악이지 가능한 최악이 아니다. 실패 핸드셰이크 경로는
 * 아직 재지 않았으므로 이 값은 그 측정 뒤에 다시 판단한다. */
#define LOAD_STACK_BYTES 5120

/* 계측(monitor, prio 3)보다 높게. 부하가 계측에 밀리면 측정 구간이 늘어진다. */
#define LOAD_PRIO        4

#define LOAD_RUN_BIT     (1 << 0)
#define HTTP_TIMEOUT_MS  10000

/* 본문을 버리기 위한 싱크. 정적으로 둬서 부하 생성기 자신이 힙을 건드리지 않게 한다.
 * 관측하려는 것은 esp_http_client 와 mbedTLS 의 할당이지 내 버퍼가 아니다. */
static char s_sink[1024];

/* mem_monitor.c 와 같은 이유로 전부 정적이다. 이벤트 그룹까지 정적으로 둔 것은
 * 부하를 켜고 끄는 행위 자체가 힙에 흔적을 남기지 않게 하기 위함이다. */
static StackType_t        s_stack[LOAD_STACK_BYTES];
static StaticTask_t       s_tcb;
static TaskHandle_t       s_task;
static StaticEventGroup_t s_event_buf;
static EventGroupHandle_t s_events;

static uint32_t s_seq;
static uint32_t s_fail;

static void do_request(void)
{
    esp_http_client_config_t cfg = {
        .url               = CONFIG_MHM_LOAD_URL,
        .timeout_ms        = HTTP_TIMEOUT_MS,

        /* 공용 루트 CA 번들. CONFIG_MBEDTLS_CERTIFICATE_BUNDLE 이 기본 y 라
         * 개별 인증서를 넣지 않아도 공개 HTTPS 사이트에 붙는다. */
        .crt_bundle_attach = esp_crt_bundle_attach,

        /* 매 요청마다 새 연결을 맺는다. keep-alive 로 재사용하면 핸드셰이크가
         * 한 번만 일어나서 관측할 할당/해제 반복이 사라진다. 단편화를 보려면
         * 큰 블록이 잡히고 풀리는 일을 반복시켜야 한다. */
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "#%lu client 생성 실패 (힙 부족일 수 있다)", (unsigned long)s_seq);
        s_fail++;
        return;
    }

    const int64_t t0 = esp_timer_get_time();

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "#%lu open 실패: %s", (unsigned long)s_seq, esp_err_to_name(err));
        s_fail++;
        esp_http_client_cleanup(client);
        return;
    }

    /* 헤더를 먼저 읽어야 상태 코드와 content-length 가 확정된다.
     * 음수면 chunked 응답이라 길이를 미리 알 수 없다는 뜻. */
    int64_t len = esp_http_client_fetch_headers(client);

    int total = 0;
    int r;
    while ((r = esp_http_client_read(client, s_sink, sizeof(s_sink))) > 0) {
        total += r;
    }

    int     status = esp_http_client_get_status_code(client);
    int32_t ms     = (int32_t)((esp_timer_get_time() - t0) / 1000);

    /* close 는 연결만 끊고, cleanup 이 핸들과 내부 버퍼를 해제한다. cleanup 을
     * 빼먹으면 매 요청이 그대로 누수가 되어 몇 번 안 가 힙이 마른다. */
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (r < 0) {
        ESP_LOGW(TAG, "#%lu 본문 읽기 실패 (%d 바이트까지 받음)",
                 (unsigned long)s_seq, total);
        s_fail++;
        return;
    }

    ESP_LOGI(TAG, "#%lu status=%d len=%lld body=%d %ldms 실패누적=%lu",
             (unsigned long)s_seq, status, (long long)len, total,
             (long)ms, (unsigned long)s_fail);
}

/* 추적은 첫 요청 한 번만 건다. 실패 경로로 빠져도 덤프가 나오도록 요청 본체를
 * do_request() 로 감쌌다. heap_probe_end() 는 추적 중이 아니면 아무 일도 안 한다. */
static void one_request(void)
{
    const bool trace = heap_probe_should_trace(s_seq);

    if (trace) {
        heap_probe_begin();
    }

    do_request();

    if (trace) {
        heap_probe_end();
    }
}

static void load_task(void *arg)
{
    for (;;) {
        /* stop 이면 여기서 블로킹한다. 태스크를 지우지 않는 이유: 정적 스택을
         * 쓰므로 삭제해도 회수되는 메모리가 없고, 재시작 경합만 생긴다. */
        xEventGroupWaitBits(s_events, LOAD_RUN_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        one_request();
        s_seq++;

        /* 요청 몇 번마다 리전 4종을 전부 찍는다. 시간축은 mem_monitor 가 담당하고
         * 여기서는 "요청 N회 누적" 이라는 다른 축을 남긴다. 단편화가 시간이 아니라
         * 요청 횟수에 비례해 쌓이는지 보려면 이 축이 필요하다. */
        if (CONFIG_MHM_LOAD_SNAPSHOT_EVERY > 0 &&
            s_seq % CONFIG_MHM_LOAD_SNAPSHOT_EVERY == 0) {
            char label[32];
            snprintf(label, sizeof(label), "요청 %lu회 누적", (unsigned long)s_seq);
            mem_snapshot_log(label);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_MHM_LOAD_INTERVAL_MS));
    }
}

esp_err_t http_load_start(void)
{
    if (s_events == NULL) {
        s_events = xEventGroupCreateStatic(&s_event_buf);
        if (s_events == NULL) {
            return ESP_FAIL;
        }
    }

    if (s_task == NULL) {
        s_task = xTaskCreateStatic(load_task, "http_load", LOAD_STACK_BYTES,
                                   NULL, LOAD_PRIO, s_stack, &s_tcb);
        if (s_task == NULL) {
            return ESP_FAIL;
        }
    }

    xEventGroupSetBits(s_events, LOAD_RUN_BIT);

    ESP_LOGI(TAG, "부하 시작: %s (간격 %d ms)",
             CONFIG_MHM_LOAD_URL, CONFIG_MHM_LOAD_INTERVAL_MS);
    return ESP_OK;
}

void http_load_stop(void)
{
    if (s_events == NULL) {
        return;
    }

    xEventGroupClearBits(s_events, LOAD_RUN_BIT);
    ESP_LOGI(TAG, "부하 중지 (진행 중인 요청은 끝까지 간다)");
}

bool http_load_is_running(void)
{
    return s_events != NULL && (xEventGroupGetBits(s_events) & LOAD_RUN_BIT) != 0;
}
