#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "lvgl.h"

#include "lcd_nv3041a.h"
#include "lvgl_port.h"
#include "ui_dashboard.h"

static const char *TAG = "ui";

/* 화면 분할 (480x272).
 *
 *   0   헤더                                        24
 *   26  ┌ INTERNAL SRAM 수치+막대 ┬ 시계열 차트 ┐  148
 *   152 └ 스택 여유 표                           ┘  272
 */
#define PAD             4
#define HEADER_H        24
#define MID_Y           (HEADER_H + PAD)
#define MID_H           118
#define LEFT_W          186
#define CHART_X         (LEFT_W + PAD * 2)
#define CHART_W         (NV3041A_H_RES - CHART_X - PAD)
#define BOTTOM_Y        (MID_Y + MID_H + PAD)
#define BOTTOM_H        (NV3041A_V_RES - BOTTOM_Y - PAD)

/* 5분치. 모니터 주기가 5초면 60점이 300초다. */
#define CHART_POINTS    60

/* 화면에 올릴 태스크 줄 수. 남은 스택이 적은 순으로 정렬돼 오므로
 * 앞에서 잘라도 위험한 것부터 보인다. */
#define STACK_ROWS      8

static lv_obj_t *s_lbl_status;
static lv_obj_t *s_lbl_uptime;
static lv_obj_t *s_lbl_int;
static lv_obj_t *s_lbl_psram;
static lv_obj_t *s_bar_free;
static lv_obj_t *s_bar_largest;
static lv_obj_t *s_chart;
static lv_chart_series_t *s_ser_free;
static lv_chart_series_t *s_ser_largest;
static lv_obj_t *s_lbl_stacks;

/* 막대와 차트의 100% 기준. 부팅 직후 free 를 쓴다.
 *
 * 절대 용량(힙 풀 크기)이 아니라 "부팅 직후" 인 것이 의도다. 이 프로젝트가
 * 보려는 것은 칩이 가진 양이 아니라 **Wi-Fi 와 부하가 그 다음에 얼마나
 * 가져가는가** 이기 때문이다. 0 은 첫 push 에서 채워진다. */
static uint32_t s_ref_free;

static lv_obj_t *make_panel(lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *p = lv_obj_create(lv_screen_active());

    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x161b22), 0);
    lv_obj_set_style_border_color(p, lv_color_hex(0x30363d), 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_radius(p, 3, 0);
    lv_obj_set_style_pad_all(p, 4, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static lv_obj_t *make_label(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);

    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static void style_bar(lv_obj_t *bar, uint32_t color)
{
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x30363d), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, 1, LV_PART_INDICATOR);
}

