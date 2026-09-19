#pragma once

#include <stddef.h>
#include <stdint.h>

/* 태스크별 스택 워터마크. 힙의 min_free 와 같은 성질의 값이다 — 샘플링이 아니라
 * 생성 이후의 누적 최저치라 두 호출 사이의 저점을 놓치지 않는다. 대신 리셋이
 * 안 되므로 "언제 그랬는지" 는 영영 알 수 없다.
 *
 * 태스크 컨텍스트에서만 부를 것. 내부적으로 커널 락을 잡는다. */

/* label 은 로그 꼬리표. 수집에 걸린 시간(us)도 같이 찍는다 — 이 함수 자체가
 * 관측 대상을 밀어내는 비용이라 그것도 측정값이다. */
void task_stats_log(const char *label);

/* 정적 배열이 .bss 에서 차지하는 크기를 한 번 찍는다. 부팅 직후 호출용. */
void task_stats_log_cost(void);

/* ---- 화면용 값 꺼내기 ----
 *
 * 수집 비용이 미사용 스택 바이트에 비례한다(README 뒤집힌 통념 ⑦,
 * `수집시간(us) = 27 + 미사용스택합 / 22.9`). 그래서 화면 주기로 부르면 안 되고,
 * 모니터 태스크가 이미 부르는 자리에서 한 번 받아 UI 로 넘긴다. */
typedef struct {
    char     name[16];   /* pcTaskName 은 TCB 안을 가리키는 포인터라 태스크가
                          * 죽으면 대롱거린다. 화면은 오래 들고 있으므로 복사한다. */
    uint32_t headroom;   /* 남은 바이트. StackType_t 가 uint8_t 라 단위가 바이트다 */
} task_stack_row_t;

/* 남은 스택이 적은 순으로 채운다. 반환값은 채운 줄 수.
 * us_out 이 NULL 이 아니면 수집에 걸린 시간을 돌려준다 — 그 자체가 측정값이다. */
size_t task_stats_snapshot(task_stack_row_t *rows, size_t max_rows, uint32_t *us_out);
