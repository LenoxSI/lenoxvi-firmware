/* Host-test stub: the tick clock is the test's g_ticks (1 tick = 1 ms). */
#pragma once
#include "freertos/FreeRTOS.h"
#define xTaskGetTickCount() ((TickType_t)g_ticks)
#define vTaskDelay(t) ((void)(t))
#define xTaskCreate(fn, name, stack, arg, prio, handle) ((void)(fn), pdPASS)
