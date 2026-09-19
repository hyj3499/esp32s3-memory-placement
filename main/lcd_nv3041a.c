/* NV3041A (480x272, QSPI) 패널 드라이버.
 *
 * IDF 내장 드라이버가 아니고 컴포넌트 레지스트리에도 QSPI 판이 없다
 * (`eric-c-e/esp_lcd_nv3041` 은 4-wire SPI 전용). 그래서 직접 썼다.
 * 구조는 IDF 내장 st7789 와 같고, 다른 것은 프레이밍과 초기화 테이블뿐이다. */

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

/* MADCTL(0x36) 비트. `Arduino_NV3041A.h:32-36`.
 * BGR 비트는 벤더 헤더에 없어 esp_lcd 의 표준 정의(LCD_CMD_BGR_BIT, 0x08)를 쓴다. */
#define NV3041A_MADCTL_MY   0x80
#define NV3041A_MADCTL_MX   0x40
#define NV3041A_MADCTL_MV   0x20

/* 이 패널의 네이티브 방향(480x272 가로)에서의 MADCTL 기본값.
 * `Arduino_NV3041A.cpp:41` 의 rotation 0 경로가 MX|MY 다. */
#define NV3041A_MADCTL_BASE (NV3041A_MADCTL_MX | NV3041A_MADCTL_MY)

/* ====================== 초기화 커맨드 테이블 ======================
 *
 * 출처: 보드 동봉 `latest initialization_4031A-01配IPS.docx`
 *       (= `4-Driver_IC_Data_Sheet/4031A-01配IPS.docx`). 손으로 옮기지 않고
 *       docx 를 파싱해 생성했다.
 *
 * `Write_Comm(c); Write_Data(d);` 96쌍이고 파라미터가 전부 1바이트다.
 * 예외는 둘뿐 — `0x11`(SLPOUT) 뒤에 120ms 대기, `0x29`(DISPON) 는 파라미터 없음.
 *
 * ⚠️ `0x3A` 는 표준 MIPI COLMOD 인코딩이 아니다. 벤더 init 배열의 주석이
 * `01---565, 00---666` 이라고 적고 있다. st7789 처럼 0x55 를 넣으면 안 된다.
 * 그래서 이 드라이버는 COLMOD 를 계산하지 않고 테이블 값을 그대로 쓴다. */
typedef struct {
    uint8_t cmd;
    uint8_t data;
    bool has_data;
    uint16_t delay_ms;
} nv3041a_init_cmd_t;

