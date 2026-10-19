/* SPDX-License-Identifier: MIT */
#pragma once

#include <atomic>
#include <cstdint>
#include <clock/clock_render.h>
#include <mooncake.h>

class AppClock : public mooncake::AppAbility {
public:
    void onCreate() override;
    void onRunning() override;
    static bool isActive() { return _active.load(std::memory_order_acquire); }
    // Main-task only. Restore the prior screen/rotation before long-A setup
    // handling or the original portrait A+B shutdown controller takes over.
    static void leaveForSystemAction();
private:
    bool enter();
    void leave();
    void render(bool full);
    inline static std::atomic<bool> _active{false};
    inline static AppClock* _instance = nullptr;
    std::uint16_t* _saved_pixels = nullptr;
    int _saved_width = 0;
    int _saved_height = 0;
    std::uint8_t _saved_rotation = 0;
    clock_render::Content _last_content;
    bool _has_rendered = false;
};
