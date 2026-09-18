#pragma once

#include <stdbool.h>
#include <stdint.h>

/* heap tracing 래퍼. 차분법(스냅샷 간 뺄셈)은 총량까지가 한계이고, 여기서
 * 얻으려는 것은 할당 하나하나의 크기와 호출자다.
 *
 * CONFIG_HEAP_TRACING_STANDALONE 이 꺼진 빌드에서는 전부 빈 함수가 되므로
 * 호출부에 #if 를 두지 않아도 된다. */

/* seq 번째 요청을 추적할 차례인가. 부팅당 한 번만 true 를 돌려준다. */
bool heap_probe_should_trace(uint32_t seq);

/* 추적 시작. 요청 직전에 부른다. */
void heap_probe_begin(void);

/* 추적 중지 + 요약 + 내부 SRAM 할당 덤프. 요청 직후에 부른다. */
void heap_probe_end(void);