esp_err_t ui_dashboard_create(void)
{
    if (!lvgl_port_lock(0)) {
        return ESP_FAIL;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0d1117), 0);
    lv_obj_set_style_text_font(scr, &lv_font_montserrat_14, 0);

    /* ---- 헤더 ---- */
    lv_obj_t *head = lv_obj_create(scr);
    lv_obj_set_pos(head, 0, 0);
    lv_obj_set_size(head, NV3041A_H_RES, HEADER_H);
    lv_obj_set_style_bg_color(head, lv_color_hex(0x1f6feb), 0);
    lv_obj_set_style_border_width(head, 0, 0);
    lv_obj_set_style_radius(head, 0, 0);
    lv_obj_set_style_pad_all(head, 2, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = make_label(head, 4, 0, 0xffffff);
    lv_label_set_text(title, "MULTI-HEAP MANAGER");

    s_lbl_status = make_label(head, 172, 0, 0xc9d1d9);
    lv_label_set_text(s_lbl_status, "booting");

    s_lbl_uptime = make_label(head, 396, 0, 0xffffff);
    lv_label_set_text(s_lbl_uptime, "up 0s");

    /* ---- 왼쪽: 내부 SRAM 수치 + 막대 ---- */
    lv_obj_t *left = make_panel(PAD, MID_Y, LEFT_W, MID_H);

    lv_obj_t *cap = make_label(left, 0, 0, 0x58a6ff);
    lv_label_set_text(cap, "INTERNAL SRAM");

    /* 패널 안쪽 높이는 MID_H(118) - pad*2 = 110px 다. montserrat_14 한 줄이
     * 17px 이므로 캡션 17 + 본문 3줄 51 + 막대 2개 20 + 아래 한 줄 17 = 105 로
     * 겨우 들어간다.
     *
     * 처음에는 본문을 4줄(free/largest/min_free/frag)로 잡았는데, 그러면 본문이
     * y=18~86 을 쓰고 막대가 y=70/82 에 놓여 **frag 줄을 덮는다.** 실기에서
     * 그렇게 나왔다. frag 는 아래 PSRAM 줄로 합쳐 내렸다. */
    s_lbl_int = make_label(left, 0, 18, 0xc9d1d9);

    s_bar_free = lv_bar_create(left);
    lv_obj_set_pos(s_bar_free, 0, 70);
    lv_obj_set_size(s_bar_free, LEFT_W - 16, 8);
    style_bar(s_bar_free, 0x3fb950);

    s_bar_largest = lv_bar_create(left);
    lv_obj_set_pos(s_bar_largest, 0, 80);
    lv_obj_set_size(s_bar_largest, LEFT_W - 16, 8);
    style_bar(s_bar_largest, 0xd29922);

    s_lbl_psram = make_label(left, 0, 90, 0x8b949e);

    /* ---- 오른쪽: 시계열 ----
     *
     * free 와 largest 를 같이 그리는 것이 요점이다. 둘이 벌어지는 폭이 곧
     * 단편화다 — "free 는 넉넉한데 largest 가 주저앉는" 상황이 한눈에 보인다.
     * 이 프로젝트가 단편화율 공식을 믿지 않기로 한 뒤(STUDY.md §7) 남은,
     * 눈으로 읽을 수 있는 표현이다. */
    lv_obj_t *right = make_panel(CHART_X, MID_Y, CHART_W, MID_H);

    lv_obj_t *cap2 = make_label(right, 0, 0, 0x58a6ff);
    lv_label_set_text(cap2, "free / largest");

    s_chart = lv_chart_create(right);
    lv_obj_set_pos(s_chart, 0, 18);
    lv_obj_set_size(s_chart, CHART_W - 16, MID_H - 32);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, CHART_POINTS);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_div_line_count(s_chart, 4, 0);
    lv_obj_set_style_bg_color(s_chart, lv_color_hex(0x0d1117), 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_line_color(s_chart, lv_color_hex(0x30363d), LV_PART_MAIN);
    /* 점 마커 끄기.
     *
     * 크기를 0 으로 주는 것으로는 안 꺼진다. `lv_chart.c:1238` 이
     * `point_w = lv_obj_get_style_width(obj, LV_PART_INDICATOR) / 2` 로 반지름을
     * 잡으므로 0 이면 반지름 0, 즉 **1픽셀 사각형**이 점마다 그대로 찍힌다.
     * 게다가 LV_PART_INDICATOR 의 기본 배경색이 흰색이라 어두운 차트 위에
     * 흰 점이 흩뿌려진 것처럼 보인다. 실기에서 그렇게 나왔다.
     * 크기가 아니라 **불투명도**로 꺼야 한다. */
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_size(s_chart, 0, 0, LV_PART_INDICATOR);

    s_ser_free = lv_chart_add_series(s_chart, lv_color_hex(0x3fb950),
                                     LV_CHART_AXIS_PRIMARY_Y);
    s_ser_largest = lv_chart_add_series(s_chart, lv_color_hex(0xd29922),
                                        LV_CHART_AXIS_PRIMARY_Y);

    /* ---- 아래: 스택 여유 ---- */
    lv_obj_t *bottom = make_panel(PAD, BOTTOM_Y, NV3041A_H_RES - PAD * 2, BOTTOM_H);

    lv_obj_t *cap3 = make_label(bottom, 0, 0, 0x58a6ff);
    lv_label_set_text(cap3, "STACK HEADROOM (bytes)");

    s_lbl_stacks = make_label(bottom, 0, 18, 0xc9d1d9);
    lv_label_set_text(s_lbl_stacks, "수집 대기");

    lvgl_port_unlock();
    ESP_LOGI(TAG, "대시보드 생성 완료");
    return ESP_OK;
}

