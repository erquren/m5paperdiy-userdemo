/* SPDX-License-Identifier: MIT */
#pragma once

// Main-task power policy. Automatic light sleep lets FreeRTOS and peripheral
// PM locks decide when both cores are idle; it never suspends another task or
// enters deep sleep. Setup failure keeps the original awake, 240-MHz behavior.
class ClockPower {
public:
    static ClockPower& instance();
    void begin();
    // Capture before app updates/rendering; crossing a minute or accepting a
    // new sync later in this frame must not defer that redraw until next minute.
    void beginFrame();
    void idle();
    // The hidden border task has no work while the clock owns the screen.
    // An exit notification resumes it without periodic idle polling.
    void waitForClockExit();

private:
    ClockPower();
    bool lockWork();
    void unlockWork();
    struct Impl;
    Impl* _impl;
    friend class ClockPowerGuard;
};

// Task context only. Covers the COMPLETE operation, including asynchronous EPD
// waveform completion, and all early-return paths of the TF benchmark.
class ClockPowerGuard {
public:
    ClockPowerGuard() : _locked(ClockPower::instance().lockWork()) {}
    ~ClockPowerGuard() { if (_locked) ClockPower::instance().unlockWork(); }
    ClockPowerGuard(const ClockPowerGuard&) = delete;
    ClockPowerGuard& operator=(const ClockPowerGuard&) = delete;
private:
    bool _locked;
};