static const nv3041a_init_cmd_t s_init_cmds[] = {
    { 0xff, 0xa5, true,    0 },
    { 0xe7, 0x10, true,    0 },
    { 0x35, 0x01, true,    0 },
    { 0x3a, 0x01, true,    0 },
    { 0x40, 0x01, true,    0 },
    { 0x41, 0x03, true,    0 },
    { 0x44, 0x15, true,    0 },
    { 0x45, 0x15, true,    0 },
    { 0x7d, 0x03, true,    0 },
    { 0xc1, 0xab, true,    0 },
    { 0xc2, 0x17, true,    0 },
    { 0xc3, 0x10, true,    0 },
    { 0xc6, 0x3a, true,    0 },
    { 0xc7, 0x25, true,    0 },
    { 0xc8, 0x11, true,    0 },
    { 0x6f, 0x2f, true,    0 },
    { 0x78, 0x4b, true,    0 },
    { 0x7a, 0x49, true,    0 },
    { 0xc9, 0x00, true,    0 },
    { 0x51, 0x20, true,    0 },
    { 0x52, 0x7c, true,    0 },
    { 0x53, 0x1c, true,    0 },
    { 0x54, 0x77, true,    0 },
    { 0x46, 0x0a, true,    0 },
    { 0x47, 0x2a, true,    0 },
    { 0x48, 0x0a, true,    0 },
    { 0x49, 0x1a, true,    0 },
    { 0x56, 0x43, true,    0 },
    { 0x57, 0x42, true,    0 },
    { 0x58, 0x3c, true,    0 },
    { 0x59, 0x64, true,    0 },
    { 0x5a, 0x41, true,    0 },
    { 0x5b, 0x3c, true,    0 },
    { 0x5c, 0x02, true,    0 },
    { 0x5d, 0x3c, true,    0 },
    { 0x5e, 0x1f, true,    0 },
    { 0x60, 0x80, true,    0 },
    { 0x61, 0x3f, true,    0 },
    { 0x62, 0x21, true,    0 },
    { 0x63, 0x07, true,    0 },
    { 0x64, 0xe0, true,    0 },
    { 0x65, 0x01, true,    0 },
    { 0xca, 0x20, true,    0 },
    { 0xcb, 0x52, true,    0 },
    { 0xcc, 0x10, true,    0 },
    { 0xcd, 0x42, true,    0 },
    { 0xd0, 0x20, true,    0 },
    { 0xd1, 0x52, true,    0 },
    { 0xd2, 0x10, true,    0 },
    { 0xd3, 0x42, true,    0 },
    { 0xd4, 0x0a, true,    0 },
    { 0xd5, 0x32, true,    0 },
    { 0xe5, 0x05, true,    0 },
    { 0xe6, 0x00, true,    0 },
    { 0x6e, 0x14, true,    0 },
    { 0x80, 0x04, true,    0 },
    { 0xa0, 0x00, true,    0 },
    { 0x81, 0x07, true,    0 },
    { 0xa1, 0x05, true,    0 },
    { 0x82, 0x06, true,    0 },
    { 0xa2, 0x04, true,    0 },
    { 0x83, 0x39, true,    0 },
    { 0xa3, 0x39, true,    0 },
    { 0x84, 0x3a, true,    0 },
    { 0xa4, 0x3a, true,    0 },
    { 0x85, 0x3f, true,    0 },
    { 0xa5, 0x3f, true,    0 },
    { 0x86, 0x2c, true,    0 },
    { 0xa6, 0x2a, true,    0 },
    { 0x87, 0x43, true,    0 },
    { 0xa7, 0x47, true,    0 },
    { 0x88, 0x08, true,    0 },
    { 0xa8, 0x08, true,    0 },
    { 0x89, 0x0f, true,    0 },
    { 0xa9, 0x0f, true,    0 },
    { 0x8a, 0x17, true,    0 },
    { 0xaa, 0x17, true,    0 },
    { 0x8b, 0x10, true,    0 },
    { 0xab, 0x10, true,    0 },
    { 0x8c, 0x16, true,    0 },
    { 0xac, 0x16, true,    0 },
    { 0x8d, 0x14, true,    0 },
    { 0xad, 0x14, true,    0 },
    { 0x8e, 0x11, true,    0 },
    { 0xae, 0x11, true,    0 },
    { 0x8f, 0x14, true,    0 },
    { 0xaf, 0x14, true,    0 },
    { 0x90, 0x06, true,    0 },
    { 0xb0, 0x06, true,    0 },
    { 0x91, 0x0f, true,    0 },
    { 0xb1, 0x0f, true,    0 },
    { 0x92, 0x16, true,    0 },
    { 0xb2, 0x16, true,    0 },
    { 0xff, 0x00, true,    0 },
    { 0x11, 0x00, false, 120 },
    { 0x29, 0x00, false,   0 },
};

