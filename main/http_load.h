#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* HTTPS 요청을 반복해 TLS 스택에 부하를 주는 생성기.
 *
 * 트리거는 이 모듈의 관심사가 아니다. 지금은 app_main 이 부르고, Phase 4 에서
 * LVGL 버튼 콜백이 같은 함수를 부르면 된다. 그래서 start/stop 으로 끊어 뒀다.
 * 부하 모듈을 두 번 만들지 않기 위한 분리다. */
esp_err_t http_load_start(void);

/* 진행 중인 요청은 끝까지 보낸 뒤 멈춘다. 중간에 끊으면 그 요청이 잡은 메모리가
 * 해제되지 않은 채 남아 이후 측정이 오염된다. */
void http_load_stop(void);

bool http_load_is_running(void);
