/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_shutdown.h"

#include <LGFX_Fonts.hpp>
#include <M5Unified.h>
#include <cstddef>
#include <cstdio>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal.h>
#include <nvs.h>
#include <nvs_flash.h>

extern const std::uint8_t img_logo_png_start[] asm("_binary_img_logo_png_start");
extern const std::uint8_t img_logo_png_end[] asm("_binary_img_logo_png_end");

namespace {

constexpr char APP_TAG[]          = "AppShutdown";
constexpr char NVS_NAMESPACE[]    = "paperdiy";
constexpr char NVS_FACTORY_FLAG[] = "factory_done";

constexpr std::uint32_t ARMING_TIME_MS    = 2000;
constexpr std::uint32_t COUNTDOWN_TIME_MS = 5000;

constexpr int DIALOG_W      = 404;
constexpr int DIALOG_H      = 224;
constexpr int DIALOG_X      = (540 - DIALOG_W) / 2;
constexpr int DIALOG_Y      = (960 - DIALOG_H) / 2;
constexpr int DIALOG_RADIUS = 6;
constexpr int COUNTDOWN_X   = DIALOG_X + 50;
constexpr int COUNTDOWN_Y   = DIALOG_Y + 119;
constexpr int COUNTDOWN_W   = DIALOG_W - 100;
constexpr int COUNTDOWN_H   = 54;

bool areBothButtonsPressed()
{
    const auto& hal = GetHAL();
    return hal.isButtonPressed(ButtonId::A) && hal.isButtonPressed(ButtonId::B);
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                            Factory First Boot                              */
/* -------------------------------------------------------------------------- */
void AppShutdown::handleFactoryBoot()
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        error = nvs_flash_erase();
        if (error == ESP_OK) {
            error = nvs_flash_init();
        }
    }
    if (error != ESP_OK) {
        ESP_LOGE(APP_TAG, "NVS initialization failed: %s", esp_err_to_name(error));
        return;
    }

    nvs_handle_t handle = 0;
    error               = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (error != ESP_OK) {
        ESP_LOGE(APP_TAG, "Unable to open factory NVS: %s", esp_err_to_name(error));
        return;
    }

    std::uint8_t factory_done = 0;
    error                     = nvs_get_u8(handle, NVS_FACTORY_FLAG, &factory_done);
    if (error == ESP_OK && factory_done != 0) {
        nvs_close(handle);
        return;
    }
    if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(APP_TAG, "Unable to read factory flag: %s", esp_err_to_name(error));
        nvs_close(handle);
        return;
    }

    ESP_LOGI(APP_TAG, "First factory boot: displaying shutdown logo");
    GetHAL().beginShutdown();
    const bool logo_drawn = displayShutdownLogo();
    if (logo_drawn) {
        error = nvs_set_u8(handle, NVS_FACTORY_FLAG, 1);
        if (error == ESP_OK) {
            error = nvs_commit(handle);
        }
        if (error != ESP_OK) {
            ESP_LOGE(APP_TAG, "Unable to commit factory flag: %s", esp_err_to_name(error));
        }
    }
    nvs_close(handle);
    sendPowerOffCommand();
}

/* -------------------------------------------------------------------------- */
/*                              App Lifecycle                                 */
/* -------------------------------------------------------------------------- */
void AppShutdown::onCreate()
{
    setAppInfo().name = "AppShutdown";
    open();
}

void AppShutdown::onOpen()
{
}

