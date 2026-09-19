/* NV3041A (480x272, QSPI) 패널 드라이버 — 골격.
 *
 * 채워야 할 곳은 TODO 로 표시했다. 나머지(구조체 배선, 팩토리, vtable 연결)는
 * st7789 등 IDF 내장 드라이버와 같은 형태다. */

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lcd_nv3041a.h"

static const char *TAG = "nv3041a";

/* ====================== QSPI 커맨드 프레이밍 ======================
 *
 * QSPI 는 "전부 4선" 이 아니다. `esp_lcd_panel_io_spi.c:398` 을 보면
 * `SPI_TRANS_MODE_QIO` 가 붙는 것은 **컬러 데이터 페이즈뿐**이고, 커맨드와
 * 파라미터는 1선으로 나간다. DC 핀도 없다(이 보드는 배선 자체가 없다).
 * 그래서 "지금 나가는 바이트가 커맨드냐 데이터냐"를 패널에 알릴 방법이
 * 프레임 안에 들어가야 한다 — 그것이 앞에 붙는 opcode 다.
 *
 * 보드 동봉 `Arduino_GFX-1.4.4/src/databus/Arduino_ESP32QSPI.cpp` 가 실제로
 * 쓰는 형식 (cmd 8bit + addr 24bit):
 *
 *   커맨드/파라미터  cmd=0x02, addr=(c << 8)      (:139, :236)
 *   픽셀 데이터      cmd=0x32, addr=0x003C00      (:197, :339)
 *
 * esp_lcd 에는 cmd/addr 페이즈가 따로 없으므로 둘을 합쳐 **32비트 커맨드 하나**로
 * 보낸다. 그래서 패널 IO 를 만들 때 `lcd_cmd_bits = 32` 여야 한다.
 * 바이트 순서는 `spi_lcd_prepare_cmd_buffer()`(`:180`)가 뒤집어 주므로
 * 여기서는 신경 쓰지 않는다. */
#define NV3041A_CMD(c)      (((uint32_t)0x02 << 24) | ((uint32_t)((c) & 0xff) << 8))
#define NV3041A_COLOR_CMD   (((uint32_t)0x32 << 24) | 0x003C00u)

/* ====================== 초기화 커맨드 테이블 ======================
 *
 * 출처: 보드 동봉 `latest initialization_4031A-01配IPS.docx`
 *       (`4-Driver_IC_Data_Sheet/4031A-01配IPS.docx` 도 같은 내용)
 *
 * 벤더 시퀀스는 `Write_Comm(c); Write_Data(d);` 96쌍이고 **파라미터가 전부
 * 1바이트**라 아래 구조체로 충분하다. 마지막 두 줄만 예외다 —
 * `Write_Comm(0x11)` 뒤에 `Delay_ms(120)`, 그리고 `Write_Comm(0x29)` 는
 * 파라미터가 없다. 그래서 `has_data` / `delay_ms` 가 필요하다. */
typedef struct {
    uint8_t cmd;
    uint8_t data;
    bool has_data;
    uint16_t delay_ms;
} nv3041a_init_cmd_t;

static const nv3041a_init_cmd_t s_init_cmds[] = {
    /* TODO(나): docx 의 96쌍을 옮긴다. 첫 줄은 0xff/0xa5 (커맨드 페이지 언락),
     *           마지막은 {0x11, .delay_ms=120} 과 {0x29} 다. */
};

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t madctl_val;
    uint8_t colmod_val;
} nv3041a_panel_t;

static esp_err_t panel_nv3041a_del(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041a_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041a_init(esp_lcd_panel_t *panel);
static esp_err_t panel_nv3041a_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data);
static esp_err_t panel_nv3041a_invert_color(esp_lcd_panel_t *panel, bool invert);
static esp_err_t panel_nv3041a_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_nv3041a_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_nv3041a_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
static esp_err_t panel_nv3041a_disp_on_off(esp_lcd_panel_t *panel, bool on_off);

esp_err_t esp_lcd_new_panel_nv3041a(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    nv3041a_panel_t *p = NULL;

    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG,
                        "invalid argument");
    ESP_RETURN_ON_FALSE(panel_dev_config->bits_per_pixel == 16, ESP_ERR_NOT_SUPPORTED, TAG,
                        "RGB565 만 지원한다 (bits_per_pixel=%" PRIu32 ")",
                        panel_dev_config->bits_per_pixel);

    p = calloc(1, sizeof(nv3041a_panel_t));
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "no mem for nv3041a panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        const gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "RST GPIO 설정 실패");
    }

    /* TODO(나): rgb_ele_order → madctl_val, colmod_val 결정.
     * MADCTL 은 0x36, COLMOD 는 0x3A 로 st7789 와 같은 MIPI DCS 다
     * (`Arduino_NV3041A.h:29,30`). BGR 비트는 `LCD_CMD_BGR_BIT`. */

    p->io = io;
    p->reset_gpio_num = panel_dev_config->reset_gpio_num;
    p->reset_level = panel_dev_config->flags.reset_active_high;

    p->base.del = panel_nv3041a_del;
    p->base.reset = panel_nv3041a_reset;
    p->base.init = panel_nv3041a_init;
    p->base.draw_bitmap = panel_nv3041a_draw_bitmap;
    p->base.invert_color = panel_nv3041a_invert_color;
    p->base.mirror = panel_nv3041a_mirror;
    p->base.swap_xy = panel_nv3041a_swap_xy;
    p->base.set_gap = panel_nv3041a_set_gap;
    p->base.disp_on_off = panel_nv3041a_disp_on_off;

    *ret_panel = &p->base;
    return ESP_OK;

