/* LVGL 를 이 프로젝트에 붙이는 최소한의 글루.
 *
 * `espressif/esp_lvgl_port` 를 쓰지 않은 이유는 그 컴포넌트가 드로우 버퍼를
 * 자기가 `heap_caps_malloc` 으로 잡기 때문이다. 드로우 버퍼를 **누가 어디에**
 * 두는가가 Phase 4 의 본론이라 그 결정을 남에게 넘길 수 없다.
 * 여기서는 `mem_place_alloc(MEM_INTENT_DMA)` 를 거쳐 잡고, 실제로 내부 SRAM 에
 * 갔는지 `lcd_nv3041a.c` 의 draw_bitmap 이 포인터로 되묻어 증명한다. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* LCD 를 올리고, 드로우 버퍼를 잡고, LVGL 태스크를 띄운다.
 * 이 함수가 리턴하면 화면에 그릴 준비가 끝난 것이다. */
esp_err_t lvgl_port_start(void);

/* LVGL 은 스레드 안전하지 않다. 위젯을 만들거나 값을 바꾸는 코드는 전부
 * 이 락 안에서 돌아야 한다. `lv_timer_handler()` 도 같은 락을 쓴다.
 *
 * timeout_ms 가 0 이면 무한 대기. 실패하면 false. */
bool lvgl_port_lock(uint32_t timeout_ms);
void lvgl_port_unlock(void);

#ifdef __cplusplus
}
#endif
