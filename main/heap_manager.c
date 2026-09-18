#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "snapshot";

static const struct {
    const char *name;
    uint32_t    caps;
} k_regions[] = {
    { "INTERNAL",      MALLOC_CAP_INTERNAL                   },
    { "INTERNAL|8BIT", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT },
    { "SPIRAM",        MALLOC_CAP_SPIRAM                     },
    { "DMA",           MALLOC_CAP_DMA                        },
};

static unsigned frag_percent(const multi_heap_info_t *info)
{
    if (info->total_free_bytes == 0) {
        return 0;
    }
    return 100u - (unsigned)((info->largest_free_block * 100u) / info->total_free_bytes);
}

static void log_snapshot(void)
{
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

void app_main(void)
{
    ESP_LOGI(TAG, "==== boot snapshot ====");
    log_snapshot();
}
