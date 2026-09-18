#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "mem_snapshot.h"
#include "wifi_conn.h"

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT      (1 << 0)
#define WIFI_FAIL_BIT           (1 << 1)

/* rssi -83 dBm 환경에서 접속 시도부터 IP 획득까지 실측 28초. 30초로는 부족하다. */
#define WIFI_CONNECT_TIMEOUT_MS 60000

#define SCAN_REPORT_MAX         8

static EventGroupHandle_t s_wifi_events;
static int                s_retry_count;

/* NULL 을 넘기면 ESP_ERR_INVALID_ARG. 해제할 일이 없어도 받아둬야 한다. */
static esp_event_handler_instance_t s_wifi_evt_inst;
static esp_event_handler_instance_t s_ip_evt_inst;

static const char *authmode_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:          return "OPEN";
    case WIFI_AUTH_WEP:           return "WEP";
    case WIFI_AUTH_WPA_PSK:       return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:      return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:  return "WPA_WPA2_PSK";
    case WIFI_AUTH_ENTERPRISE:    return "ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:      return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
    case WIFI_AUTH_WAPI_PSK:      return "WAPI_PSK";
    case WIFI_AUTH_OWE:           return "OWE";
    default:                      return "기타";
    }
}

/* reason 코드만으로는 신호 문제와 보안 방식 불일치를 구분할 수 없다.
 * 버퍼를 정적으로 잡은 것은 진단 코드가 관측 대상인 힙을 건드리지 않게 하기 위함. */
static void scan_report(void)
{
    wifi_scan_config_t cfg = {
        .ssid        = (uint8_t *)CONFIG_MHM_WIFI_SSID,
        .show_hidden = true,
        .scan_type   = WIFI_SCAN_TYPE_ACTIVE,
    };

    ESP_LOGI(TAG, "'%s' 스캔 중...", CONFIG_MHM_WIFI_SSID);

    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "스캔 실패: %s", esp_err_to_name(err));
        return;
    }

    uint16_t found = 0;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_num(&found));
    if (found == 0) {
        ESP_LOGE(TAG, "같은 SSID 의 AP 가 하나도 안 보인다 (오타 또는 5GHz 전용)");
        return;
    }

    static wifi_ap_record_t records[SCAN_REPORT_MAX];
    uint16_t want = (found > SCAN_REPORT_MAX) ? SCAN_REPORT_MAX : found;
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&want, records));

    ESP_LOGI(TAG, "%u 개 발견:", found);
    for (uint16_t i = 0; i < want; i++) {
        const wifi_ap_record_t *r = &records[i];
        ESP_LOGI(TAG, "  " MACSTR "  ch=%2u  rssi=%4d dBm  auth=%s",
                 MAC2STR(r->bssid), r->primary, r->rssi, authmode_str(r->authmode));
    }
}

/* STA_START 에서 자동 접속하지 않는다. 접속 전에 스캔 진단을 돌려야 하므로
 * wifi_conn_start() 가 직접 esp_wifi_connect() 를 부른다. */
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;

        /* 201 NO_AP_FOUND / 2 AUTH_EXPIRE / 15, 205 비밀번호 불일치 / 4 비활성 */
        if (s_retry_count < CONFIG_MHM_WIFI_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGW(TAG, "연결 끊김 (reason=%d). 재시도 %d/%d",
                     d->reason, s_retry_count, CONFIG_MHM_WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "재시도 한도 초과 (reason=%d). 포기한다", d->reason);
            xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *evt = data;
        ESP_LOGI(TAG, "IP 획득: " IPSTR, IP2STR(&evt->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t init_nvs(void)
{
    esp_err_t err = nvs_flash_init();

    /* Wi-Fi 는 RF 캘리브레이션 값을 NVS 에 보관하므로 이 단계가 선행돼야 한다. */
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS 를 다시 만든다 (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t wifi_conn_start(void)
{
    if (strlen(CONFIG_MHM_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "SSID 가 비어 있다. idf.py menuconfig → Multi-heap Manager 에서 설정할 것");
        return ESP_ERR_INVALID_ARG;
    }

    ESP_ERROR_CHECK(init_nvs());

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 없으면 AP 에는 붙는데 IP 를 못 받는다. esp_wifi_* 는 링크 계층만 담당한다. */
    esp_netif_create_default_wifi_sta();

    s_wifi_events = xEventGroupCreate();
    if (s_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }

    mem_snapshot_log("B: esp_wifi_init 직전");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    mem_snapshot_log("C: esp_wifi_init 직후");

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, &s_wifi_evt_inst));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, &s_ip_evt_inst));

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid,
            CONFIG_MHM_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password,
            CONFIG_MHM_WIFI_PASSWORD, sizeof(wifi_config.sta.password));

    /* 기본값 FAST_SCAN 은 SSID 가 맞는 AP 를 처음 찾는 즉시 멈춘다. 중계기가 있으면
     * 약한 쪽을 골라 인증에 실패할 수 있어 전 채널을 훑고 신호 세기로 고르게 한다. */
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    mem_snapshot_log("D: esp_wifi_start 직후");

    /* 기본값 WIFI_PS_MIN_MODEM 은 비컨 사이에 라디오를 재운다. 약한 링크에서는
     * 비컨을 놓쳐 reason 4 (비활성으로 인한 결합 해제) 로 끊긴다. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    scan_report();

    ESP_LOGI(TAG, "'%s' 에 연결 시도", CONFIG_MHM_WIFI_SSID);
    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits == 0) {
        ESP_LOGE(TAG, "%d 초 안에 IP 를 받지 못했다", WIFI_CONNECT_TIMEOUT_MS / 1000);
        return ESP_ERR_TIMEOUT;
    }
    if ((bits & WIFI_CONNECTED_BIT) == 0) {
        return ESP_FAIL;
    }

    mem_snapshot_log("E: IP 획득 후");
    return ESP_OK;
}
