#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"

#include "mem_place.h"

static const char *TAG = "place";

/* 의도 → caps.
 *
 * MALLOC_CAP_DMA 는 ESP32-S3 에서 "내부 SRAM 이고 DMA 가 닿는 곳" 이다.
 * INTERNAL 을 따로 붙이지 않는 이유는 DMA 가 이미 그것을 함의하기 때문이다
 * (STUDY.md §9 — DMA 가 INTERNAL 보다 7,788 작게 나오는 것이 그 증거다).
 *
 * 폴백 여부가 이 표의 본론이다. DMA 만 폴백이 없다. */
static uint32_t caps_for(mem_intent_t intent)
{
    switch (intent) {
    case MEM_INTENT_DMA:
        return MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    case MEM_INTENT_BULK:
        return MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    case MEM_INTENT_HOT:
    default:
        return MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    }
}

static const char *intent_name(mem_intent_t intent)
{
    switch (intent) {
    case MEM_INTENT_DMA:  return "DMA";
    case MEM_INTENT_BULK: return "BULK";
    case MEM_INTENT_HOT:  return "HOT";
    default:              return "?";
    }
}

void *mem_place_alloc(size_t size, mem_intent_t intent, const char *what)
{
    void *p = heap_caps_malloc(size, caps_for(intent));

    if (p == NULL && intent != MEM_INTENT_DMA) {
        /* 폴백. DMA 만 빠져 있는 것이 이 함수의 전부라고 해도 된다.
         * BULK 는 PSRAM 이 모자라면 내부로, HOT 은 내부가 모자라면 PSRAM 으로.
         * 둘 다 "느려지지만 동작한다" 이므로 폴백에 의미가 있다. */
        const uint32_t fallback = (intent == MEM_INTENT_BULK)
                                      ? (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
                                      : (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        p = heap_caps_malloc(size, fallback);
        if (p != NULL) {
            ESP_LOGW(TAG, "%s(%s, %zu B) 1순위 실패 → 폴백", what, intent_name(intent), size);
        }
    }

    if (p == NULL) {
        ESP_LOGE(TAG, "%s(%s, %zu B) 할당 실패%s", what, intent_name(intent), size,
                 intent == MEM_INTENT_DMA ? " — DMA 는 폴백하지 않는다" : "");
        return NULL;
    }

    /* 의도대로 갔는지 검증해서 남긴다. caps 를 믿지 않고 포인터를 되묻는 것이
     * 요점이다 — 할당기가 무엇을 줬는지는 주소가 말해 준다. */
    const bool external = esp_ptr_external_ram(p);
    ESP_LOGI(TAG, "%-16s %-4s %7zu B → %p (%s)%s",
             what, intent_name(intent), size, p,
             external ? "PSRAM" : "내부 SRAM",
             (intent == MEM_INTENT_DMA && external) ? "  ⚠️ DMA 인데 외부다!" : "");

    return p;
}

void mem_place_free(void *ptr)
{
    heap_caps_free(ptr);
}
