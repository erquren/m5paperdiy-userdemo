/* SPDX-License-Identifier: MIT */
#include "app_wifi_scan.h"
#include <clock/time_service.h>
#include <app_shutdown/app_shutdown.h>
#include <LGFX_Fonts.hpp>
#include <cstdio>
#include <esp_heap_caps.h>
#include <hal.h>

namespace {
std::string clipped(const std::string& text, int width)
{
    std::string result = text;
    auto& display = GetHAL().display;
    while (!result.empty() && display.textWidth(result.c_str()) > width) {
        std::size_t at = result.size() - 1;
        while (at > 0 && (static_cast<unsigned char>(result[at]) & 0xc0) == 0x80) --at;
        result.resize(at);
    }
    return result;
}
}
void AppWifiScan::onCreate() { setAppInfo().name = "AppWifiScan"; open(); }
void AppWifiScan::onOpen()
{
    if (WifiManager::instance().begin()) {
        TimeService::instance().begin();
        WifiManager::instance().requestScan();
    }
    _revision = UINT32_MAX;
}
void AppWifiScan::onRunning()
{
    auto& hal = GetHAL();
    auto& wifi = WifiManager::instance();
    wifi.tick();
    TimeService::instance().tick();
    // Keep networking alive, but never paint over the shutdown overlay.
    if (AppShutdown::isDialogOpen() || hal.isShutdownInProgress()) return;
    if (isModal()) {
        // Restore the normal screen before the existing A+B shutdown dialog opens.
        if ((hal.isButtonPressed(ButtonId::A) && hal.isButtonPressed(ButtonId::B)) ||
            hal.wasButtonBClicked() || hal.wasButtonALongPressed()) {
            wifi.stopProvisioning();
        }
        if (!wifi.provisioning()) leaveSetup();
    } else if (hal.wasButtonALongPressed()) {
        enterSetup();
    } else if (hal.wasButtonAClicked()) {
        _local_error.clear();
        wifi.requestScan();
    }
    auto snapshot = wifi.snapshot();
    if (isModal()) {
        const auto minutes = (snapshot.seconds_remaining + 59) / 60;
        if (snapshot.revision != _revision || minutes != _last_minute) {
            drawSetup(snapshot, false);
            _last_minute = minutes;
        }
    } else if (snapshot.revision != _revision) {
        drawOverview(snapshot);
    }
    _revision = snapshot.revision;
}
void AppWifiScan::enterSetup()
{
    auto& hal = GetHAL();
    auto& d = hal.display;
    _local_error.clear();
    _saved_width = d.width(); _saved_height = d.height();
    _saved_pixels = static_cast<std::uint16_t*>(heap_caps_malloc(
        static_cast<std::size_t>(_saved_width) * _saved_height * sizeof(std::uint16_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_saved_pixels) {
        // Do not consume scarce internal Wi-Fi heap for a full-screen backup.
        _local_error = "Setup needs free PSRAM";
        _revision = UINT32_MAX;
        return;
    }
    {
        DisplayGuard guard(hal);
        _modal.store(true, std::memory_order_release);
        d.waitDisplay();
        d.readRect(0, 0, _saved_width, _saved_height, _saved_pixels);
    }
    hal.suppressButtonActionsUntilRelease();
    if (!WifiManager::instance().startProvisioning()) { leaveSetup(); return; }
    _last_minute = UINT32_MAX;
    drawSetup(WifiManager::instance().snapshot(), true);
}
void AppWifiScan::leaveSetup()
{
    auto& hal = GetHAL();
    if (_saved_pixels) {
        DisplayGuard guard(hal);
        hal.display.waitDisplay();
        hal.display.pushImage(0, 0, _saved_width, _saved_height, _saved_pixels);
        hal.requestFullRedraw(m5gfx::epd_mode_t::epd_quality);
        _modal.store(false, std::memory_order_release);
        heap_caps_free(_saved_pixels); _saved_pixels = nullptr;
    }
    _modal.store(false, std::memory_order_release);
    hal.suppressButtonActionsUntilRelease();
    _revision = UINT32_MAX;
}
void AppWifiScan::drawSetup(const WifiManager::Snapshot& s, bool full)
{
    auto& hal = GetHAL();
    DisplayGuard guard(hal);
    auto& d = hal.display;
    d.setTextColor(TFT_BLACK, TFT_WHITE); d.setTextDatum(middle_center); d.setTextSize(1);
    if (full) {
        d.fillScreen(TFT_WHITE);
        d.setFont(&fonts::lvFontMontserrat28); d.drawString("WI-FI SETUP", 270, 42);
        d.setFont(&fonts::lvFontMontserrat18); d.drawString("1. Scan to join the device hotspot",270,88);
        std::string qr = "WIFI:T:WPA;S:" + s.ap_ssid + ";P:" + s.ap_password + ";;";
        d.qrcode(qr.c_str(),150,119,240,5,true);
        d.setFont(&fonts::lvFontMontserrat20); d.drawString(s.ap_ssid.c_str(),270,393);
        std::string password = "Password: " + s.ap_password;
        d.drawString(password.c_str(),270,427);
        d.setFont(&fonts::lvFontMontserrat18); d.drawString("2. Open the setup page",270,477);
        d.qrcode("http://192.168.4.1",190,508,160,3,true);
        d.drawString("http://192.168.4.1",270,697);
        d.setFont(&fonts::lvFontMontserrat14);
        d.drawString("If your phone says no internet, stay connected.",270,735);
        d.drawString("Choose your 2.4 GHz Wi-Fi in the phone page.",270,762);
        d.drawString("B: exit     A+B: existing power-off controls",270,918);
    }
    d.fillRect(12,790,516,105,TFT_WHITE);
    d.setFont(&fonts::lvFontMontserrat18);
    d.drawString(clipped(s.message,500).c_str(),270,815);
    d.setFont(&fonts::lvFontMontserrat14);
    char text[80];
    std::snprintf(text,sizeof(text),"Setup closes after %lu min of inactivity",
                  static_cast<unsigned long>((s.seconds_remaining+59)/60));
    d.drawString(text,270,862);
    if (full) hal.requestFullRedraw(m5gfx::epd_mode_t::epd_quality);
    else hal.requestRefresh(12,790,516,105,m5gfx::epd_mode_t::epd_text);
}
void AppWifiScan::drawOverview(const WifiManager::Snapshot& s)
{
    auto& hal = GetHAL(); DisplayGuard guard(hal); auto& d = hal.display;
    constexpr int x=121,y=503,w=297,h=212;
    d.fillRect(x,y,w,h,TFT_WHITE); d.setClipRect(x,y,w,h);
    d.setTextColor(TFT_BLACK,TFT_WHITE); d.setTextDatum(top_left); d.setTextSize(1);
    d.setFont(&fonts::lvFontMontserrat14);
    d.drawString("Hold A 3s: Wi-Fi setup",x+6,y+6);
    d.drawString(clipped(_local_error.empty() ? s.message : _local_error,w-12).c_str(),x+6,y+31);
    d.setFont(&fonts::lvFontMontserrat16);
    if (!s.ssid.empty()) d.drawString(clipped(s.ssid,w-12).c_str(),x+6,y+53);
    int row=0;
    for (const auto& net:s.networks) {
        if (row==5) break;
        char signal[20]; std::snprintf(signal,sizeof(signal),"%d",net.rssi);
        d.drawString(clipped(net.ssid.empty()?"<hidden>":net.ssid,w-65).c_str(),x+6,y+80+row*24);
        d.drawString(signal,x+w-46,y+80+row*24); ++row;
    }
    d.clearClipRect(); hal.requestRefresh(x,y,w,h,m5gfx::epd_mode_t::epd_text);
}
