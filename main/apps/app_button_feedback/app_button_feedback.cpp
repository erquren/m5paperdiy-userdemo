/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_button_feedback.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <hal.h>
#include <app_wifi_scan/app_wifi_scan.h>
#include <app_clock/app_clock.h>

namespace {

constexpr int BORDER_X               = 9;
constexpr int BORDER_A_Y             = 620;
constexpr int BORDER_B_Y             = 731;
constexpr int BORDER_RIGHT           = 119;
constexpr int BORDER_A_BOTTOM        = 686;
constexpr int BORDER_B_BOTTOM        = 797;
constexpr int BORDER_W               = BORDER_RIGHT - BORDER_X + 1;
constexpr int BORDER_H               = BORDER_A_BOTTOM - BORDER_A_Y + 1;
constexpr int BORDER_THICKNESS       = 2;
constexpr int BORDER_RIGHT_RADIUS    = 17;
constexpr int BORDER_REFRESH_X       = BORDER_X & ~3;
constexpr int BORDER_REFRESH_RIGHT   = (BORDER_RIGHT + 4) & ~3;
constexpr int BORDER_REFRESH_W       = BORDER_REFRESH_RIGHT - BORDER_REFRESH_X;
constexpr unsigned int BUTTON_A_MASK = 1U << 0;
constexpr unsigned int BUTTON_B_MASK = 1U << 1;

static_assert(BORDER_B_Y + BORDER_H - 1 == BORDER_B_BOTTOM);

void drawRightCorners(M5GFX& display, int center_x, int top_center_y, int bottom_center_y, std::uint16_t color)
{
    const int inner_radius = BORDER_RIGHT_RADIUS - BORDER_THICKNESS + 1;
    display.fillArc(center_x, top_center_y, BORDER_RIGHT_RADIUS, inner_radius, 270.0f, 360.0f, color);
    display.fillArc(center_x, bottom_center_y, BORDER_RIGHT_RADIUS, inner_radius, 0.0f, 90.0f, color);
}

void drawBorder(int y, bool pressed)
{
    auto& display = GetHAL().display;
    display.setEpdMode(m5gfx::epd_mode_t::epd_fastest);
    const std::uint16_t color = pressed ? TFT_BLACK : TFT_WHITE;
    const int right_center_x  = BORDER_RIGHT - BORDER_RIGHT_RADIUS;
    const int bottom          = y + BORDER_H - 1;

    drawRightCorners(display, right_center_x, y + BORDER_RIGHT_RADIUS, bottom - BORDER_RIGHT_RADIUS, color);

    display.fillRect(BORDER_X, y, right_center_x - BORDER_X + 1, BORDER_THICKNESS, color);
    display.fillRect(BORDER_X, bottom - BORDER_THICKNESS + 1, right_center_x - BORDER_X + 1, BORDER_THICKNESS, color);
    display.fillRect(BORDER_X, y, BORDER_THICKNESS, BORDER_H, color);
    display.fillRect(BORDER_RIGHT - BORDER_THICKNESS + 1, y + BORDER_RIGHT_RADIUS, BORDER_THICKNESS,
                     BORDER_H - BORDER_RIGHT_RADIUS * 2, color);
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                            Feedback Task                                   */
/* -------------------------------------------------------------------------- */
bool ButtonFeedback::begin()
{
    {
        DisplayGuard guard(GetHAL());
        update(true);
        refreshUpdate();
    }
    GetHAL().display.waitDisplay();

    return xTaskCreatePinnedToCore(feedbackTask, "button_feedback", 4096, this, tskIDLE_PRIORITY + 3, nullptr, 1) ==
           pdPASS;
}

void ButtonFeedback::feedbackTask(void* argument)
{
    auto* feedback = static_cast<ButtonFeedback*>(argument);
    while (true) {
        auto& hal = GetHAL();
        if (hal.isShutdownInProgress() || AppWifiScan::isModal() || AppClock::isActive()) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        bool changed = false;
        {
            DisplayGuard guard(hal);
            if (!hal.isShutdownInProgress() && !AppWifiScan::isModal() && !AppClock::isActive()) {
                changed = feedback->update();
                if (changed) {
                    feedback->refreshUpdate();
                }
            }
        }

        if (changed) {
            hal.display.waitDisplay();
        } else {
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }
}

/* -------------------------------------------------------------------------- */
/*                           Border Rendering                                 */
/* -------------------------------------------------------------------------- */
bool ButtonFeedback::update(bool force_draw)
{
    auto& hal        = GetHAL();
    _updated_buttons = 0;

    if (force_draw || !_initialized) {
        _button_a_pressed = hal.isButtonPressed(ButtonId::A);
        _button_b_pressed = hal.isButtonPressed(ButtonId::B);
        if (_button_a_pressed) {
            drawBorder(BORDER_A_Y, true);
            _updated_buttons |= BUTTON_A_MASK;
        }
        if (_button_b_pressed) {
            drawBorder(BORDER_B_Y, true);
            _updated_buttons |= BUTTON_B_MASK;
        }
        _initialized = true;
        return _updated_buttons != 0;
    }

    const bool button_a_pressed = hal.isButtonPressed(ButtonId::A);
    if (button_a_pressed != _button_a_pressed) {
        _button_a_pressed = button_a_pressed;
        drawBorder(BORDER_A_Y, button_a_pressed);
        _updated_buttons |= BUTTON_A_MASK;
    }

    const bool button_b_pressed = hal.isButtonPressed(ButtonId::B);
    if (button_b_pressed != _button_b_pressed) {
        _button_b_pressed = button_b_pressed;
        drawBorder(BORDER_B_Y, button_b_pressed);
        _updated_buttons |= BUTTON_B_MASK;
    }

    return _updated_buttons != 0;
}

void ButtonFeedback::refreshUpdate()
{
    if (_updated_buttons == 0) {
        return;
    }

    auto& hal                   = GetHAL();
    bool full_refresh_completed = false;
    if ((_updated_buttons & BUTTON_A_MASK) != 0) {
        full_refresh_completed = hal.flushDisplayRegion(BORDER_REFRESH_X, BORDER_A_Y, BORDER_REFRESH_W, BORDER_H,
                                                        m5gfx::epd_mode_t::epd_fastest);
    }
    if (!full_refresh_completed && (_updated_buttons & BUTTON_B_MASK) != 0) {
        hal.flushDisplayRegion(BORDER_REFRESH_X, BORDER_B_Y, BORDER_REFRESH_W, BORDER_H,
                               m5gfx::epd_mode_t::epd_fastest);
    }
    _updated_buttons = 0;
}
