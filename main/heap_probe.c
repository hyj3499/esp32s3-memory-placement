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

/* 크기 구간. 어떤 크기대가 어느 리전으로 가는지가 이 표에서 나온다. */
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

    /* [0] = 내부 SRAM, [1] = PSRAM. 두 쪽을 같이 찍어야 "얼마가 옮겨졌나" 를
     * 한 번의 부팅으로 볼 수 있다. 내부만 찍으면 사라진 양은 알아도 그것이
     * 어디로 갔는지, 아니면 애초에 안 잡힌 것인지 구분되지 않는다. */
    size_t cnt[2][BUCKETS]   = { { 0 } };
    size_t bytes[2][BUCKETS] = { { 0 } };
    size_t total[2] = { 0 }, alive[2] = { 0 }, shown[2] = { 0 };

    static const char *k_region_name[2] = { "내부 SRAM", "PSRAM" };

    ESP_LOGI(TAG, "---- 할당 덤프 (R,idx,size,addr,I|E,freed,콜스택 %d단) ----",
             CONFIG_HEAP_TRACING_STACK_DEPTH);

    for (size_t i = 0; i < n; i++) {
        heap_trace_record_t r;
        if (heap_trace_get(i, &r) != ESP_OK) {
            break;
        }

        /* 내부도 PSRAM 도 아닌 것(RTC 등)은 이 측정의 관심 밖이다. */
        int reg;
        if (esp_ptr_internal(r.address)) {
            reg = 0;
        } else if (esp_ptr_external_ram(r.address)) {
            reg = 1;
        } else {
            continue;
        }

        size_t bk = bucket_of(r.size);
        cnt[reg][bk]++;
        bytes[reg][bk] += r.size;
        total[reg] += r.size;
        if (!r.freed) {
            alive[reg] += r.size;
        }
        shown[reg]++;

        /* 콜스택을 전부 찍는다. 깊이 2 로는 [0]=heap_caps_malloc_default,
         * [1]=malloc 처럼 할당기 껍데기만 나와 정작 누가 불렀는지 알 수 없다. */
        char cs[CONFIG_HEAP_TRACING_STACK_DEPTH * 11 + 1];
        int  off = 0;
        for (int j = 0; j < CONFIG_HEAP_TRACING_STACK_DEPTH; j++) {
            off += snprintf(cs + off, sizeof(cs) - off, "%s0x%08x",
                            j ? "," : "", (unsigned)(uintptr_t)r.alloced_by[j]);
        }

        ESP_LOGI(TAG, "R,%u,%u,%p,%c,%d,%s",
                 (unsigned)i, (unsigned)r.size, r.address,
                 reg ? 'E' : 'I', r.freed ? 1 : 0, cs);
    }

    for (int reg = 0; reg < 2; reg++) {
        ESP_LOGI(TAG, "---- 크기 분포 / %s (%u 건) ----",
                 k_region_name[reg], (unsigned)shown[reg]);
        for (size_t i = 0; i < BUCKETS; i++) {
            if (cnt[reg][i] == 0) {
                continue;
            }
            ESP_LOGI(TAG, "B,%c,<=%u,%u건,%u바이트", reg ? 'E' : 'I',
                     (unsigned)k_bucket_max[i],
                     (unsigned)cnt[reg][i], (unsigned)bytes[reg][i]);
        }
    }

    /* total 은 "이 구간에 잡힌 적이 있는 바이트의 합" 이지 동시 피크가 아니다.
     * 같은 주소가 재사용되면 중복 계상되므로 피크의 상한으로만 읽는다.
     * 피크 자체는 min_free 로만 나온다. 두 숫자는 다른 것을 뜻한다. */
    ESP_LOGI(TAG, "합계 내부=%u PSRAM=%u / 살아있음 내부=%u PSRAM=%u",
             (unsigned)total[0], (unsigned)total[1],
             (unsigned)alive[0], (unsigned)alive[1]);
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
