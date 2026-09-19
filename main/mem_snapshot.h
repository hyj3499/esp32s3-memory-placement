#pragma once

#include <stddef.h>

/* ---- 프로그램이 읽는 값 ----
 *
 * 로그는 사람이 읽는 출력이고, 이쪽은 프로그램이 읽는 값이다. 대시보드가
 * 생기면서 같은 숫자를 시리얼과 화면이 같이 쓰게 됐다.
 *
 * ⚠️ `heap_caps_get_info()` 는 힙을 순회하는 O(n) 함수다. 화면 갱신 주기로
 * 부르면 계측이 관측 대상을 밀어낸다. **수집은 모니터 태스크가 한 번만 하고,
 * 시리얼과 UI 는 그 결과를 나눠 쓴다.** */
typedef struct {
    size_t   internal_free;
    size_t   internal_largest;
    size_t   internal_min_free;
    size_t   internal_alloc;
    unsigned internal_frag;
    size_t   psram_free;
    size_t   psram_largest;
    size_t   psram_alloc;
} mem_stats_t;

void mem_snapshot_get(mem_stats_t *out);

/* label 은 측정 지점 구분용 꼬리표. 로그에 그대로 찍힌다. */
void mem_snapshot_log(const char *label);

/* 한 줄 요약. 주기 출력용이다. 시계열을 눈으로 훑는 것이 목적이라 리전 4종을
 * 다 찍지 않고 내부 SRAM 과 PSRAM 만 남긴다.
 *
 * 값을 직접 걷지 않고 받아 쓴다 — 위 주석의 이유. */
void mem_snapshot_log_line(const char *label, const mem_stats_t *s);
