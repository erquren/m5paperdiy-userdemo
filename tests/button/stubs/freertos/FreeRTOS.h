#pragma once
#include <cstdint>
using TickType_t = std::uint32_t;
using BaseType_t = int;
using portMUX_TYPE = int;
constexpr int pdPASS = 1;
constexpr int pdTRUE = 1;
constexpr int pdFALSE = 0;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr int tskIDLE_PRIORITY = 0;
#define portMUX_INITIALIZER_UNLOCKED 0
#define pdMS_TO_TICKS(ms) (static_cast<TickType_t>(ms))
void testEnterCritical(portMUX_TYPE*);
void testExitCritical(portMUX_TYPE*);
#define portENTER_CRITICAL(mux) testEnterCritical(mux)
#define portEXIT_CRITICAL(mux) testExitCritical(mux)
#define portYIELD_FROM_ISR() ((void)0)
