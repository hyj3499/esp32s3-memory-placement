#pragma once

#include "esp_err.h"

/* CONFIG_MHM_MONITOR_PERIOD_MS 주기로 힙 한 줄 요약을 찍는 태스크를 올린다.
 * 두 번 불러도 태스크는 하나만 생긴다.
 *
 * 멈추는 기능은 두지 않았다. 계측이 꺼지는 구간이 생기면 부하 그래프에 구멍이 난다. */
esp_err_t mem_monitor_start(void);
