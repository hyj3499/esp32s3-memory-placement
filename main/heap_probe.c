#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "sdkconfig.h"

#include "heap_probe.h"

#if CONFIG_HEAP_TRACING_STANDALONE

#include "esp_heap_trace.h"

static const char *TAG = "probe";

/* 레코드 버퍼를 PSRAM 에 둔다. 내부 SRAM 에 두면 44 바이트 x N 만큼 관측 대상을
 * 밀어내는데, 지금 재려는 것이 바로 그 내부 SRAM 이다. 계측 태스크를 전부 정적으로
 * 올린 것과 같은 이유다.
 * 대가: ISR 에서 일어난 할당은 기록되지 않는다 (esp_heap_trace.h:70).
 * 표적인 TLS 핸드셰이크는 태스크 컨텍스트라 해당 없다. */
EXT_RAM_BSS_ATTR static heap_trace_record_t s_records[CONFIG_MHM_TRACE_RECORDS];

static bool s_running;
static bool s_done;

bool heap_probe_should_trace(uint32_t seq)
{
    return CONFIG_MHM_TRACE_AT_REQUEST >= 0
        && !s_done
        && seq == (uint32_t)CONFIG_MHM_TRACE_AT_REQUEST;
}

void heap_probe_begin(void)
{
    if (s_running || s_done) {
        return;
    }

    esp_err_t err = heap_trace_init_standalone(s_records, CONFIG_MHM_TRACE_RECORDS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init 실패: %s", esp_err_to_name(err));
        s_done = true;
        return;
    }

    /* HEAP_TRACE_ALL 을 쓴다. LEAKS 모드는 free 된 레코드를 지워서 "무엇이 남았나"
     * 만 보여준다. 여기서 알고 싶은 것은 누수가 아니라 크기 분포이므로 해제된
     * 것까지 남아야 한다. 누수가 없다는 것은 이미 45 회 부하로 확인했다. */
    err = heap_trace_start(HEAP_TRACE_ALL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "start 실패: %s", esp_err_to_name(err));
        s_done = true;
        return;
    }

    s_running = true;
    ESP_LOGI(TAG, "추적 시작 — 레코드 %d 개 (PSRAM)", CONFIG_MHM_TRACE_RECORDS);
}

/* 크기 구간. 래퍼의 임계값을 어디에 둘지가 이 표에서 나온다. */
static const size_t k_bucket_max[] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, SIZE_MAX,
};
#define BUCKETS (sizeof(k_bucket_max) / sizeof(k_bucket_max[0]))

static size_t bucket_of(size_t size)
{
    for (size_t i = 0; i < BUCKETS; i++) {
        if (size <= k_bucket_max[i]) {
            return i;
        }
    }
    return BUCKETS - 1;
}

void heap_probe_end(void)
{
    if (!s_running) {
        return;
    }

    heap_trace_stop();
    s_running = false;
    s_done    = true;

    heap_trace_summary_t s;
    if (heap_trace_summary(&s) == ESP_OK) {
        ESP_LOGI(TAG, "alloc=%u free=%u 기록=%u/%u 최대=%u overflow=%s",
                 (unsigned)s.total_allocations, (unsigned)s.total_frees,
                 (unsigned)s.count, (unsigned)s.capacity,
                 (unsigned)s.high_water_mark,
                 s.has_overflowed ? "예 (MHM_TRACE_RECORDS 를 늘릴 것)" : "아니오");
    }

    /* heap_trace_dump_caps() 를 쓰지 않는다. 그 함수는 덤프 전체를
     * portENTER_CRITICAL 안에서 돌리는데(heap_trace_standalone.c:343), 264 줄을
     * 115200 baud 로 뱉으면 인터럽트를 2 초 넘게 끈 채 있게 되어 인터럽트
     * 워치독(기본 300 ms)이 코어를 죽인다. 실제로 죽었다. TROUBLESHOOTING.md #3
     *
     * heap_trace_get() 은 호출마다 락을 잡고 놓으므로(같은 파일 269 행) 직접
     * 순회하면 critical section 이 레코드 하나 길이로 짧아진다. */
    size_t n = heap_trace_get_count();

    size_t cnt[BUCKETS]   = { 0 };
    size_t bytes[BUCKETS] = { 0 };
    size_t total = 0, alive = 0, shown = 0;

    ESP_LOGI(TAG, "---- 내부 SRAM 할당 (R,idx,size,addr,freed,caller0,caller1) ----");

    for (size_t i = 0; i < n; i++) {
        heap_trace_record_t r;
        if (heap_trace_get(i, &r) != ESP_OK) {
            break;
        }

        /* 표적은 내부 SRAM 이다. PSRAM 으로 간 mbedTLS 몫은 이미 38,896 으로 안다. */
        if (!esp_ptr_internal(r.address)) {
            continue;
        }

        size_t b = bucket_of(r.size);
        cnt[b]++;
        bytes[b] += r.size;
        total += r.size;
        if (!r.freed) {
            alive += r.size;
        }
        shown++;

        /* 콜스택을 전부 찍는다. 깊이 2 로는 [0]=heap_caps_malloc_default,
         * [1]=malloc 처럼 할당기 껍데기만 나와 정작 누가 불렀는지 알 수 없다. */
        char cs[CONFIG_HEAP_TRACING_STACK_DEPTH * 11 + 1];
        int  off = 0;
        for (int j = 0; j < CONFIG_HEAP_TRACING_STACK_DEPTH; j++) {
            off += snprintf(cs + off, sizeof(cs) - off, "%s0x%08x",
                            j ? "," : "", (unsigned)(uintptr_t)r.alloced_by[j]);
        }

        ESP_LOGI(TAG, "R,%u,%u,%p,%d,%s",
                 (unsigned)i, (unsigned)r.size, r.address, r.freed ? 1 : 0, cs);
    }

    ESP_LOGI(TAG, "---- 크기 분포 (내부 SRAM %u 건) ----", (unsigned)shown);
    for (size_t i = 0; i < BUCKETS; i++) {
        if (cnt[i] == 0) {
            continue;
        }
        ESP_LOGI(TAG, "B,<=%u,%u건,%u바이트",
                 (unsigned)k_bucket_max[i], (unsigned)cnt[i], (unsigned)bytes[i]);
    }

    /* total 은 "이 구간에 잡힌 적이 있는 바이트의 합" 이지 동시 피크가 아니다.
     * 피크(47,039)는 min_free 로만 나온다. 두 숫자는 다른 것을 뜻한다. */
    ESP_LOGI(TAG, "내부 합계=%u 바이트 / 추적 끝까지 살아있음=%u 바이트",
             (unsigned)total, (unsigned)alive);
    ESP_LOGI(TAG, "---- 덤프 끝 ----");
}

#else /* !CONFIG_HEAP_TRACING_STANDALONE */

bool heap_probe_should_trace(uint32_t seq)
{
    (void)seq;
    return false;
}

void heap_probe_begin(void) { }
void heap_probe_end(void)   { }

#endif
