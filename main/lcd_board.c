#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "lcd_board.h"
#include "lcd_nv3041a.h"

static const char *TAG = "lcd_board";

#define LCD_HOST            SPI2_HOST

#define BL_LEDC_TIMER       LEDC_TIMER_0
#define BL_LEDC_CHANNEL     LEDC_CHANNEL_0
#define BL_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define BL_LEDC_RES         LEDC_TIMER_10_BIT
#define BL_LEDC_FREQ_HZ     5000

/* 한 번에 나갈 수 있는 최대 전송. 드로우 버퍼가 통째로 한 트랜잭션에 실리도록
 * 잡는다. 모자라면 esp_lcd 가 쪼개서 보내긴 하지만(`:381`), 쪼갤 때마다
 * CS 를 유지한 채 트랜잭션이 하나 더 생겨 오버헤드가 붙는다. */
#define LCD_MAX_TRANSFER_SZ (NV3041A_H_RES * CONFIG_MHM_LCD_BUF_LINES * 2 + 8)

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;

static esp_err_t backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = BL_LEDC_MODE,
        .timer_num = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        .freq_hz = BL_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "LEDC 타이머 실패");

    const ledc_channel_config_t ch = {
        .gpio_num = LCD_PIN_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel = BL_LEDC_CHANNEL,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    return ledc_channel_config(&ch);
}

esp_err_t lcd_board_backlight_set(int percent)
{
    if (percent < 0) {
        percent = 0;
    } else if (percent > 100) {
        percent = 100;
    }

    const uint32_t max_duty = (1u << BL_LEDC_RES) - 1;
    const uint32_t duty = (max_duty * (uint32_t)percent) / 100u;

    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty), TAG, "duty 실패");
    return ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
}

esp_err_t lcd_board_init(esp_lcd_panel_io_color_trans_done_cb_t done_cb, void *user_ctx)
{
    if (s_panel != NULL) {
        return ESP_OK;
    }

    /* QSPI 버스. data0~3 은 mosi/miso/quadwp/quadhd 와 같은 union 이라
     * 이름만 다르고 자리는 같다. 네 줄을 다 채우는 것이 QSPI 다. */
    const spi_bus_config_t buscfg = {
        .sclk_io_num = LCD_PIN_SCK,
        .data0_io_num = LCD_PIN_D0,
        .data1_io_num = LCD_PIN_D1,
        .data2_io_num = LCD_PIN_D2,
        .data3_io_num = LCD_PIN_D3,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
        .max_transfer_sz = LCD_MAX_TRANSFER_SZ,
        .flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI 버스 초기화 실패");

    /* lcd_cmd_bits = 32 가 이 설정의 핵심이다. NV3041A 의 QSPI 프레임은
     * opcode(8) + 주소(24) 인데 esp_lcd 에는 주소 페이즈가 없어서, 둘을 합친
     * 32비트를 "커맨드" 로 밀어 넣는다. lcd_nv3041a.c 의 NV3041A_CMD() 참고.
     *
     * dc_gpio_num 이 -1 인 것은 이 보드에 DC 배선이 없기 때문이고, 바로 그래서
     * 위의 opcode 프레이밍이 필요하다. */
    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_PIN_CS,
        .dc_gpio_num = -1,
        .spi_mode = 0,
        .pclk_hz = CONFIG_MHM_LCD_PCLK_MHZ * 1000 * 1000,
        .trans_queue_depth = 10,
        .lcd_cmd_bits = 32,
        .lcd_param_bits = 8,
        .on_color_trans_done = done_cb,
        .user_ctx = user_ctx,
        .flags = {
            .quad_mode = true,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST,
                                                 &io_config, &s_io),
                        TAG, "패널 IO 생성 실패");

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_nv3041a(s_io, &panel_config, &s_panel),
                        TAG, "패널 생성 실패");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "리셋 실패");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "초기화 실패");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "DISPON 실패");

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "백라이트 초기화 실패");
    ESP_RETURN_ON_ERROR(lcd_board_backlight_set(CONFIG_MHM_LCD_BACKLIGHT_PCT),
                        TAG, "백라이트 설정 실패");

    ESP_LOGI(TAG, "LCD %dx%d, QSPI %d MHz, 최대 전송 %d B",
             NV3041A_H_RES, NV3041A_V_RES, CONFIG_MHM_LCD_PCLK_MHZ, LCD_MAX_TRANSFER_SZ);
    return ESP_OK;
}

esp_lcd_panel_handle_t lcd_board_panel(void)
{
    return s_panel;
}
