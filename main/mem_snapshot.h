#pragma once

/* label 은 측정 지점 구분용 꼬리표. 로그에 그대로 찍힌다. */
void mem_snapshot_log(const char *label);

/* 한 줄 요약. 주기 출력용이다. 시계열을 눈으로 훑는 것이 목적이라 리전 4종을
 * 다 찍지 않고 내부 SRAM 과 PSRAM 만 남긴다. */
void mem_snapshot_log_line(const char *label);
