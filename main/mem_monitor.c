#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "mem_monitor.h"
#include "mem_snapshot.h"

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

static void monitor_task(void *arg)
{
    const int64_t t0 = esp_timer_get_time();
    uint32_t seq = 0;

    for (;;) {
        /* 부동소수 포맷(%f)을 피한다. nano formatting 설정에 따라 안 찍힐 수 있다. */
        uint32_t sec = (uint32_t)((esp_timer_get_time() - t0) / 1000000);

        char label[24];
        snprintf(label, sizeof(label), "#%lu t=%lus",
                 (unsigned long)seq++, (unsigned long)sec);

        mem_snapshot_log_line(label);
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
