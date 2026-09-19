/* 메모리 대시보드 화면.
 *
 * 이 모듈은 **숫자를 만들지 않는다.** `heap_caps_get_info()` 도
 * `uxTaskGetSystemState()` 도 여기서 부르지 않는다. 둘 다 비싸고(전자는 힙
 * 순회 O(n), 후자는 미사용 스택 바이트에 비례), 화면 주기로 부르면 계측이
 * 관측 대상을 밀어낸다 — 이 프로젝트가 처음부터 경계해 온 관측자 효과다.
 *
 * 수집은 모니터 태스크가 하던 자리에서 그대로 하고, 여기는 받아서 그린다.
 * 그래서 아래 함수들은 전부 push 형태다. */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "mem_snapshot.h"
#include "task_stats.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 위젯을 만든다. 내부에서 LVGL 락을 잡으므로 아무 태스크에서나 불러도 된다. */
esp_err_t ui_dashboard_create(void);

/* 모니터 태스크가 매 주기 부른다. */
void ui_dashboard_push_heap(const mem_stats_t *s, uint32_t uptime_sec);

/* 모니터 태스크가 스택을 수집한 주기에만 부른다. */
void ui_dashboard_push_stacks(const task_stack_row_t *rows, size_t n, uint32_t us);

/* 헤더 오른쪽에 뜨는 한 줄. Wi-Fi 상태나 부하 on/off 같은 것. */
void ui_dashboard_set_status(const char *text);

#ifdef __cplusplus
}
#endif
