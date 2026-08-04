/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <hal.h>
#include <mooncake.h>

class AppTfCard : public mooncake::AppAbility {
public:
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;

private:
    void updateCard(bool force_draw, bool retry_mount = false);
    void drawInfo();
    void drawStatus(const char* text);
    bool startBenchmark();
    static void benchmarkTask(void* argument);

    TfCardStatus _last_status = TfCardStatus::NoCard;
    TfBenchmarkResult _benchmark;
    std::uint32_t _last_poll_ms = 0;
    std::atomic<bool> _test_done{false};
    bool _test_running = false;
    bool _has_drawn    = false;
};
