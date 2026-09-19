/* JC4827W543 보드의 LCD 결선을 아는 유일한 파일.
 *
 * 칩 드라이버(`lcd_nv3041a.c`)와 분리한 이유는, 같은 NV3041A 라도 보드마다
 * 핀·클럭·백라이트가 다르기 때문이다. 드라이버는 패널을, 여기는 기판을 안다. */
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 핀맵 ----
 * 출처: 보드 동봉 `Arduino_GFX_dev_device.h:104-109`, 활성화된
 * `#define ESP32_4827A043_QSPI` 블록. 서드파티 레포 2곳과도 일치한다. */
#define LCD_PIN_CS   45
#define LCD_PIN_SCK  47
#define LCD_PIN_D0   21
#define LCD_PIN_D1   48
#define LCD_PIN_D2   40
#define LCD_PIN_D3   39
#define LCD_PIN_BL    1
#define LCD_PIN_RST  -1   /* 배선 자체가 없다. 리셋은 SWRESET 로 간다 */

/* SPI2_HOST / mode 0 — 벤더 데모와 같다 (`Arduino_ESP32QSPI.h:10,11`). */

/* 버스 → 패널 IO → 패널 순으로 올리고 백라이트를 켠다.
 *
 * done_cb 는 컬러 전송이 끝났을 때 불린다. `esp_lcd_panel_io_tx_color()` 가
 * 논블로킹이라 이 콜백 없이는 버퍼를 언제 재사용해도 되는지 알 수 없다.
 * ⚠️ ISR 컨텍스트에서 불린다 — 안에서 블로킹하면 안 된다. */
esp_err_t lcd_board_init(esp_lcd_panel_io_color_trans_done_cb_t done_cb, void *user_ctx);

esp_lcd_panel_handle_t lcd_board_panel(void);

/* LEDC PWM. 0 이면 끈다. */
esp_err_t lcd_board_backlight_set(int percent);

#ifdef __cplusplus
}
#endif
