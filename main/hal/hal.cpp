/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static constexpr char HAL_TAG[] = "HAL";

/* -------------------------------------------------------------------------- */
/*                                HAL Instance                                */
/* -------------------------------------------------------------------------- */
Hal& GetHAL()
{
    static Hal hal;
    return hal;
}

/* -------------------------------------------------------------------------- */
/*                                    Init                                    */
/* -------------------------------------------------------------------------- */
bool Hal::init()
{
    auto config          = M5.config();
    config.clear_display = false;
    config.internal_imu  = false;
    config.internal_mic  = false;
    config.internal_spk  = false;
    // A/B are handled by the debounced GPIO task; do not poll M5PM1 keys.
    config.pmic_button   = false;
    M5.begin(config);

    _display_mutex = xSemaphoreCreateMutex();
    if (_display_mutex == nullptr) {
        ESP_LOGE(HAL_TAG, "Display mutex initialization failed");
        return false;
    }

    if (!initButtonInput()) {
        ESP_LOGW(HAL_TAG, "Button input task initialization failed");
    }

    if (M5.getDisplayCount() == 0 || display.width() <= 0 || display.height() <= 0) {
        ESP_LOGE(HAL_TAG, "Display initialization failed");
        return false;
    }

    display.setRotation(0);
    display.setAutoDisplay(false);
    if (!initTfCard()) {
        ESP_LOGW(HAL_TAG, "TF card detect initialization failed");
    }
    ESP_LOGI(HAL_TAG, "Display ready: %dx%d", display.width(), display.height());
    return true;
}

/* -------------------------------------------------------------------------- */
/*                                   System                                   */
/* -------------------------------------------------------------------------- */
void Hal::delay(std::uint32_t ms)
{
    m5gfx::delay(ms);
}

std::uint32_t Hal::millis()
{
    return m5gfx::millis();
}

std::int16_t Hal::getBatteryVoltageMv()
{
    return M5.Power.M5pm1.getBatteryVoltage();
}

void Hal::feedTheDog()
{
    vTaskDelay(pdMS_TO_TICKS(5));
}

/* -------------------------------------------------------------------------- */
/*                               Display Locking                              */
/* -------------------------------------------------------------------------- */
void Hal::lockDisplay()
{
    xSemaphoreTake(_display_mutex, portMAX_DELAY);
}

void Hal::unlockDisplay()
{
    xSemaphoreGive(_display_mutex);
}