void ui_dashboard_push_heap(const mem_stats_t *s, uint32_t uptime_sec)
{
    if (s_lbl_int == NULL) {
        return;
    }
    if (s_ref_free == 0) {
        s_ref_free = (uint32_t)s->internal_free;
        if (s_ref_free == 0) {
            s_ref_free = 1;
        }
    }

    char buf[192];

    if (!lvgl_port_lock(100)) {
        /* 락을 못 잡으면 이번 갱신을 버린다. 모니터 태스크를 붙잡고 있으면
         * 계측 주기가 흔들리고, 그러면 시계열이 거짓말을 한다. */
        ESP_LOGW(TAG, "LVGL 락 실패 — 이번 갱신 생략");
        return;
    }

    snprintf(buf, sizeof(buf),
             "free     %7u\n"
             "largest  %7u\n"
             "min_free %7u",
             (unsigned)s->internal_free, (unsigned)s->internal_largest,
             (unsigned)s->internal_min_free);
    lv_label_set_text(s_lbl_int, buf);

    snprintf(buf, sizeof(buf), "frag %u%%   PSRAM %u",
             s->internal_frag, (unsigned)s->psram_free);
    lv_label_set_text(s_lbl_psram, buf);

    lv_bar_set_value(s_bar_free,
                     (int32_t)((s->internal_free * 100u) / s_ref_free), LV_ANIM_OFF);
    lv_bar_set_value(s_bar_largest,
                     (int32_t)((s->internal_largest * 100u) / s_ref_free), LV_ANIM_OFF);

    /* 차트 세로축을 0 부터 두지 않는다. 이 프로젝트에서 의미 있는 변화폭은
     * 수십 KB 인데 0~300KB 축에 올리면 선이 거의 평평해 보인다. */
    lv_chart_set_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 0, (int32_t)s_ref_free);
    lv_chart_set_next_value(s_chart, s_ser_free, (int32_t)s->internal_free);
    lv_chart_set_next_value(s_chart, s_ser_largest, (int32_t)s->internal_largest);

    snprintf(buf, sizeof(buf), "up %us", (unsigned)uptime_sec);
    lv_label_set_text(s_lbl_uptime, buf);

    lvgl_port_unlock();
}

void ui_dashboard_push_stacks(const task_stack_row_t *rows, size_t n, uint32_t us)
{
    if (s_lbl_stacks == NULL || n == 0) {
        return;
    }
    if (n > STACK_ROWS) {
        n = STACK_ROWS;
    }

    /* 한 줄에 둘씩 넣는다. 480px 에 14pt 로 두 칸이 들어가고, 8개를 4줄로
     * 줄이면 아래 패널(약 100px)에 딱 맞는다. */
    char buf[256];
    int off = 0;

    for (size_t i = 0; i < n && off < (int)sizeof(buf) - 1; i++) {
        off += snprintf(buf + off, sizeof(buf) - off, "%-10s %5u%s",
                        rows[i].name, (unsigned)rows[i].headroom,
                        (i % 2 == 1) ? "\n" : "   ");
    }

    if (!lvgl_port_lock(100)) {
        ESP_LOGW(TAG, "LVGL 락 실패 — 스택 표 생략");
        return;
    }
    lv_label_set_text(s_lbl_stacks, buf);
    lvgl_port_unlock();

    /* 수집 비용 자체가 측정값이라 헤더에 남긴다 — README 뒤집힌 통념 ⑦. */
    ESP_LOGD(TAG, "스택 표 갱신 (%u줄, 수집 %uus)", (unsigned)n, (unsigned)us);
}

void ui_dashboard_set_status(const char *text)
{
    if (s_lbl_status == NULL || text == NULL) {
        return;
    }
    if (!lvgl_port_lock(100)) {
        return;
    }
    lv_label_set_text(s_lbl_status, text);
    lvgl_port_unlock();
}
