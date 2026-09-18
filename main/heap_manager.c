#include "esp_err.h"
#include "esp_log.h"

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

    ESP_LOGI(TAG, "Wi-Fi 준비 완료. 다음은 HTTPS 부하.");
}
