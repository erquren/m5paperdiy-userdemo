/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "app_wifi_scan.h"

#include <LGFX_Fonts.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <hal.h>
#include <nvs_flash.h>

namespace {

constexpr char APP_TAG[] = "AppWifiScan";

constexpr int WIFI_INFO_X = 121;
constexpr int WIFI_INFO_Y = 503;
constexpr int WIFI_INFO_W = 418 - WIFI_INFO_X;
constexpr int WIFI_INFO_H = 715 - WIFI_INFO_Y;

constexpr int WIFI_ROW_H          = 24;
constexpr int WIFI_LIST_PADDING_X = 8;
constexpr int WIFI_LIST_PADDING_Y = 9;
constexpr int WIFI_RSSI_W         = 72;

void requestWifiRefresh(m5gfx::epd_mode_t mode)
{
    if (!GetHAL().isFullRedrawRequested()) {
        GetHAL().requestRefresh(WIFI_INFO_X, WIFI_INFO_Y, WIFI_INFO_W, WIFI_INFO_H, mode);
    }
}

std::string fitSsidText(const std::string& ssid, int max_width)
{
    auto& display    = GetHAL().display;
    std::string text = ssid.empty() ? "<hidden>" : ssid;
    if (display.textWidth(text.c_str()) <= max_width) {
        return text;
    }

    while (!text.empty()) {
        std::size_t erase_at = text.size() - 1;
        while (erase_at > 0 && (static_cast<unsigned char>(text[erase_at]) & 0xC0) == 0x80) {
            --erase_at;
        }
        text.resize(erase_at);
        const std::string clipped = text + ".";
        if (display.textWidth(clipped.c_str()) <= max_width) {
            return clipped;
        }
    }
    return ".";
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                              App Lifecycle                                 */
/* -------------------------------------------------------------------------- */
void AppWifiScan::onCreate()
{
    setAppInfo().name = "AppWifiScan";
    open();
}

void AppWifiScan::onOpen()
{
    startScan();
}

void AppWifiScan::onRunning()
{
    if (!_scan_running && GetHAL().wasButtonAClicked()) {
        startScan();
        return;
    }

    if (!_scan_running || !_scan_done.exchange(false, std::memory_order_acquire)) {
        return;
    }

    _scan_running = false;
    if (_scan_status.load(std::memory_order_relaxed) != 0) {
        esp_wifi_clear_ap_list();
        ESP_LOGE(APP_TAG, "Wi-Fi scan failed");
        drawStatus("SCAN FAILED");
        return;
    }
    finishScan();
}

/* -------------------------------------------------------------------------- */
/*                               Wi-Fi Driver                                 */
/* -------------------------------------------------------------------------- */
void AppWifiScan::wifiEventHandler(void* arg, esp_event_base_t event_base, std::int32_t event_id, void* event_data)
{
    if (event_base != WIFI_EVENT || event_id != WIFI_EVENT_SCAN_DONE || event_data == nullptr) {
        return;
    }

    auto* app              = static_cast<AppWifiScan*>(arg);
    const auto* scan_event = static_cast<const wifi_event_sta_scan_done_t*>(event_data);
    app->_scan_status.store(scan_event->status, std::memory_order_relaxed);
    app->_scan_done.store(true, std::memory_order_release);
}

bool AppWifiScan::initWifi()
{
    esp_err_t error = nvs_flash_init();
    if (error == ESP_ERR_NVS_NO_FREE_PAGES || error == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
        error = nvs_flash_init();
    }
    if (error != ESP_OK) {
        ESP_LOGE(APP_TAG, "NVS initialization failed: %s", esp_err_to_name(error));
        return false;
    }

    error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(APP_TAG, "esp_netif initialization failed: %s", esp_err_to_name(error));
        return false;
    }

    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(APP_TAG, "Event loop initialization failed: %s", esp_err_to_name(error));
        return false;
    }

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    error                          = esp_wifi_init(&wifi_config);
    if (error != ESP_OK) {
        ESP_LOGE(APP_TAG, "Wi-Fi initialization failed: %s", esp_err_to_name(error));
        return false;
    }

    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK || esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_start() != ESP_OK) {
        ESP_LOGE(APP_TAG, "Wi-Fi station startup failed");
        return false;
    }

    error = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &AppWifiScan::wifiEventHandler, this,
                                                &_scan_event_instance);
    if (error != ESP_OK) {
        ESP_LOGE(APP_TAG, "Scan event registration failed: %s", esp_err_to_name(error));
        return false;
    }

    _wifi_ready = true;
    return true;
}

/* -------------------------------------------------------------------------- */
/*                               Scan Control                                 */
/* -------------------------------------------------------------------------- */
void AppWifiScan::startScan()
{
    if (_scan_running) {
        return;
    }
    if (!_wifi_ready && !initWifi()) {
        drawStatus("WIFI INIT FAILED");
        return;
    }

    wifi_scan_config_t scan_config   = {};
    scan_config.show_hidden          = true;
    scan_config.scan_type            = WIFI_SCAN_TYPE_ACTIVE;
    scan_config.scan_time.active.min = 0;
    scan_config.scan_time.active.max = 120;

    _scan_done.store(false, std::memory_order_relaxed);
    const esp_err_t error = esp_wifi_scan_start(&scan_config, false);
    if (error != ESP_OK) {
        ESP_LOGE(APP_TAG, "Unable to start Wi-Fi scan: %s", esp_err_to_name(error));
        drawStatus("SCAN START FAILED");
        return;
    }

    _scan_running = true;
    drawStatus("SCANNING...", true);
}

