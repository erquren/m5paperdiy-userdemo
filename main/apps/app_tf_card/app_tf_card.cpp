/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_tf_card.h"
#include <app_wifi_scan/app_wifi_scan.h>

#include <LGFX_Fonts.hpp>
#include <cstdio>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string>

namespace {

constexpr std::uint32_t TF_POLL_MS       = 500;
constexpr int TF_INFO_X                  = 121;
constexpr int TF_INFO_Y                  = 730;
constexpr int TF_INFO_W                  = 418 - TF_INFO_X;
constexpr int TF_INFO_H                  = 831 - TF_INFO_Y;
constexpr int TF_CONTENT_LEFT            = TF_INFO_X + 7;
constexpr int TF_CONTENT_RIGHT           = TF_INFO_X + TF_INFO_W - 7;
constexpr int TF_CONTENT_W               = TF_CONTENT_RIGHT - TF_CONTENT_LEFT;
constexpr int TF_COLUMN_GAP              = 14;
constexpr int TF_COLUMN_W                = (TF_CONTENT_W - TF_COLUMN_GAP) / 2;
constexpr int TF_RIGHT_COLUMN_X          = TF_CONTENT_LEFT + TF_COLUMN_W + TF_COLUMN_GAP;
constexpr int TF_LEFT_COLUMN_CENTER      = TF_CONTENT_LEFT + TF_COLUMN_W / 2;
constexpr int TF_RIGHT_COLUMN_CENTER     = TF_RIGHT_COLUMN_X + TF_COLUMN_W / 2;
constexpr int TF_HEADER_LEFT_X           = TF_CONTENT_LEFT + 13;
constexpr int TF_HEADER_RIGHT_X          = TF_CONTENT_RIGHT - 13;
constexpr int TF_HEADER_Y                = TF_INFO_Y + 20;
constexpr int TF_DIVIDER_Y               = TF_INFO_Y + 40;
constexpr int TF_SPEED_LABEL_Y           = TF_INFO_Y + 55;
constexpr int TF_SPEED_VALUE_Y           = TF_INFO_Y + 79;
constexpr std::uint16_t TF_DIVIDER_COLOR = 0xA514;

std::string formatCapacity(std::uint64_t bytes)
{
    char text[24] = {};
    std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return text;
}

std::string formatSpeed(float speed)
{
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%.2f MB/s", static_cast<double>(speed));
    return text;
}

void requestTfRefresh(m5gfx::epd_mode_t mode)
{
    if (!GetHAL().isFullRedrawRequested()) {
        GetHAL().requestRefresh(TF_INFO_X, TF_INFO_Y, TF_INFO_W, TF_INFO_H, mode);
    }
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                              App Lifecycle                                 */
/* -------------------------------------------------------------------------- */
void AppTfCard::onCreate()
{
    setAppInfo().name = "AppTfCard";
    open();
}

void AppTfCard::onOpen()
{
    updateCard(true);
}

void AppTfCard::onRunning()
{
    if (AppWifiScan::isModal()) return;
    if (_test_done.exchange(false, std::memory_order_acq_rel)) {
        _test_running = false;
        drawInfo();
        return;
    }

    if (!_test_running && GetHAL().wasButtonBClicked()) {
        updateCard(false, true);
        if (GetHAL().getTfCardInfo().status == TfCardStatus::Mounted) {
            drawStatus("TESTING...");
            if (!startBenchmark()) {
                _benchmark.message = "TASK FAILED";
                drawInfo();
            }
        }
        return;
    }

    if (_test_running) {
        return;
    }

    const std::uint32_t now_ms = GetHAL().millis();
    if (now_ms - _last_poll_ms >= TF_POLL_MS) {
        updateCard(false);
    }
}

/* -------------------------------------------------------------------------- */
/*                            Card Benchmark                                  */
/* -------------------------------------------------------------------------- */
bool AppTfCard::startBenchmark()
{
    _test_done.store(false, std::memory_order_relaxed);
    _test_running            = true;
    const BaseType_t created = xTaskCreate(benchmarkTask, "tf_benchmark", 6144, this, tskIDLE_PRIORITY + 2, nullptr);
    if (created != pdPASS) {
        _test_running = false;
        return false;
    }
    return true;
}

void AppTfCard::benchmarkTask(void* argument)
{
    auto* app       = static_cast<AppTfCard*>(argument);
    app->_benchmark = GetHAL().benchmarkTfCard();
    app->_test_done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

/* -------------------------------------------------------------------------- */
/*                              Card Display                                  */
/* -------------------------------------------------------------------------- */
void AppTfCard::updateCard(bool force_draw, bool retry_mount)
{
    _last_poll_ms               = GetHAL().millis();
    const TfCardStatus previous = GetHAL().getTfCardInfo().status;
    const TfCardInfo& info      = GetHAL().updateTfCard(retry_mount);
    if (info.status != previous) {
        _benchmark = {};
    }
    if (force_draw || !_has_drawn || info.status != _last_status) {
        _last_status = info.status;
        drawInfo();
    }
}

void AppTfCard::drawInfo()
{
    const TfCardInfo& info = GetHAL().getTfCardInfo();
    auto& hal              = GetHAL();
    DisplayGuard guard(hal);
    auto& display = hal.display;
    display.setEpdMode(m5gfx::epd_mode_t::epd_quality);
    display.fillRect(TF_INFO_X, TF_INFO_Y, TF_INFO_W, TF_INFO_H, TFT_WHITE);
    display.setTextColor(TFT_BLACK, TFT_WHITE);
    display.setTextSize(1.0f);
    display.setClipRect(TF_INFO_X, TF_INFO_Y, TF_INFO_W, TF_INFO_H);

    if (info.status == TfCardStatus::Mounted) {
        const std::string capacity = formatCapacity(info.capacity_bytes);

        // Header: card type on the left and capacity on the right.
        display.setFont(&fonts::lvFontMontserrat18);
        display.setTextDatum(middle_left);
        display.setClipRect(TF_CONTENT_LEFT, TF_INFO_Y, TF_COLUMN_W, TF_DIVIDER_Y - TF_INFO_Y);
        display.drawString(info.type.c_str(), TF_HEADER_LEFT_X, TF_HEADER_Y);

        display.setTextDatum(middle_right);
        display.setClipRect(TF_RIGHT_COLUMN_X, TF_INFO_Y, TF_COLUMN_W, TF_DIVIDER_Y - TF_INFO_Y);
        display.drawString(capacity.c_str(), TF_HEADER_RIGHT_X, TF_HEADER_Y);

        display.setClipRect(TF_INFO_X, TF_INFO_Y, TF_INFO_W, TF_INFO_H);
        display.drawFastHLine(TF_CONTENT_LEFT, TF_DIVIDER_Y, TF_CONTENT_W, TF_DIVIDER_COLOR);

        if (!_benchmark.message.empty() && !_benchmark.success) {
            display.setTextDatum(middle_center);
            display.setFont(&fonts::lvFontMontserrat14);
            display.drawString("TEST FAILED", TF_INFO_X + TF_INFO_W / 2, TF_SPEED_LABEL_Y);
            display.setFont(&fonts::lvFontMontserrat18);
            display.drawString(_benchmark.message.c_str(), TF_INFO_X + TF_INFO_W / 2, TF_SPEED_VALUE_Y);
        } else {
            // Each speed cell has its own clip rectangle, preventing cross-column overlap.
            display.drawFastVLine(TF_INFO_X + TF_INFO_W / 2, TF_DIVIDER_Y + 8,
                                  TF_INFO_H - (TF_DIVIDER_Y - TF_INFO_Y) - 15, TF_DIVIDER_COLOR);
            display.setTextDatum(middle_center);
            display.setFont(&fonts::lvFontMontserrat14);
            display.setClipRect(TF_CONTENT_LEFT, TF_DIVIDER_Y + 1, TF_COLUMN_W,
                                TF_INFO_Y + TF_INFO_H - TF_DIVIDER_Y - 1);
            display.drawString("WRITE", TF_LEFT_COLUMN_CENTER, TF_SPEED_LABEL_Y);
            display.setFont(&fonts::lvFontMontserrat18);
            const std::string write_speed = _benchmark.success ? formatSpeed(_benchmark.write_mb_s) : "-- MB/s";
            display.drawString(write_speed.c_str(), TF_LEFT_COLUMN_CENTER, TF_SPEED_VALUE_Y);

            display.setClipRect(TF_RIGHT_COLUMN_X, TF_DIVIDER_Y + 1, TF_COLUMN_W,
                                TF_INFO_Y + TF_INFO_H - TF_DIVIDER_Y - 1);
            display.setFont(&fonts::lvFontMontserrat14);
            display.drawString("READ", TF_RIGHT_COLUMN_CENTER, TF_SPEED_LABEL_Y);
            display.setFont(&fonts::lvFontMontserrat18);
            const std::string read_speed = _benchmark.success ? formatSpeed(_benchmark.read_mb_s) : "-- MB/s";
            display.drawString(read_speed.c_str(), TF_RIGHT_COLUMN_CENTER, TF_SPEED_VALUE_Y);
        }
    } else {
        display.setFont(&fonts::lvFontMontserrat20);
        display.setTextDatum(middle_center);
        const char* text = info.status == TfCardStatus::NoCard ? "NO TF CARD" : "TF INIT FAILED";
        display.drawString(text, TF_INFO_X + TF_INFO_W / 2, TF_INFO_Y + TF_INFO_H / 2);
    }

    display.clearClipRect();
    requestTfRefresh(m5gfx::epd_mode_t::epd_quality);
    _has_drawn = true;
}

void AppTfCard::drawStatus(const char* text)
{
    auto& hal = GetHAL();
    DisplayGuard guard(hal);
    auto& display = hal.display;
    display.setEpdMode(m5gfx::epd_mode_t::epd_text);
    display.fillRect(TF_INFO_X, TF_INFO_Y, TF_INFO_W, TF_INFO_H, TFT_WHITE);
    display.setTextColor(TFT_BLACK, TFT_WHITE);
    display.setTextDatum(middle_center);
    display.setTextSize(1.0f);
    display.setFont(&fonts::lvFontMontserrat18);
    display.setClipRect(TF_INFO_X, TF_INFO_Y, TF_INFO_W, TF_INFO_H);
    display.drawString(text, TF_INFO_X + TF_INFO_W / 2, TF_INFO_Y + TF_INFO_H / 2);
    display.clearClipRect();
    requestTfRefresh(m5gfx::epd_mode_t::epd_text);
}
