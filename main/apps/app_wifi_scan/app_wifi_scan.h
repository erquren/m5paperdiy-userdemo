/* SPDX-License-Identifier: MIT */
#pragma once
#include <atomic>
#include <cstdint>
#include <mooncake.h>
#include <network/wifi_manager.h>

class AppWifiScan : public mooncake::AppAbility {
public:
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    static bool isModal() { return _modal.load(std::memory_order_acquire); }
private:
    void enterSetup();
    void leaveSetup();
    void drawSetup(const WifiManager::Snapshot& snapshot, bool full);
    void drawOverview(const WifiManager::Snapshot& snapshot);
    inline static std::atomic<bool> _modal{false};
    std::string _local_error;
    std::uint16_t* _saved_pixels = nullptr;
    int _saved_width = 0;
    int _saved_height = 0;
    std::uint32_t _revision = UINT32_MAX;
    std::uint32_t _last_minute = UINT32_MAX;
};
