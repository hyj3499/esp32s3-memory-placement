/* 배치 래퍼 — 의도를 받아 리전을 정한다.
 *
 * Phase 2 에서 여기로 내려온 것이다. 그때는 만들지 않았다. mbedTLS 의
 * `CUSTOM` 훅은 시그니처가 `(size_t n, size_t size)` 뿐이라 의도도 DMA 플래그도
 * 수명도 받을 수 없어서, 래퍼를 끼워도 `heap_caps_malloc` 재구현밖에 안 됐다.
 *
 * Phase 4 에서 사정이 바뀐다. **호출자가 자기 의도를 알고 있다.** LVGL 드로우
 * 버퍼는 DMA 가 직접 읽으므로 내부 SRAM 이 사실상 강제이고, 프레임버퍼나 로그
 * 링버퍼는 CPU 만 만지므로 PSRAM 이어도 된다. 취향이 아니라 하드웨어가 정한다.
 *
 * `heap_caps_malloc` 과의 차이는 추상화 수준 하나다 — 호출자가 caps 비트가
 * 아니라 **용도**를 넘긴다. 그래서 정책이 바뀌면 호출부가 아니라 여기만 고친다. */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    /* DMA 가 직접 읽거나 쓴다. 내부 SRAM 이어야 하고 **폴백이 없다.**
     *
     * ⚠️ "PSRAM 은 DMA 가 못 닿는다" 가 아니다. ESP32-S3 의 AHB GDMA 는 PSRAM 을
     * 지원하고(`soc_caps.h:172` `SOC_AHB_GDMA_SUPPORT_PSRAM 1`), esp_lcd 도 조건이
     * 맞으면 `SPI_TRANS_DMA_USE_PSRAM` 을 세워 직접 읽는다
     * (`esp_lcd_panel_io_spi.c:375`).
     *
     * 폴백을 막는 진짜 이유는 다른 데 있다. 플래그가 없거나 정렬이 안 맞으면 SPI
     * 드라이버가 **내부 SRAM 에 바운스 버퍼를 새로 잡고 매 전송마다 memcpy 한다**
     * (`spi_master.c:1191-1198`). 아끼려던 내부 SRAM 은 어차피 나가고 복사 비용만
     * 붙는다 — 할당은 성공하고 화면도 나오므로 **조용히 느려진다.** 그래서 여기서
     * 실패시키는 편이 낫다.
     *
     * 이것은 뒤집힌 통념 ④(mbedTLS 를 PSRAM 으로 보내면 하드웨어 AES 가 내부에
     * 바운스 버퍼를 잡는다)와 **같은 메커니즘**이다. PSRAM 으로 옮기면 어딘가에서
     * 바운스 버퍼가 나타난다. */
    MEM_INTENT_DMA,

    /* 크고, CPU 만 만지고, 수명이 길다. PSRAM 이 제자리다.
     * 내부 SRAM 에 두면 희소 자원을 통째로 먹는다. */
    MEM_INTENT_BULK,

    /* 작고 자주 접근한다. 내부 SRAM 을 선호하되 모자라면 PSRAM 도 받는다.
     * 캐시 미스로 느려질 뿐 동작은 한다. */
    MEM_INTENT_HOT,
} mem_intent_t;

/* what 은 로그 꼬리표. 배치가 의도대로 갔는지 한 줄 남긴다 —
 * 로드맵의 "의도 → 리전 매핑이 로그로 증명됨" 이 이 줄이다.
 * 실패 시 NULL. MEM_INTENT_DMA 는 내부 SRAM 이 모자라면 그냥 NULL 이다. */
void *mem_place_alloc(size_t size, mem_intent_t intent, const char *what);

void mem_place_free(void *ptr);

#ifdef __cplusplus
}
#endif
