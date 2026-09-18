#include "esp_err.h"
#include "esp_log.h"

#include "http_load.h"
#include "mem_monitor.h"
#include "mem_snapshot.h"
#include "wifi_conn.h"

static const char *TAG = "app";

void app_main(void)
{
    /* 측정 지점 B~E 는 각 단계 안쪽에 있어야 의미가 있어 해당 모듈이 직접 찍는다. */
    mem_snapshot_log("A: 부팅 직후 baseline");

    esp_err_t err = wifi_conn_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 연결 실패: %s", esp_err_to_name(err));
        return;
    }

    /* 부하보다 먼저 올린다. 부하 없는 상태의 줄이 몇 개 남아야 비교 기준이 생긴다. */
    ESP_ERROR_CHECK(mem_monitor_start());

    /* 지금은 app_main 이 트리거다. Phase 4 에서 LVGL 버튼이 같은 함수를 부른다. */
    ESP_ERROR_CHECK(http_load_start());

    /* 여기서 리턴해도 monitor / http_load 태스크는 계속 돈다.
     * main_task 만 종료되고 스케줄러는 살아 있다. */
    ESP_LOGI(TAG, "부하와 계측을 넘겼다. main_task 종료");
}
