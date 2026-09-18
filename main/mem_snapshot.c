#include "esp_heap_caps.h"
#include "esp_log.h"

#include "mem_snapshot.h"

static const char *TAG = "snapshot";

/* INTERNAL 과 INTERNAL|8BIT 는 현재 설정에서 값이 같다. 칩의 성질이 아니라
 * MEMPROT 설정의 결과이므로 둘 다 남겨둔다. docs/STUDY.md §9 */
static const struct {
    const char *name;
    uint32_t    caps;
} k_regions[] = {
    { "INTERNAL",      MALLOC_CAP_INTERNAL                   },
    { "INTERNAL|8BIT", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT },
    { "SPIRAM",        MALLOC_CAP_SPIRAM                     },
    { "DMA",           MALLOC_CAP_DMA                        },
};

/* 절대값이 아니라 baseline 대비 증가분으로 읽어야 한다. 여러 힙 합산과 TLSF
 * 크기 클래스 내림 때문에 단편화가 0이어도 값이 뜬다. docs/STUDY.md §7 */
static unsigned frag_percent(const multi_heap_info_t *info)
{
    if (info->total_free_bytes == 0) {
        return 0;
    }
    /* 곱셈을 먼저. 나눗셈이 앞서면 정수 연산이라 항상 0% 가 된다. */
    return 100u - (unsigned)((info->largest_free_block * 100u) / info->total_free_bytes);
}

void mem_snapshot_log(const char *label)
{
    ESP_LOGI(TAG, "==== %s ====", label);
    ESP_LOGI(TAG, "%-14s %10s %10s %10s %10s %5s",
             "region", "free", "largest", "min_free", "alloc", "frag");

    for (size_t i = 0; i < sizeof(k_regions) / sizeof(k_regions[0]); i++) {
        multi_heap_info_t info;
        heap_caps_get_info(&info, k_regions[i].caps);

        ESP_LOGI(TAG, "%-14s %10zu %10zu %10zu %10zu %4u%%",
                 k_regions[i].name,
                 info.total_free_bytes,
                 info.largest_free_block,
                 info.minimum_free_bytes,
                 info.total_allocated_bytes,
                 frag_percent(&info));
    }
}

void mem_snapshot_log_line(const char *label)
{
    multi_heap_info_t in, ex;

    heap_caps_get_info(&in, MALLOC_CAP_INTERNAL);
    heap_caps_get_info(&ex, MALLOC_CAP_SPIRAM);

    /* min_free 를 같이 찍는 이유: 이 출력은 샘플링이라 두 줄 사이에서 일어난
     * 순간적인 저점을 놓친다. min_free 는 그 저점을 힙이 기억해 준 값이다. */
    ESP_LOGI(TAG, "%-12s INT free=%zu largest=%zu min=%zu frag=%u%% | PSRAM free=%zu",
             label,
             in.total_free_bytes,
             in.largest_free_block,
             in.minimum_free_bytes,
             frag_percent(&in),
             ex.total_free_bytes);
}