void AppShutdown::onRunning()
{
    auto& hal                  = GetHAL();
    const std::uint32_t now_ms = hal.millis();
    const bool both_pressed    = areBothButtonsPressed();

    switch (_state) {
        case State::Idle:
            if (both_pressed) {
                _arming_started_ms = now_ms;
                _state             = State::Arming;
            }
            break;

        case State::Arming:
            if (!both_pressed) {
                _state = State::Idle;
            } else if (now_ms - _arming_started_ms >= ARMING_TIME_MS) {
                hal.suppressButtonActionsUntilRelease();
                if (showDialog()) {
                    _countdown_started_ms = hal.millis();
                    _last_countdown       = 5;
                    _state                = State::Countdown;
                } else {
                    _state = State::WaitForRelease;
                }
            }
            break;

        case State::Countdown: {
            if (!both_pressed) {
                dismissDialog();
                _state = State::WaitForRelease;
                break;
            }

            const std::uint32_t elapsed_ms = now_ms - _countdown_started_ms;
            if (elapsed_ms >= COUNTDOWN_TIME_MS) {
                shutDown();
            }

            const unsigned int seconds = 5U - elapsed_ms / 1000U;
            if (seconds != _last_countdown) {
                drawCountdown(seconds);
                _last_countdown = seconds;
            }
            break;
        }

        case State::WaitForRelease:
            if (!hal.isButtonPressed(ButtonId::A) && !hal.isButtonPressed(ButtonId::B)) {
                _state = State::Idle;
            }
            break;
    }
}

