#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "task_stats.h"

static const char *TAG = "stack";

#if CONFIG_FREERTOS_USE_TRACE_FACILITY

/* 정적으로 잡는 이유는 힙 계측과 같다 — 스택을 재려고 힙을 건드리면 안 된다.
 * uxTaskGetSystemState() 가 커널 락 안에서 이 배열을 채우므로, 크기를 키워도
 * 시간 비용은 늘지 않는다. 늘어나는 것은 .bss 뿐이다. */
static TaskStatus_t s_status[CONFIG_MHM_MAX_TASKS];

void task_stats_log_cost(void)
{
    ESP_LOGI(TAG, "TaskStatus_t=%u bytes x %d = %u bytes (.bss), 현재 태스크 %u개",
             (unsigned)sizeof(TaskStatus_t),
             CONFIG_MHM_MAX_TASKS,
             (unsigned)sizeof(s_status),
             (unsigned)uxTaskGetNumberOfTasks());
}

/* eTaskState -> 한 글자. 순서는 task.h 의 enum 정의를 따른다. */
static char state_char(eTaskState st)
{
    switch (st) {
    case eRunning:   return 'R';
    case eReady:     return 'r';
    case eBlocked:   return 'B';
    case eSuspended: return 'S';
    case eDeleted:   return 'D';
    default:         return '?';
    }
}

/* 남은 스택이 적은 순으로 정렬한다. 위험한 줄이 위에 오지 않으면 표를 눈으로
 * 훑는 의미가 없다. n 이 24 이하라 삽입 정렬로 충분하다. */
static void sort_by_headroom(TaskStatus_t *a, UBaseType_t n)
{
    for (UBaseType_t i = 1; i < n; i++) {
        TaskStatus_t key = a[i];
        UBaseType_t j = i;
        while (j > 0 && a[j - 1].usStackHighWaterMark > key.usStackHighWaterMark) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

void task_stats_log(const char *label)
{
    /* 수집과 출력을 분리한다. uxTaskGetSystemState() 는 커널 락을 쥔 채
     * (tasks.c:2976) 태스크마다 스택을 바이트 단위로 훑으므로(tasks.c:4807),
     * 그 안에서 로그를 부르면 TROUBLESHOOTING #3 과 같은 길이 된다.
     * 배열은 이미 복사본이라 락 밖에서 찍으면 된다. */
    const int64_t t0 = esp_timer_get_time();
    const UBaseType_t n = uxTaskGetSystemState(s_status, CONFIG_MHM_MAX_TASKS, NULL);
    const int64_t us = esp_timer_get_time() - t0;

    /* 배열이 모자라면 에러가 아니라 0 을 반환하고 아무것도 쓰지 않는다.
     * 이 처리가 없으면 표가 그냥 비어 보이고 원인을 한참 찾게 된다. */
    if (n == 0) {
        ESP_LOGW(TAG, "%s 배열 부족 — MHM_MAX_TASKS=%d, 현재 태스크 %u개",
                 label, CONFIG_MHM_MAX_TASKS, (unsigned)uxTaskGetNumberOfTasks());
        return;
    }

    sort_by_headroom(s_status, n);

    /* 수집 자체가 관측 대상을 밀어내는 비용이다. 그것도 측정값이라 같이 찍는다. */
    ESP_LOGI(TAG, "==== %s 스택 워터마크 (%u개, 수집 %luus) ====",
             label, (unsigned)n, (unsigned long)us);
    ESP_LOGI(TAG, "%-16s %10s %5s %5s", "task", "남은바이트", "prio", "st");

    for (UBaseType_t i = 0; i < n; i++) {
        const TaskStatus_t *t = &s_status[i];

        /* usStackHighWaterMark 의 단위는 바이트다. tasks.c:4818 이
         * sizeof(StackType_t) 로 나누는데 Xtensa 에서 그 값이 1 이다. */
#ifdef configTASKLIST_INCLUDE_COREID
        ESP_LOGI(TAG, "%-16s %10u %5u %5c %d",
                 t->pcTaskName, (unsigned)t->usStackHighWaterMark,
                 (unsigned)t->uxCurrentPriority, state_char(t->eCurrentState),
                 (int)t->xCoreID);
#else
        ESP_LOGI(TAG, "%-16s %10u %5u %5c",
                 t->pcTaskName, (unsigned)t->usStackHighWaterMark,
                 (unsigned)t->uxCurrentPriority, state_char(t->eCurrentState));
#endif
    }
}


size_t task_stats_snapshot(task_stack_row_t *rows, size_t max_rows, uint32_t *us_out)
{
    const int64_t t0 = esp_timer_get_time();
    const UBaseType_t n = uxTaskGetSystemState(s_status, CONFIG_MHM_MAX_TASKS, NULL);
    const int64_t us = esp_timer_get_time() - t0;

    if (us_out) {
        *us_out = (uint32_t)us;
    }
    if (n == 0) {
        return 0;
    }

    sort_by_headroom(s_status, n);

    size_t out = 0;
    for (UBaseType_t i = 0; i < n && out < max_rows; i++, out++) {
        /* 이름을 복사한다. pcTaskName 은 TCB 안을 가리키므로 태스크가 죽으면
         * 화면이 죽은 메모리를 읽게 된다. */
        strncpy(rows[out].name, s_status[i].pcTaskName, sizeof(rows[out].name) - 1);
        rows[out].name[sizeof(rows[out].name) - 1] = 0;
        rows[out].headroom = (uint32_t)s_status[i].usStackHighWaterMark;
    }
    return out;
}

#else  /* CONFIG_FREERTOS_USE_TRACE_FACILITY */

void task_stats_log_cost(void)
{
    ESP_LOGW(TAG, "FREERTOS_USE_TRACE_FACILITY 가 꺼져 있어 스택 표를 수집할 수 없다");
}

void task_stats_log(const char *label)
{
    (void)label;
}


size_t task_stats_snapshot(task_stack_row_t *rows, size_t max_rows, uint32_t *us_out)
{
    (void)rows;
    (void)max_rows;
    if (us_out) {
        *us_out = 0;
    }
    return 0;
}

#endif /* CONFIG_FREERTOS_USE_TRACE_FACILITY */

/* IDF 기본 구현은 weak 다(portable/xtensa/port.c:561). 여기서 덮어쓴다.
 *
 * 이 함수가 불린 시점에는 스택이 이미 깨져 있다. 그 위에서 할 수 있는 일이
 * 많지 않다 — ESP_LOGx 는 내부 포맷팅에 스택을 더 쓰므로 부르지 않고,
 * ROM 의 printf 로 이름만 남기고 즉시 죽인다.
 *
 * 감지 방식은 CONFIG_FREERTOS_CHECK_STACKOVERFLOW_CANARY, 즉
 * configCHECK_FOR_STACK_OVERFLOW=2 다. 컨텍스트 스위치마다 스택 바닥의
 * 4워드만 0xa5a5a5a5 인지 본다(stack_macros.h:94). 16바이트를 건너뛰어
 * 망가뜨리면 못 잡는다 — 커널 주석도 "does not guarantee" 라고 적어 뒀다. */
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    esp_rom_printf("*** stack overflow: %s ***\n", pcTaskName);
    esp_system_abort("stack overflow");
}