typedef struct {
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t madctl_val;   /* MADCTL 은 읽을 수 없다. mirror 와 swap_xy 가 같은
                           * 레지스터를 나눠 쓰므로 마지막 값을 들고 있어야 한다. */
    bool invert_on;
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

/* 파라미터 없는 커맨드 한 발. */
static esp_err_t tx_cmd(nv3041a_panel_t *p, uint8_t cmd)
{
    return esp_lcd_panel_io_tx_param(p->io, NV3041A_CMD(cmd), NULL, 0);
}

static esp_err_t tx_cmd8(nv3041a_panel_t *p, uint8_t cmd, uint8_t data)
{
    return esp_lcd_panel_io_tx_param(p->io, NV3041A_CMD(cmd), &data, 1);
}

esp_err_t esp_lcd_new_panel_nv3041a(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t *panel_dev_config,
                                    esp_lcd_panel_handle_t *ret_panel)
{
    esp_err_t ret = ESP_OK;
    nv3041a_panel_t *p = NULL;

    ESP_RETURN_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, TAG,
                        "invalid argument");
    /* COLMOD 는 테이블이 정하므로 여기서 바꿀 수 없다. draw_bitmap 이 픽셀당
     * 2바이트로 길이를 계산하기 때문에 16 이외는 그냥 막는다. */
    ESP_RETURN_ON_FALSE(panel_dev_config->bits_per_pixel == 16, ESP_ERR_NOT_SUPPORTED, TAG,
                        "RGB565 만 지원한다");

    p = calloc(1, sizeof(nv3041a_panel_t));
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "no mem for nv3041a panel");

    if (panel_dev_config->reset_gpio_num >= 0) {
        const gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "RST GPIO 설정 실패");
    }

    p->madctl_val = NV3041A_MADCTL_BASE;
    if (panel_dev_config->rgb_ele_order == LCD_RGB_ELEMENT_ORDER_BGR) {
        p->madctl_val |= LCD_CMD_BGR_BIT;
    }

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
    ESP_LOGI(TAG, "패널 생성 (RST=%d, MADCTL=0x%02x)", p->reset_gpio_num, p->madctl_val);
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

    if (p->reset_gpio_num >= 0) {
        gpio_set_level(p->reset_gpio_num, p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(p->reset_gpio_num, !p->reset_level);
        vTaskDelay(pdMS_TO_TICKS(NV3041A_RESET_DELAY_MS));
        return ESP_OK;
    }

    /* 이 보드는 RST 가 배선돼 있지 않다(`Arduino_GFX_dev_device.h:109`,
     * GFX_NOT_DEFINED). 소프트 리셋으로 간다. 대기 시간은 벤더 헤더가 잡은
     * 값이다(`Arduino_NV3041A.h:10`). */
    ESP_RETURN_ON_ERROR(tx_cmd(p, LCD_CMD_SWRESET), TAG, "SWRESET 실패");
    vTaskDelay(pdMS_TO_TICKS(NV3041A_RESET_DELAY_MS));
    return ESP_OK;
}

static esp_err_t panel_nv3041a_init(esp_lcd_panel_t *panel)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        const nv3041a_init_cmd_t *c = &s_init_cmds[i];

        ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, NV3041A_CMD(c->cmd),
                                                      c->has_data ? &c->data : NULL,
                                                      c->has_data ? 1 : 0),
                            TAG, "초기화 커맨드 0x%02x 실패", c->cmd);
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }

    /* MADCTL 은 벤더 테이블에 없다. 방향/색순서는 이쪽이 관리하므로 뒤에서 넣는다.
     * COLMOD(0x3A)는 반대로 테이블이 정한다 — 위 주석 참고. */
    ESP_RETURN_ON_ERROR(tx_cmd8(p, LCD_CMD_MADCTL, p->madctl_val), TAG, "MADCTL 실패");

    ESP_LOGI(TAG, "초기화 완료 (%u 커맨드)",
             (unsigned)(sizeof(s_init_cmds) / sizeof(s_init_cmds[0])));
    return ESP_OK;
}

