/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include <M5Unified.h>
#include <apps.h>
#include <cstddef>
#include <cstdint>
#include <clock/clock_power.h>
#include <esp_log.h>
#include <hal.h>
#include <memory>
#include <mooncake.h>

static constexpr const char* APP_TAG          = "PaperDIY";
static constexpr const char* FIRMWARE_VERSION = "V1.0";

extern const std::uint8_t img_bg_png_start[] asm("_binary_img_bg_png_start");
extern const std::uint8_t img_bg_png_end[] asm("_binary_img_bg_png_end");

static void drawBackground()
{
    auto& hal = GetHAL();
    DisplayGuard guard(hal);
    auto& display = hal.display;
    display.setEpdMode(m5gfx::epd_mode_t::epd_quality);
    display.fillScreen(TFT_WHITE);

    const std::size_t image_size = img_bg_png_end - img_bg_png_start;
    if (!display.drawPng(img_bg_png_start, image_size, 0, 0)) {
        ESP_LOGE(APP_TAG, "Failed to decode embedded background PNG");
    }
    hal.requestFullRedraw(m5gfx::epd_mode_t::epd_quality);
}

static void refreshDisplay()
{
    auto& hal = GetHAL();
    if (!hal.isRefreshRequested()) {
        return;
    }

    {
        DisplayGuard guard(hal);
        hal.display.setEpdMode(hal.isFullRedrawRequested() ? hal.fullRedrawMode() : hal.partialRefreshMode());
        hal.flushDisplay();
    }
    hal.display.waitDisplay();
    hal.clearRefreshRequest();
}

extern "C" void app_main(void)
{
    ESP_LOGI(APP_TAG, "Firmware version: %s", FIRMWARE_VERSION);

    if (!GetHAL().init()) {
        while (true) {
            GetHAL().delay(1000);
        }
    }

    ClockPower::instance().begin();

    // A blank production NVS shows the shipping logo once, records the marker,
    // and powers the unit off before any normal application is started.
    AppShutdown::handleFactoryBoot();

    // Build the complete startup frame in memory before the first panel update.
    drawBackground();

    // Install data apps before the shutdown controller so its modal state is
    // evaluated after the normal application updates in each frame.
    // Clock mode owns gestures before underlying apps can consume short clicks.
    mooncake::GetMooncake().installApp(std::make_unique<AppClock>());
    mooncake::GetMooncake().installApp(std::make_unique<AppWifiScan>());
    mooncake::GetMooncake().installApp(std::make_unique<AppBattery>());
    mooncake::GetMooncake().installApp(std::make_unique<AppTfCard>());
    mooncake::GetMooncake().installApp(std::make_unique<AppShutdown>());

    // Button border feedback owns a dedicated high-priority input display task.
    static ButtonFeedback button_feedback;

    // Run the initial Mooncake lifecycle before the first panel update.
    mooncake::GetMooncake().update();
    refreshDisplay();
    if (!button_feedback.begin()) {
        ESP_LOGE(APP_TAG, "Button feedback task initialization failed");
    }

    while (true) {
        ClockPower::instance().beginFrame();
        M5.update();
        mooncake::GetMooncake().update();
        refreshDisplay();
        ClockPower::instance().idle();
    }
}