/* -------------------------------------------------------------------------- */
/*                              Shutdown Dialog                               */
/* -------------------------------------------------------------------------- */
bool AppShutdown::showDialog()
{
    constexpr std::size_t pixel_count = static_cast<std::size_t>(DIALOG_W) * DIALOG_H;
    _saved_pixels                     = static_cast<std::uint16_t*>(
        heap_caps_malloc(pixel_count * sizeof(std::uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (_saved_pixels == nullptr) {
        _saved_pixels =
            static_cast<std::uint16_t*>(heap_caps_malloc(pixel_count * sizeof(std::uint16_t), MALLOC_CAP_8BIT));
    }
    if (_saved_pixels == nullptr) {
        ESP_LOGE(APP_TAG, "Unable to allocate dialog background buffer");
        return false;
    }

    auto& hal     = GetHAL();
    auto& display = hal.display;
    display.waitDisplay();
    {
        DisplayGuard guard(hal);
        display.readRect(DIALOG_X, DIALOG_Y, DIALOG_W, DIALOG_H, _saved_pixels);
        display.setEpdMode(m5gfx::epd_mode_t::epd_quality);
        display.fillRoundRect(DIALOG_X, DIALOG_Y, DIALOG_W, DIALOG_H, DIALOG_RADIUS, TFT_WHITE);
        display.drawRoundRect(DIALOG_X, DIALOG_Y, DIALOG_W, DIALOG_H, DIALOG_RADIUS, TFT_BLACK);
        display.drawRoundRect(DIALOG_X + 1, DIALOG_Y + 1, DIALOG_W - 2, DIALOG_H - 2, DIALOG_RADIUS - 1, TFT_BLACK);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        display.setTextDatum(middle_center);
        display.setTextSize(1.0f);
        display.setClipRect(DIALOG_X, DIALOG_Y, DIALOG_W, DIALOG_H);

        display.setFont(&fonts::lvFontMontserrat28);
        display.drawString("POWER OFF", DIALOG_X + DIALOG_W / 2, DIALOG_Y + 34);
        display.drawFastHLine(DIALOG_X + 24, DIALOG_Y + 59, DIALOG_W - 48, 0xA514);
        display.setFont(&fonts::lvFontMontserrat20);
        display.drawString("KEEP HOLDING A + B", DIALOG_X + DIALOG_W / 2, DIALOG_Y + 87);
        display.setFont(&fonts::lvFontMontserrat14);
        display.drawString("POWER OFF IN", DIALOG_X + DIALOG_W / 2, DIALOG_Y + 113);
        display.setFont(&fonts::lvFontMontserrat36);
        display.drawString("5 SECONDS", DIALOG_X + DIALOG_W / 2, DIALOG_Y + 146);
        display.setFont(&fonts::lvFontMontserrat14);
        display.drawString("RELEASE EITHER BUTTON TO CANCEL", DIALOG_X + DIALOG_W / 2, DIALOG_Y + 196);
        display.clearClipRect();
        hal.flushDisplayRegion(DIALOG_X, DIALOG_Y, DIALOG_W, DIALOG_H, m5gfx::epd_mode_t::epd_quality);
    }
    display.waitDisplay();
    return true;
}

void AppShutdown::drawCountdown(unsigned int seconds)
{
    char text[24] = {};
    std::snprintf(text, sizeof(text), "%u SECONDS", seconds);

    auto& hal     = GetHAL();
    auto& display = hal.display;
    display.waitDisplay();
    {
        DisplayGuard guard(hal);
        display.setEpdMode(m5gfx::epd_mode_t::epd_fastest);
        display.fillRect(COUNTDOWN_X, COUNTDOWN_Y, COUNTDOWN_W, COUNTDOWN_H, TFT_WHITE);
        display.setTextColor(TFT_BLACK, TFT_WHITE);
        display.setTextDatum(middle_center);
        display.setTextSize(1.0f);
        display.setFont(&fonts::lvFontMontserrat36);
        display.setClipRect(COUNTDOWN_X, COUNTDOWN_Y, COUNTDOWN_W, COUNTDOWN_H);
        display.drawString(text, COUNTDOWN_X + COUNTDOWN_W / 2, COUNTDOWN_Y + COUNTDOWN_H / 2);
        display.clearClipRect();
        hal.flushDisplayRegion(COUNTDOWN_X, COUNTDOWN_Y, COUNTDOWN_W, COUNTDOWN_H, m5gfx::epd_mode_t::epd_fastest);
    }
    display.waitDisplay();
}

void AppShutdown::dismissDialog()
{
    if (_saved_pixels == nullptr) {
        return;
    }

    auto& hal     = GetHAL();
    auto& display = hal.display;
    display.waitDisplay();
    {
        DisplayGuard guard(hal);
        display.setEpdMode(m5gfx::epd_mode_t::epd_quality);
        display.pushImage(DIALOG_X, DIALOG_Y, DIALOG_W, DIALOG_H, _saved_pixels);
        hal.flushDisplayRegion(DIALOG_X, DIALOG_Y, DIALOG_W, DIALOG_H, m5gfx::epd_mode_t::epd_quality);
    }
    display.waitDisplay();
    heap_caps_free(_saved_pixels);
    _saved_pixels = nullptr;
}

/* -------------------------------------------------------------------------- */
/*                              Power Control                                 */
/* -------------------------------------------------------------------------- */
[[noreturn]] void AppShutdown::shutDown()
{
    GetHAL().beginShutdown();
    displayShutdownLogo();
    sendPowerOffCommand();
}

bool AppShutdown::displayShutdownLogo()
{
    auto& hal     = GetHAL();
    auto& display = hal.display;
    bool drawn    = false;
    display.waitDisplay();
    {
        DisplayGuard guard(hal);
        display.setEpdMode(m5gfx::epd_mode_t::epd_quality);
        display.fillScreen(TFT_WHITE);
        const std::size_t image_size = img_logo_png_end - img_logo_png_start;
        drawn                        = display.drawPng(img_logo_png_start, image_size, 0, 0);
        if (!drawn) {
            ESP_LOGE(APP_TAG, "Failed to decode shutdown logo PNG");
        }
        hal.flushFullDisplay(m5gfx::epd_mode_t::epd_quality);
    }
    display.waitDisplay();
    return drawn;
}

[[noreturn]] void AppShutdown::sendPowerOffCommand()
{
    ESP_LOGI(APP_TAG, "Sending M5PM1 shutdown command");
    while (true) {
        if (!M5.Power.M5pm1.powerOff()) {
            ESP_LOGE(APP_TAG, "M5PM1 shutdown command failed");
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