void AppWifiScan::finishScan()
{
    std::uint16_t record_count = 0;
    esp_err_t error            = esp_wifi_scan_get_ap_num(&record_count);
    if (error != ESP_OK) {
        ESP_LOGE(APP_TAG, "Unable to read scan count: %s", esp_err_to_name(error));
        esp_wifi_clear_ap_list();
        drawStatus("SCAN FAILED");
        return;
    }

    std::vector<wifi_ap_record_t> records(record_count);
    if (record_count > 0) {
        error = esp_wifi_scan_get_ap_records(&record_count, records.data());
        if (error != ESP_OK) {
            ESP_LOGE(APP_TAG, "Unable to read scan results: %s", esp_err_to_name(error));
            esp_wifi_clear_ap_list();
            drawStatus("SCAN FAILED");
            return;
        }
    } else {
        esp_wifi_clear_ap_list();
    }

    _networks.clear();
    _networks.reserve(record_count);
    for (std::uint16_t i = 0; i < record_count; ++i) {
        char ssid[sizeof(records[i].ssid) + 1] = {};
        std::memcpy(ssid, records[i].ssid, sizeof(records[i].ssid));
        _networks.push_back({ssid, records[i].rssi});
    }
    std::sort(_networks.begin(), _networks.end(),
              [](const WifiNetwork& left, const WifiNetwork& right) { return left.rssi > right.rssi; });

    ESP_LOGI(APP_TAG, "Wi-Fi scan complete: %u networks", record_count);
    drawResults();
}

/* -------------------------------------------------------------------------- */
/*                              Scan Rendering                                */
/* -------------------------------------------------------------------------- */
void AppWifiScan::drawStatus(const char* text, bool large)
{
    auto& hal = GetHAL();
    DisplayGuard guard(hal);
    auto& display = hal.display;
    display.setEpdMode(m5gfx::epd_mode_t::epd_text);
    display.fillRect(WIFI_INFO_X, WIFI_INFO_Y, WIFI_INFO_W, WIFI_INFO_H, TFT_WHITE);
    display.setTextColor(TFT_BLACK, TFT_WHITE);
    display.setTextDatum(middle_center);
    display.setTextSize(1.0f);
    display.setFont(large ? &fonts::lvFontMontserrat20 : &fonts::lvFontMontserrat18);
    display.setClipRect(WIFI_INFO_X, WIFI_INFO_Y, WIFI_INFO_W, WIFI_INFO_H);
    display.drawString(text, WIFI_INFO_X + WIFI_INFO_W / 2, WIFI_INFO_Y + WIFI_INFO_H / 2);
    display.clearClipRect();
    requestWifiRefresh(m5gfx::epd_mode_t::epd_text);
}

void AppWifiScan::drawResults()
{
    auto& hal = GetHAL();
    DisplayGuard guard(hal);
    auto& display = hal.display;
    display.setEpdMode(m5gfx::epd_mode_t::epd_quality);
    display.fillRect(WIFI_INFO_X, WIFI_INFO_Y, WIFI_INFO_W, WIFI_INFO_H, TFT_WHITE);
    display.setTextColor(TFT_BLACK, TFT_WHITE);
    display.setTextSize(1.0f);
    display.setFont(&fonts::lvFontMontserrat18);
    display.setClipRect(WIFI_INFO_X, WIFI_INFO_Y, WIFI_INFO_W, WIFI_INFO_H);

    if (_networks.empty()) {
        display.setTextDatum(middle_center);
        display.drawString("NO NETWORKS", WIFI_INFO_X + WIFI_INFO_W / 2, WIFI_INFO_Y + WIFI_INFO_H / 2);
    } else {
        const int visible_rows = (WIFI_INFO_H - WIFI_LIST_PADDING_Y * 2) / WIFI_ROW_H;
        const int rssi_right   = WIFI_INFO_X + WIFI_INFO_W - 8;
        const int ssid_x       = WIFI_INFO_X + WIFI_LIST_PADDING_X;
        const int ssid_w       = rssi_right - WIFI_RSSI_W - ssid_x - 4;

        for (int row = 0; row < visible_rows && row < static_cast<int>(_networks.size()); ++row) {
            const auto& network    = _networks[row];
            const int row_y        = WIFI_INFO_Y + WIFI_LIST_PADDING_Y + row * WIFI_ROW_H;
            const int row_center_y = row_y + WIFI_ROW_H / 2;
            const std::string ssid = fitSsidText(network.ssid, ssid_w);
            char rssi_text[16]     = {};
            std::snprintf(rssi_text, sizeof(rssi_text), "%lddBm", static_cast<long>(network.rssi));

            display.setTextDatum(middle_left);
            display.drawString(ssid.c_str(), ssid_x, row_center_y);
            display.setTextDatum(middle_right);
            display.drawString(rssi_text, rssi_right, row_center_y);
        }
    }

    display.clearClipRect();
    requestWifiRefresh(m5gfx::epd_mode_t::epd_quality);
}
