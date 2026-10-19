#pragma once
#include "FreeRTOS.h"
using TaskFunction_t = void (*)(void*);
using TaskHandle_t = void*;
TickType_t xTaskGetTickCount();
void vTaskDelay(TickType_t ticks);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char*, unsigned int, void*, int, TaskHandle_t*, int);
void xTaskNotifyGive(TaskHandle_t);
void vTaskNotifyGiveFromISR(TaskHandle_t, BaseType_t*);
std::uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
