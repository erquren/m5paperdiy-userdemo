/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstdint>

// Main-task coordinator. WifiManager remains the sole Wi-Fi driver owner.
// Apps switch this policy with setActive(); tick() replaces separate Wi-Fi and
// TimeService ticks and continues running while any UI modal is visible.
class ClockRadio {
public:
    static ClockRadio& instance();
    void setActive(bool active);
    void tick();
    bool active() const;
    // Main-task idle scheduling; never sleep through radio stop or AP teardown.
    bool sleepSafe() const;
    std::int64_t nextWakeDeadlineUs() const;

private:
    ClockRadio();
    struct Impl;
    Impl* _impl;
};
