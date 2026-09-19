#include "esp_err.h"
#include "esp_log.h"

#include "http_load.h"
#include "lvgl_port.h"
#include "mem_monitor.h"
#include "mem_snapshot.h"
#include "task_stats.h"
#include "ui_dashboard.h"
#include "wifi_conn.h"

static const char *TAG = "app";

void app_main(void)
{
    /* 측정 지점 B~E 는 각 단계 안쪽에 있어야 의미가 있어 해당 모듈이 직접 찍는다.
     *
     * A 는 무엇보다 먼저다. LCD 드로우 버퍼도 내부 SRAM 을 먹으므로, 그 전에
     * 찍지 않으면 "칩이 준 것" 과 "대시보드가 가져간 것" 이 섞인다. */
    mem_snapshot_log("A: 부팅 직후 baseline");

    /* 계측의 정적 비용을 A 지점 옆에 남긴다. 나중에 힙 숫자가 흔들렸을 때
     * "계측이 먹은 것" 과 "관측 대상이 먹은 것" 을 분리하려면 이 줄이 필요하다. */
    task_stats_log_cost();

    /* Wi-Fi 보다 먼저 올린다. 이유가 둘이다.
     *
     * ① 화면이 부팅 과정을 보여줄 수 있다. Wi-Fi 연결은 몇 초 걸리고 실패도
     *    하는데, 그동안 까만 화면이면 "죽은 건지 붙는 중인지" 를 알 수 없다.
     * ② 배치 검증이 깨끗해진다. 드로우 버퍼를 Wi-Fi 뒤에 잡으면 내부 SRAM 이
     *    이미 조각나 있어, DMA 할당 실패가 "정책이 틀려서" 인지 "자리가 없어서"
     *    인지 구분되지 않는다.
     *
     * 대신 A→C/E 의 Wi-Fi 구동 비용이 달라질 수 있다는 물음이 생긴다.
     * Phase 2 의 결론이 "정적 비용과 동적 비용은 독립"(빌드 3개에서 재확인)
     * 이므로 **48,312 / 51,584 가 그대로 나와야 한다.** 안 그러면 그 결론이
     * 틀렸던 것이고, 그것도 결과다. */
    esp_err_t err = lvgl_port_start();
    if (err != ESP_OK) {
        /* 화면이 없어도 프로젝트의 본체인 계측은 돌아야 한다. 시리얼 로그가
         * 원래의 산출물이고 대시보드는 그 위에 얹은 표현이다. */
        ESP_LOGE(TAG, "LCD/LVGL 실패: %s — 시리얼만으로 계속한다", esp_err_to_name(err));
    } else {
        ESP_ERROR_CHECK(ui_dashboard_create());
        ui_dashboard_set_status("wifi...");
        mem_snapshot_log("A2: LCD/LVGL 기동 후");
    }

    err = wifi_conn_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 연결 실패: %s", esp_err_to_name(err));
        ui_dashboard_set_status("wifi FAIL");
        return;
    }
    ui_dashboard_set_status("idle");

    /* 부하보다 먼저 올린다. 부하 없는 상태의 줄이 몇 개 남아야 비교 기준이 생긴다. */
    ESP_ERROR_CHECK(mem_monitor_start());

    /* 지금은 app_main 이 트리거다. 버튼을 달면 같은 함수를 부르면 된다. */
    ESP_ERROR_CHECK(http_load_start());
    ui_dashboard_set_status("HTTPS load");

    /* 여기서 리턴해도 monitor / http_load / lvgl 태스크는 계속 돈다.
     * main_task 만 종료되고 스케줄러는 살아 있다. */
    ESP_LOGI(TAG, "부하와 계측을 넘겼다. main_task 종료");
}
