#include "esp_check.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "lvgl.h"

#include "lcd_board.h"
#include "lcd_nv3041a.h"
#include "lvgl_port.h"
#include "mem_place.h"

static const char *TAG = "lvgl";

/* 드로우 버퍼 크기 — 이 프로젝트에서 가장 설계다운 결정.
 *
 * DMA 가 직접 읽으므로 내부 SRAM 이 물리적 강제다(PSRAM 은 DMA 가 못 닿는다).
 * 그런데 내부 SRAM 은 이 프로젝트가 아끼려는 바로 그 자원이다. 즉 화면을
 * 부드럽게 만들수록 관측 대상이 줄어든다. 트레이드오프가 실재한다.
 *
 * 부하 중 내부 연속 블록 실측이 188,416 이므로 20줄 x 2버퍼 = 38,400 은 그
 * 20% 다. 전체 프레임버퍼(480x272x2 = 261,120)를 내부에 두는 것은 애초에
 * 불가능하고, 그 사실이 이 구조의 출발점이었다. */
#define DRAW_BUF_PX     (NV3041A_H_RES * CONFIG_MHM_LCD_BUF_LINES)
#define DRAW_BUF_BYTES  (DRAW_BUF_PX * 2)

/* 계측 태스크(3)와 부하 태스크(4)보다 낮게 둔다. 화면이 계측을 밀어내면
 * 대시보드가 자기가 보여주는 숫자를 왜곡하게 된다. */
#define LVGL_TASK_PRIO  2
#define LVGL_STACK_BYTES 6144

/* mem_monitor 와 같은 이유로 정적이다 — 계측 대상인 힙을 건드리지 않는다.
 * 대가는 .bss 고정 증가이고, 그 값은 부팅 로그에 한 번 찍히면 끝이다. */
static StackType_t  s_stack[LVGL_STACK_BYTES];
static StaticTask_t s_tcb;
static TaskHandle_t s_task;

static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_lock_buf;

static lv_display_t *s_disp;

/* ISR 컨텍스트에서 불린다. `esp_lcd_panel_io_tx_color()` 가 큐에 넣고 바로
 * 리턴하므로, 버퍼를 다시 써도 되는 시점은 여기뿐이다. 이 콜백이 없으면
 * LVGL 이 아직 전송 중인 버퍼에 다음 프레임을 덮어쓴다. */
static bool on_color_trans_done(esp_lcd_panel_io_handle_t io,
                                esp_lcd_panel_io_event_data_t *edata,
                                void *user_ctx)
{
    (void)io;
    (void)edata;
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* LVGL 은 RGB565 를 호스트 바이트 순서(리틀엔디언)로 렌더링하는데 패널은
     * 빅엔디언으로 받는다. 이걸 빼먹으면 화면이 나오긴 하는데 색이 전부
     * 틀어진다 — "드라이버가 잘못됐나" 로 한참 헤매기 좋은 자리다. */
    lv_draw_rgb565_swap(px_map, lv_area_get_width(area) * lv_area_get_height(area));

    /* draw_bitmap 의 끝 좌표는 미포함(exclusive)이고 LVGL 의 area 는
     * 포함(inclusive)이라 +1 이 붙는다. */
    esp_lcd_panel_draw_bitmap(lcd_board_panel(),
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              px_map);

    /* 여기서 lv_display_flush_ready() 를 부르지 않는다. 전송이 아직 안 끝났다.
     * on_color_trans_done 이 대신 부른다. */
    (void)disp;
}

/* LVGL 에 시간을 알려주는 방법은 두 가지다 — 주기 타이머로 lv_tick_inc() 를
 * 때리거나, 지금처럼 "물어보면 답하는" 콜백을 주거나. 후자가 타이머 태스크를
 * 하나 아끼고, 이 프로젝트에서는 태스크 하나가 곧 스택 하나다. */
static uint32_t tick_get_cb(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool lvgl_port_lock(uint32_t timeout_ms)
{
    const TickType_t ticks = (timeout_ms == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTakeRecursive(s_lock, ticks) == pdTRUE;
}

void lvgl_port_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

static void lvgl_task(void *arg)
{
    (void)arg;

    for (;;) {
        uint32_t next_ms = CONFIG_MHM_UI_PERIOD_MS;

        if (lvgl_port_lock(0)) {
            next_ms = lv_timer_handler();
            lvgl_port_unlock();
        }

        /* lv_timer_handler() 가 LV_NO_TIMER_READY 를 돌려주면 할 일이 없다는
         * 뜻이다. 그대로 쓰면 아주 긴 값이라 화면 갱신 주기로 자른다. */
        if (next_ms > CONFIG_MHM_UI_PERIOD_MS) {
            next_ms = CONFIG_MHM_UI_PERIOD_MS;
        }
        if (next_ms < 5) {
            next_ms = 5;
        }
        vTaskDelay(pdMS_TO_TICKS(next_ms));
    }
}

esp_err_t lvgl_port_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateRecursiveMutexStatic(&s_lock_buf);
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "락 생성 실패");

    lv_init();
    lv_tick_set_cb(tick_get_cb);

    s_disp = lv_display_create(NV3041A_H_RES, NV3041A_V_RES);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "display 생성 실패");

    /* LCD 는 여기서 올린다. 완료 콜백이 s_disp 를 필요로 하므로 순서가 강제된다. */
    ESP_RETURN_ON_ERROR(lcd_board_init(on_color_trans_done, s_disp), TAG, "LCD 실패");

    void *buf1 = mem_place_alloc(DRAW_BUF_BYTES, MEM_INTENT_DMA, "lvgl_draw_buf1");
    ESP_RETURN_ON_FALSE(buf1, ESP_ERR_NO_MEM, TAG, "드로우 버퍼 1 실패");

    void *buf2 = NULL;
#if CONFIG_MHM_LCD_DOUBLE_BUFFER
    /* 두 번째 버퍼는 있으면 좋고 없어도 된다. DMA 가 앞 버퍼를 보내는 동안
     * LVGL 이 다음 조각을 그릴 수 있게 해줄 뿐이다. 내부 SRAM 이 빠듯하면
     * 여기를 먼저 포기하는 것이 맞다. */
    buf2 = mem_place_alloc(DRAW_BUF_BYTES, MEM_INTENT_DMA, "lvgl_draw_buf2");
    if (buf2 == NULL) {
        ESP_LOGW(TAG, "두 번째 드로우 버퍼를 못 잡았다 — 싱글 버퍼로 간다");
    }
#endif

    lv_display_set_buffers(s_disp, buf1, buf2, DRAW_BUF_BYTES,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(s_disp, flush_cb);

    s_task = xTaskCreateStatic(lvgl_task, "lvgl", LVGL_STACK_BYTES, NULL,
                               LVGL_TASK_PRIO, s_stack, &s_tcb);
    ESP_RETURN_ON_FALSE(s_task, ESP_FAIL, TAG, "LVGL 태스크 생성 실패");

    ESP_LOGI(TAG, "드로우 버퍼 %d줄 x %d = %d B x %d개 (정적 스택 %d B)",
             CONFIG_MHM_LCD_BUF_LINES, NV3041A_H_RES, DRAW_BUF_BYTES,
             buf2 ? 2 : 1, LVGL_STACK_BYTES);
    return ESP_OK;
}
