#pragma once

#include "esp_err.h"

/* STA 모드로 올리고 IP 를 받을 때까지 블로킹한다.
 * 초기화 단계마다 mem_snapshot_log() 를 찍는 것이 이 함수의 또 다른 목적이라
 * IDF 예제의 example_connect() 헬퍼를 쓰지 않는다. */
esp_err_t wifi_conn_start(void);
