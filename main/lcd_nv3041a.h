/* NV3041A (480x272, QSPI) 패널 드라이버.
 *
 * IDF 내장 드라이버가 아니고 컴포넌트 레지스트리에도 QSPI 판이 없다
 * (`eric-c-e/esp_lcd_nv3041` 은 4-wire SPI 전용). 그래서 직접 쓴다.
 * 골격은 espressif/esp_lcd_sh8601 · esp_lcd_spd2010 · esp_lcd_gc9b71 과 같다 —
 * QSPI 패널 드라이버끼리 다른 것은 초기화 커맨드 테이블뿐이다.
 *
 * 사용 순서:
 *   spi_bus_initialize()            data0~3_io_num 을 채운 버스
 *   esp_lcd_new_panel_io_spi()      flags.quad_mode = 1, lcd_cmd_bits = 32
 *   esp_lcd_new_panel_nv3041a()     여기
 *   esp_lcd_panel_reset/init/disp_on_off()
 */
#pragma once

#include "esp_err.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NV3041A_H_RES 480
#define NV3041A_V_RES 272

/* 패널 핸들 생성. io 는 반드시 quad_mode 로 만든 SPI 패널 IO 여야 한다.
 *
 * panel_dev_config 에서 이 드라이버가 보는 것:
 *   reset_gpio_num       RST. 이 보드는 미정의라 -1 이 될 수 있다
 *   flags.reset_active_high
 *   rgb_ele_order        RGB / BGR → MADCTL
 *   bits_per_pixel       16 (RGB565) 만 지원한다
 */
esp_err_t esp_lcd_new_panel_nv3041a(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
