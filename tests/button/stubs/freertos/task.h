#pragma once
#include "FreeRTOS.h"
using TaskFunction_t = void (*)(void*);
TickType_t xTaskGetTickCount();
void vTaskDelay(TickType_t ticks);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char*, unsigned int, void*, int, void*, int);
