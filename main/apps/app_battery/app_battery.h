/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstdint>
#include <mooncake.h>

class AppBattery : public mooncake::AppAbility {
public:
    void onCreate() override;
    void onOpen() override;
    void onRunning() override;

private:
    void startSampling(bool force_draw);
    void collectSample();
    void drawPendingVoltage();
    bool drawVoltage(std::int16_t battery_mv, bool force_draw);

    std::uint32_t _last_update_ms     = 0;
    std::uint32_t _last_sample_ms     = 0;
    std::int32_t _sample_sum          = 0;
    std::uint32_t _sample_count       = 0;
    std::uint32_t _valid_sample_count = 0;
    bool _sampling                    = false;
    bool _force_draw                  = false;
    bool _pending_draw                = false;
    bool _pending_force_draw          = false;
    std::int16_t _pending_mv          = -1;
    char _last_text[8]                = {};
};
