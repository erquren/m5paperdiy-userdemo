/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_battery.h"

#include <LGFX_Fonts.hpp>
#include <cstdio>
#include <cstring>
#include <hal.h>
#include <app_wifi_scan/app_wifi_scan.h>
#include <app_clock/app_clock.h>

namespace {

constexpr std::uint32_t BATTERY_REFRESH_MS      = 10000;
constexpr std::uint32_t BATTERY_SAMPLE_DELAY_MS = 50;
constexpr std::uint32_t BATTERY_SAMPLE_COUNT    = 10;
constexpr int BATTERY_X                         = 159;
constexpr int BATTERY_Y                         = 888;
constexpr int BATTERY_W                         = 241 - BATTERY_X;
constexpr int BATTERY_H                         = 925 - BATTERY_Y;

}  // namespace

/* -------------------------------------------------------------------------- */
/*                              App Lifecycle                                 */
/* -------------------------------------------------------------------------- */
void AppBattery::onCreate()
{
    setAppInfo().name = "AppBattery";
    open();
}

void AppBattery::onOpen()
{
    startSampling(true);
    while (_sampling) {
        GetHAL().delay(BATTERY_SAMPLE_DELAY_MS);
        collectSample();
    }
    drawPendingVoltage();
}

void AppBattery::onRunning()
{
    if (AppWifiScan::isModal() || AppClock::isActive()) return;
    if (_pending_draw) {
        drawPendingVoltage();
        return;
    }

    auto& hal                  = GetHAL();
    const std::uint32_t now_ms = hal.millis();
    if (_sampling) {
        if (now_ms - _last_sample_ms >= BATTERY_SAMPLE_DELAY_MS) {
            collectSample();
        }
    } else if (now_ms - _last_update_ms >= BATTERY_REFRESH_MS) {
        startSampling(false);
    }
}

/* -------------------------------------------------------------------------- */
/*                            Voltage Sampling                                */
/* -------------------------------------------------------------------------- */
void AppBattery::startSampling(bool force_draw)
{
    _sample_sum         = 0;
    _sample_count       = 0;
    _valid_sample_count = 0;
    _sampling           = true;
    _force_draw         = force_draw;
    collectSample();
}

void AppBattery::collectSample()
{
    _last_sample_ms               = GetHAL().millis();
    const std::int16_t battery_mv = GetHAL().getBatteryVoltageMv();
    if (battery_mv > 0) {
        _sample_sum += battery_mv;
        ++_valid_sample_count;
    }
    ++_sample_count;

    if (_sample_count < BATTERY_SAMPLE_COUNT) {
        return;
    }

    const std::int16_t average_mv =
        _valid_sample_count > 0
            ? static_cast<std::int16_t>((_sample_sum + _valid_sample_count / 2) / _valid_sample_count)
            : -1;
    _sampling           = false;
    _last_update_ms     = _last_sample_ms;
    _pending_mv         = average_mv;
    _pending_force_draw = _pending_force_draw || _force_draw;
    _pending_draw       = true;
}

/* -------------------------------------------------------------------------- */
/*                            Voltage Rendering                               */
/* -------------------------------------------------------------------------- */
void AppBattery::drawPendingVoltage()
{
    auto& hal = GetHAL();
    if (!_pending_draw || hal.isButtonInteractionActive()) {
        return;
    }
    if (drawVoltage(_pending_mv, _pending_force_draw)) {
        _pending_draw       = false;
        _pending_force_draw = false;
    }
}

bool AppBattery::drawVoltage(std::int16_t battery_mv, bool force_draw)
{
    char voltage_text[sizeof(_last_text)] = {};
    if (battery_mv > 0) {
        std::snprintf(voltage_text, sizeof(voltage_text), "%.2fV", battery_mv / 1000.0f);
    } else {
        std::snprintf(voltage_text, sizeof(voltage_text), "--.--V");
    }

    if (!force_draw && std::strcmp(voltage_text, _last_text) == 0) {
        return true;
    }
    auto& hal = GetHAL();
    DisplayGuard guard(hal);
    if (hal.isButtonInteractionActive()) {
        return false;
    }
    auto& display = hal.display;
    display.setEpdMode(m5gfx::epd_mode_t::epd_text);
    display.setTextColor(TFT_BLACK, TFT_WHITE);
    display.setTextDatum(middle_center);
    display.setTextSize(1.0f);
    display.setFont(&fonts::lvFontMontserrat24);
    display.fillRect(BATTERY_X, BATTERY_Y, BATTERY_W, BATTERY_H, TFT_WHITE);
    display.setClipRect(BATTERY_X, BATTERY_Y, BATTERY_W, BATTERY_H);
    display.drawString(voltage_text, BATTERY_X + BATTERY_W / 2, BATTERY_Y + BATTERY_H / 2);
    display.clearClipRect();
    std::snprintf(_last_text, sizeof(_last_text), "%s", voltage_text);

    if (!hal.isFullRedrawRequested()) {
        hal.requestRefresh(BATTERY_X, BATTERY_Y, BATTERY_W, BATTERY_H);
    }
    return true;
}
