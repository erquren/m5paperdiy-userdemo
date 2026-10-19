/* SPDX-License-Identifier: MIT */
#include "clock_radio.h"
#include "time_service.h"
#include "time_policy.h"
#include <network/wifi_manager.h>
#include <esp_timer.h>

namespace {
constexpr std::int64_t WINDOW_US = 90LL * clock_policy::SECOND_US;
constexpr std::int64_t RETRY_US = clock_policy::COOLDOWN_US;
}

struct ClockRadio::Impl {
    bool active = false;
    bool window = false;
    std::int64_t deadline_us = 0;
    // Kept across UI transitions so repeated enter/leave cannot reset a failed
    // or cancelled window into continuous retries. A real new sync clears it.
    std::int64_t retry_at_us = 0;
    std::uint32_t seen_success = 0;
    std::uint32_t window_success = 0;

    void closeWindow(bool success, std::int64_t now)
    {
        TimeService::instance().setNetworkEnabled(false);
        WifiManager::instance().setClockSyncEnabled(false);
        window = false;
        deadline_us = 0;
        retry_at_us = success ? 0 : now + RETRY_US;
    }
    void advance()
    {
        const auto now = esp_timer_get_time();
        const auto time = TimeService::instance().snapshot();
        if (time.sync_generation != seen_success) {
            seen_success = time.sync_generation;
            retry_at_us = 0;
        }
        if (!active) return;
        if (window) {
            if (time.sync_generation != window_success) closeWindow(true, now);
            else if (now >= deadline_us) closeWindow(false, now);
            return;
        }
        const bool due = !time.valid || now - time.last_successful_sync_us >= clock_policy::DAY_US;
        if (!due || now < retry_at_us) return;
        // With no saved network there is nothing to calibrate through. Never
        // open a hotspot or scan automatically from the clock.
        if (!WifiManager::instance().snapshot().has_saved_credentials) {
            retry_at_us = now + RETRY_US;
            return;
        }
        window = true;
        window_success = time.sync_generation;
        deadline_us = now + WINDOW_US;
        WifiManager::instance().setClockSyncEnabled(true);
    }
    void updateTimeGate()
    {
        const auto wifi = WifiManager::instance().snapshot();
        // Raw IP events cannot reopen the time service while the driver is off,
        // stopping, or not yet validated against the saved SSID/BSSID/config.
        const bool allowed = wifi.station_connected && (!active || window);
        TimeService::instance().setNetworkEnabled(allowed, true, active && window ? deadline_us : 0);
    }
};

ClockRadio::ClockRadio()
{
    static Impl implementation;
    _impl = &implementation;
}
ClockRadio& ClockRadio::instance()
{
    static ClockRadio radio;
    return radio;
}
bool ClockRadio::active() const { return _impl->active; }
bool ClockRadio::sleepSafe() const
{
    return _impl->active && !_impl->window && WifiManager::instance().snapshot().radio_idle;
}
std::int64_t ClockRadio::nextWakeDeadlineUs() const
{
    const auto now = esp_timer_get_time();
    if (!_impl->active || _impl->window) return now;
    const auto time = TimeService::instance().snapshot();
    const auto next_sync = time.valid ? time.last_successful_sync_us + clock_policy::DAY_US : now;
    return std::max(next_sync, _impl->retry_at_us);
}
void ClockRadio::setActive(bool active)
{
    auto& self = *_impl;
    if (self.active == active) return;
    // A startup gesture can precede the network app's first lifecycle call.
    // Initialize through the existing owners before inspecting saved networks.
    if (active && WifiManager::instance().begin()) TimeService::instance().begin();
    // A response already accepted before this gesture still establishes a real
    // anchor. Fence all subsequent callbacks before requesting driver stop.
    TimeService::instance().tick();
    self.advance();
    if (!active && self.window) self.closeWindow(false, esp_timer_get_time());
    TimeService::instance().setNetworkEnabled(false);
    self.active = active;
    WifiManager::instance().setClockMode(active);
    self.advance();
}
void ClockRadio::tick()
{
    auto& self = *_impl;
    // Consume a response received before the overall deadline before declaring
    // failure, then enforce that deadline before any reconnect/scan work.
    TimeService::instance().tick();
    self.advance();
    WifiManager::instance().tick();
    self.updateTimeGate();
    TimeService::instance().tick();
    self.advance();
}