static esp_err_t panel_nv3041a_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start,
                                           int x_end, int y_end, const void *color_data)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    ESP_RETURN_ON_FALSE(x_start < x_end && y_start < y_end, ESP_ERR_INVALID_ARG, TAG,
                        "잘못된 범위");

    x_start += p->x_gap;
    x_end   += p->x_gap;
    y_start += p->y_gap;
    y_end   += p->y_gap;

    /* 이 프로젝트에서 이 함수가 갖는 의미가 하나 더 있다. DMA 가 실제로 건드리는
     * 포인터가 여기로 들어오므로, 플러시 버퍼가 의도대로 내부 SRAM 에서 왔는지
     * 확인할 수 있는 유일한 자리다 — Phase 4 배치 래퍼의 검증 지점.
     *
     * 매 프레임 찍으면 그 자체가 관측자 효과가 되므로 처음 몇 번만 남긴다. */
    static int s_placement_logs = 3;
    if (s_placement_logs > 0) {
        s_placement_logs--;
        ESP_LOGI(TAG, "flush 버퍼 %p → %s (%dx%d)",
                 color_data,
                 esp_ptr_external_ram(color_data) ? "PSRAM (DMA 불가!)" : "내부 SRAM",
                 x_end - x_start, y_end - y_start);
    }

    const uint8_t caset[] = {
        (uint8_t)(x_start >> 8), (uint8_t)x_start,
        (uint8_t)((x_end - 1) >> 8), (uint8_t)(x_end - 1),
    };
    const uint8_t raset[] = {
        (uint8_t)(y_start >> 8), (uint8_t)y_start,
        (uint8_t)((y_end - 1) >> 8), (uint8_t)(y_end - 1),
    };

    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, NV3041A_CMD(LCD_CMD_CASET),
                                                  caset, sizeof(caset)), TAG, "CASET 실패");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_io_tx_param(p->io, NV3041A_CMD(LCD_CMD_RASET),
                                                  raset, sizeof(raset)), TAG, "RASET 실패");
    /* 벤더 라이브러리도 RAMWR 는 0x02 경로로 보내고(`Arduino_NV3041A.cpp:69`)
     * 픽셀 push 만 0x32/0x3C00 으로 간다. 그래서 두 번 나간다. */
    ESP_RETURN_ON_ERROR(tx_cmd(p, LCD_CMD_RAMWR), TAG, "RAMWR 실패");

    const size_t len = (size_t)(x_end - x_start) * (size_t)(y_end - y_start) * 2;

    /* 논블로킹이다(`esp_lcd_panel_io_spi.c:404`, queue_trans). 리턴했다고 전송이
     * 끝난 게 아니므로 color_data 를 바로 재사용하면 안 된다. 완료는 IO 의
     * on_color_trans_done 콜백으로 온다 — lcd_board.c 가 그것으로 LVGL 에
     * flush_ready 를 알린다. */
    return esp_lcd_panel_io_tx_color(p->io, NV3041A_COLOR_CMD, color_data, len);
}

static esp_err_t panel_nv3041a_invert_color(esp_lcd_panel_t *panel, bool invert)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    p->invert_on = invert;
    return tx_cmd(p, invert ? LCD_CMD_INVON : LCD_CMD_INVOFF);
}

static esp_err_t panel_nv3041a_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    /* 네이티브 방향이 이미 MX|MY 라(`NV3041A_MADCTL_BASE`) "미러" 는 기본값을
     * 뒤집는 것이 된다. */
    if (mirror_x) {
        p->madctl_val &= (uint8_t)~NV3041A_MADCTL_MX;
    } else {
        p->madctl_val |= NV3041A_MADCTL_MX;
    }
    if (mirror_y) {
        p->madctl_val &= (uint8_t)~NV3041A_MADCTL_MY;
    } else {
        p->madctl_val |= NV3041A_MADCTL_MY;
    }
    return tx_cmd8(p, LCD_CMD_MADCTL, p->madctl_val);
}

static esp_err_t panel_nv3041a_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    nv3041a_panel_t *p = __containerof(panel, nv3041a_panel_t, base);

    if (swap_axes) {
        p->madctl_val |= NV3041A_MADCTL_MV;
    } else {
        p->madctl_val &= (uint8_t)~NV3041A_MADCTL_MV;
    }
    return tx_cmd8(p, LCD_CMD_MADCTL, p->madctl_val);
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

    return tx_cmd(p, on_off ? LCD_CMD_DISPON : LCD_CMD_DISPOFF);
}
