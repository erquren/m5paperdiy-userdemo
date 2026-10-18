/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <atomic>
#include <mooncake.h>

class AppShutdown : public mooncake::AppAbility {
public:
    static void handleFactoryBoot();
    static bool isDialogOpen() { return _dialog_open.load(std::memory_order_acquire); }
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;

private:
    enum class State : std::uint8_t {
        Idle,
        Arming,
        Countdown,
        WaitForRelease,
    };

    bool showDialog();
    void drawCountdown(unsigned int seconds);
    void dismissDialog();
    [[noreturn]] void shutDown();
    static bool displayShutdownLogo();
    [[noreturn]] static void sendPowerOffCommand();

    inline static std::atomic<bool> _dialog_open{false};
    State _state                        = State::Idle;
    std::uint32_t _arming_started_ms    = 0;
    std::uint32_t _countdown_started_ms = 0;
    unsigned int _last_countdown        = 0;
    std::uint16_t* _saved_pixels        = nullptr;
};