err:
    free(p);
    return ret;
}

static esp_err_t panel_nv3041a_del(esp_lcd_panel_t *panel)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    if (p->reset_gpio_num >= 0) {
        gpio_reset_pin(p->reset_gpio_num);
    }
    free(p);
    return ESP_OK;
}

static esp_err_t panel_nv3041a_reset(esp_lcd_panel_t *panel)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    /* 이 보드는 RST 가 배선돼 있지 않다(`Arduino_GFX_dev_device.h:109`,
     * GFX_NOT_DEFINED). 그래서 -1 로 들어오고 소프트 리셋으로 가야 한다.
     * TODO(나): reset_gpio_num >= 0 이면 GPIO 토글, 아니면 SWRESET(0x01) +
     *           대기. 대기 시간은 벤더 헤더가 120ms 로 잡고 있다
     *           (`Arduino_NV3041A.h:10`). */
    (void)p;
    return ESP_OK;
}

static esp_err_t panel_nv3041a_init(esp_lcd_panel_t *panel)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    /* TODO(나): s_init_cmds 를 순회하며
     *   esp_lcd_panel_io_tx_param(p->io, NV3041A_CMD(c.cmd),
     *                             c.has_data ? &c.data : NULL,
     *                             c.has_data ? 1 : 0);
     * 그리고 c.delay_ms 만큼 vTaskDelay.
     *
     * MADCTL/COLMOD 는 벤더 테이블에도 들어 있다(0x36 은 없고 0x3A 가 0x01).
     * 테이블 값과 panel_dev_config 로 계산한 값이 충돌하므로, 어느 쪽을
     * 최종으로 둘 것인지 정해야 한다 — 테이블을 먼저 흘리고 뒤에서 덮는 쪽이
     * 흔하다. */
    (void)p;
    (void)s_init_cmds;  /* 위 TODO 를 채우면 같이 지운다 */
    return ESP_OK;
}

static esp_err_t panel_nv3041a_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    ESP_RETURN_ON_FALSE(x_start < x_end && y_start < y_end, ESP_ERR_INVALID_ARG, TAG,
                        "잘못된 범위");

    /* 이 프로젝트에서 이 함수가 갖는 의미가 하나 더 있다.
     * Phase 4 배치 래퍼의 **검증 지점**이 여기다 — 플러시 버퍼가 의도대로
     * 내부 SRAM 에서 왔는지를 `esp_ptr_external_ram(color_data)` 로 확인할 수
     * 있는 유일한 자리다. DMA 가 실제로 건드리는 포인터가 여기로 들어온다.
     * TODO(나): 부팅 후 첫 N 회만 로그를 남기는 식으로 넣는다. 매 프레임
     *           찍으면 그 자체가 관측자 효과가 된다(Phase 1~3 과 같은 함정). */

    /* TODO(나):
     *   1. x_gap / y_gap 더하기
     *   2. CASET(0x2A) ← {x>>8, x, xe>>8, xe}   tx_param
     *   3. RASET(0x2B) ← {y>>8, y, ye>>8, ye}   tx_param
     *   4. RAMWR(0x2C)                          tx_param, 파라미터 없음
     *   5. 픽셀                                  tx_color(p->io, NV3041A_COLOR_CMD, ...)
     *
     * 4번과 5번이 따로인 것이 QSPI 라서다. 벤더 라이브러리도 RAMWR 는 0x02
     * 경로로 보내고(`Arduino_NV3041A.cpp:69`), 픽셀 push 만 0x32/0x3C00 으로
     * 간다. 길이는 (x_end-x_start)*(y_end-y_start)*2 바이트.
     *
     * ⚠️ tx_color 는 **논블로킹**이다(`esp_lcd_panel_io_spi.c:404` queue_trans).
     * 리턴했다고 전송이 끝난 게 아니므로 color_data 버퍼를 바로 재사용하면 안
     * 된다. LVGL 에 flush_ready 를 알리는 시점은 IO 의 on_color_trans_done
     * 콜백이다. */
    (void)p;
    (void)color_data;
    return ESP_OK;
}

static esp_err_t panel_nv3041a_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    /* TODO(나): INVON(0x21) / INVOFF(0x20), 파라미터 없음. */
    (void)p;
    (void)invert;
    return ESP_OK;
}

static esp_err_t panel_nv3041a_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    /* TODO(나): madctl_val 의 MX(0x40)/MY(0x80) 비트를 손보고 MADCTL(0x36) 재전송.
     * 값을 구조체에 들고 있는 이유는 MADCTL 이 읽기 불가라서다 — mirror 와
     * swap_xy 가 같은 레지스터를 나눠 쓰므로 마지막 값을 기억해야 한다. */
    (void)p;
    (void)mirror_x;
    (void)mirror_y;
    return ESP_OK;
}

static esp_err_t panel_nv3041a_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    /* TODO(나): madctl_val 의 MV(0x20) 비트. */
    (void)p;
    (void)swap_axes;
    return ESP_OK;
}

static esp_err_t panel_nv3041a_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    p->x_gap = x_gap;
    p->y_gap = y_gap;
    return ESP_OK;
}

static esp_err_t panel_nv3041a_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    /* TODO(나): DISPON(0x29) / DISPOFF(0x28), 파라미터 없음. */
    (void)p;
    (void)on_off;
    return ESP_OK;
}
