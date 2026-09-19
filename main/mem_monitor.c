#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "mem_monitor.h"
#include "mem_snapshot.h"
#include "task_stats.h"
#include "ui_dashboard.h"

static const char *TAG = "monitor";

/* StackType_t 가 uint8_t 이므로 이 숫자는 워드가 아니라 바이트다 (portmacro.h:88).
 * ESP_LOGI 의 포맷팅이 스택을 쓰므로 1KB 로는 부족하다. */
#define MONITOR_STACK_BYTES 3072

/* 부하 태스크(prio 4)보다 낮게 둔다. 계측이 관측 대상을 밀어내면 안 된다. */
#define MONITOR_PRIO        3

/* 스택과 TCB 를 정적으로 잡는다. xTaskCreate 를 쓰면 스택이 힙에서 나오고,
 * 그러면 계측이 관측 대상을 오염시킨다. 정적으로 두면 비용이 .bss 로 옮겨가서
 * 힙 풀이 그만큼 줄지만, 그 값은 부팅 로그에 찍히는 고정값이라 계산에서 뺄 수 있다.
 * 측정에서는 "매번 달라지는 작은 값"보다 "한 번 확인하면 끝인 큰 값"이 낫다. */
static StackType_t  s_stack[MONITOR_STACK_BYTES];
static StaticTask_t s_tcb;
static TaskHandle_t s_task;

/* 화면으로 넘길 스택 표. 모니터 태스크의 스택에 두면 240바이트를 더 먹는데,
 * 이 태스크의 여유가 실측 936바이트뿐이라 .bss 로 뺀다. */
#define UI_STACK_ROWS 8
static task_stack_row_t s_rows[UI_STACK_ROWS];

static void monitor_task(void *arg)
{
    const int64_t t0 = esp_timer_get_time();
    uint32_t seq = 0;

    for (;;) {
        /* 부동소수 포맷(%f)을 피한다. nano formatting 설정에 따라 안 찍힐 수 있다. */
        uint32_t sec = (uint32_t)((esp_timer_get_time() - t0) / 1000000);

        const uint32_t n = seq++;

        char label[24];
        snprintf(label, sizeof(label), "#%lu t=%lus",
                 (unsigned long)n, (unsigned long)sec);

        /* 힙을 한 번만 걷는다. heap_caps_get_info() 는 O(n) 이라, 시리얼과
         * 화면이 각자 걷으면 계측 비용이 그대로 두 배가 된다. */
        mem_stats_t st;
        mem_snapshot_get(&st);

        mem_snapshot_log_line(label, &st);
        ui_dashboard_push_heap(&st, sec);

        /* 수집 주기를 여기로 일원화한다. 태스크를 하나 더 띄우면 그 스택과 TCB 가
         * 또 관측 대상이 되고, 두 계측이 서로의 출력 사이에 끼어든다.
         * 스택 표는 힙 줄보다 훨씬 길어서 매번 찍으면 시계열이 묻힌다. 워터마크는
         * 누적 최저치라 자주 찍어도 새로 얻는 값이 없다. */
#if CONFIG_MHM_TASK_STATS_EVERY > 0
        if (n % CONFIG_MHM_TASK_STATS_EVERY == 0) {
            task_stats_log(label);

            /* ⚠️ 여기서 스택을 한 번 더 걷는다. task_stats_log() 가 쓰는 배열이
             * 그 파일 안에 갇혀 있어서다. 비용은 수집 1회분(실측 624us)이고
             * 이 분기는 30초에 한 번이라 감수한다. 합치려면 task_stats 쪽에
             * "수집해서 돌려주고, 출력은 호출자가" 형태로 바꿔야 한다. */
            uint32_t us = 0;
            const size_t rows = task_stats_snapshot(s_rows, UI_STACK_ROWS, &us);
            ui_dashboard_push_stacks(s_rows, rows, us);
        }
#endif

        vTaskDelay(pdMS_TO_TICKS(CONFIG_MHM_MONITOR_PERIOD_MS));
    }
}

esp_err_t mem_monitor_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }

    s_task = xTaskCreateStatic(monitor_task, "mem_monitor", MONITOR_STACK_BYTES,
                               NULL, MONITOR_PRIO, s_stack, &s_tcb);
    if (s_task == NULL) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "%d ms 주기로 힙을 찍는다", CONFIG_MHM_MONITOR_PERIOD_MS);
    return ESP_OK;
}
