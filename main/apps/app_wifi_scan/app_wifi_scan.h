/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <esp_event.h>
#include <mooncake.h>
#include <string>
#include <vector>

class AppWifiScan : public mooncake::AppAbility {
public:
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;

private:
    struct WifiNetwork {
        std::string ssid;
        std::int32_t rssi = 0;
    };

    static void wifiEventHandler(void* arg, esp_event_base_t event_base, std::int32_t event_id, void* event_data);

    bool initWifi();
    void startScan();
    void finishScan();
    void drawStatus(const char* text, bool large = false);
    void drawResults();

    std::vector<WifiNetwork> _networks;
    esp_event_handler_instance_t _scan_event_instance = nullptr;
    std::atomic<bool> _scan_done{false};
    std::atomic<std::uint32_t> _scan_status{0};
    bool _wifi_ready   = false;
    bool _scan_running = false;
};
