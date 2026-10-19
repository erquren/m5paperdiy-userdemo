#include "test_sdk.h"
#include <clock/clock_power.h>
#include <clock/clock_radio.h>
#include <clock/time_service.h>
#include <string>

ClockRadio::ClockRadio() : _impl(nullptr) {}
ClockRadio& ClockRadio::instance() { static ClockRadio radio; return radio; }
bool ClockRadio::sleepSafe() const { return mock::radio_safe; }
std::int64_t ClockRadio::nextWakeDeadlineUs() const { return mock::radio_deadline; }
TimeService::TimeService() : _impl(nullptr) {}
TimeService& TimeService::instance() { static TimeService service; return service; }
TimeService::Snapshot TimeService::snapshot() const { Snapshot s; s.sync_generation = mock::sync_generation; return s; }
std::int64_t TimeService::nextMinuteDeadlineUs() const { return mock::minute_deadline; }

static void expectAwake(ClockPower& power)
{
    const auto takes = mock::take_count;
    const auto feeds = mock::feed_count;
    power.idle();
    assert(mock::take_count == takes && mock::feed_count == feeds + 1);
}
static void expectMainLocked()
{
    assert(mock::held(ESP_PM_CPU_FREQ_MAX) == 1);
    assert(mock::held(ESP_PM_NO_LIGHT_SLEEP) == 1);
}
int main(int argc, char** argv)
{
    const std::string scenario = argc > 1 ? argv[1] : "normal";
    auto& hal = GetHAL();
    auto& power = ClockPower::instance();
    if (scenario == "no-button-wake") hal.wake_ready = false;
    if (scenario == "no-mutex") mock::fail_mutex = true;
    if (scenario == "create-failure") mock::create_fail_at = 3;
    if (scenario == "initial-lock-failure") mock::acquire_fail_at = 2;
    if (scenario == "first-retention-failure") mock::retention_fail_at = 1;
    if (scenario == "last-retention-failure") mock::retention_fail_at = 20;
    if (scenario == "gpio-a-failure") mock::gpio_fail_at = 1;
    if (scenario == "gpio-b-failure") mock::gpio_fail_at = 2;
    if (scenario == "wake-enable-failure") mock::fail_sleep_wake = true;
    if (scenario == "config-failure") mock::fail_configure = true;
    power.begin();
    power.beginFrame();
    assert(hal.wake_task == mock::task);
    const auto creates = mock::creates;
    power.begin();
    assert(creates == mock::creates); // Initialization is idempotent.
    if (scenario != "normal" && scenario != "work-lock-failure" && scenario != "release-failure" &&
        scenario != "reacquire-failure") {
        assert(!mock::config.light_sleep_enable && mock::config.min_freq_mhz == 240);
        expectAwake(power);
        assert(!hal.idle_mode);
        puts(("PASS safe awake fallback: " + scenario).c_str());
        return 0;
    }
    assert(mock::config.light_sleep_enable && mock::config.min_freq_mhz == 40 &&
           mock::config.max_freq_mhz == 240);
    assert(mock::retained_pins == (std::vector<int>{
        6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 45, 46, 38, 39, 47, 41, 42}));
    assert(mock::wake_pins == (std::vector<int>{4, 3}));
    expectMainLocked();
    if (scenario == "work-lock-failure") {
        mock::acquire_fail_at = mock::acquires + 2;
        { ClockPowerGuard work; }
        assert(!mock::config.light_sleep_enable && mock::config.min_freq_mhz == 240);
        expectAwake(power);
        puts("PASS work-lock failure fixes CPU at maximum and prohibits sleep");
        return 0;
    }
    if (scenario == "release-failure") {
        mock::release_fail_at = mock::releases + 2;
        power.idle();
        assert(!mock::config.light_sleep_enable && mock::config.min_freq_mhz == 240);
        assert(mock::take_count == 0);
        expectAwake(power);
        puts("PASS release failure stays awake");
        return 0;
    }
    if (scenario == "reacquire-failure") {
        mock::acquire_fail_at = mock::acquires + 1;
        power.idle();
        assert(!mock::config.light_sleep_enable && mock::config.min_freq_mhz == 240);
        expectAwake(power);
        puts("PASS wake reacquire failure disables further sleep/DFS");
        return 0;
    }
    mock::on_wait = [] {
        assert(mock::held(ESP_PM_CPU_FREQ_MAX) == 0);
        assert(mock::held(ESP_PM_NO_LIGHT_SLEEP) == 0);
    };
    power.idle();
    assert(mock::waited_ticks == 11234 && hal.idle_mode); // Fraction rounded down.
    expectMainLocked();
    mock::radio_deadline = mock::now + 750000;
    power.idle();
    assert(mock::waited_ticks == 750);
    mock::radio_deadline = 3600000000;

    // A notification racing between the idle checks and the wait is retained.
    mock::pending_notifications = 1;
    mock::on_wait = [] { assert(mock::pending_notifications == 1); };
    power.idle();
    assert(mock::pending_notifications == 0);
    expectMainLocked();
    mock::on_wait = {};

    mock::clock_active = false; expectAwake(power); assert(!hal.idle_mode); mock::clock_active = true;
    mock::dialog = true; expectAwake(power); mock::dialog = false;
    hal.shutdown = true; expectAwake(power); hal.shutdown = false;
    mock::usb = true; expectAwake(power); assert(!hal.idle_mode); mock::usb = false;
    mock::debugger = true; expectAwake(power); mock::debugger = false;
    hal.input_idle = false; expectAwake(power); hal.input_idle = true;
    hal.wake_ready = false; expectAwake(power); hal.wake_ready = true;
    mock::radio_safe = false; expectAwake(power); mock::radio_safe = true;
    hal.requestRefresh(); expectAwake(power); hal.clearRefreshRequest();
    mock::minute_deadline = mock::now + 1999; power.beginFrame(); expectAwake(power);
    mock::minute_deadline = mock::now - 1; power.beginFrame(); expectAwake(power);
    // A minute rolling during render/waveform must not be skipped by recomputing
    // the deadline from the newer minute at idle time.
    mock::minute_deadline = mock::now + 1000;
    power.beginFrame();
    mock::now += 2000;
    mock::minute_deadline = mock::now + 60000000;
    expectAwake(power);
    power.beginFrame();
    // AppClock already rendered when the subsequent radio tick accepts SNTP.
    ++mock::sync_generation;
    expectAwake(power);
    power.beginFrame();
    {
        ClockPowerGuard benchmark;
        expectAwake(power);
        assert(mock::held(ESP_PM_CPU_FREQ_MAX) == 2);
        {
            ClockPowerGuard nested;
            assert(mock::held(ESP_PM_CPU_FREQ_MAX) == 3);
            assert(mock::held(ESP_PM_NO_LIGHT_SLEEP) == 3);
        }
        assert(mock::held(ESP_PM_CPU_FREQ_MAX) == 2);
    }
    expectMainLocked();
    const auto early_return = [] { ClockPowerGuard operation; return 7; };
    assert(early_return() == 7);
    expectMainLocked();

    // Exercise actual production HAL partial/full submission and wait, while
    // the main locks are released, so the waveform cannot borrow their safety.
    mock::on_wait = [&] {
        assert(mock::held(ESP_PM_CPU_FREQ_MAX) == 0);
        mock::on_display_wait = [&] {
            assert(mock::held(ESP_PM_CPU_FREQ_MAX) == 1);
            assert(mock::held(ESP_PM_NO_LIGHT_SLEEP) == 1);
        };
        assert(!hal.flushDisplayRegion(8, 10, 20, 30, m5gfx::epd_mode_t::epd_text));
        assert(!hal.display.busy && hal.display.completions == 1);
        hal.flushFullDisplay(m5gfx::epd_mode_t::epd_quality);
        assert(!hal.display.busy && hal.display.completions == 2);
        for (unsigned n = 1; n <= 40; ++n) {
            assert(hal.flushDisplayRegion(8, 10, 20, 30, m5gfx::epd_mode_t::epd_text) == (n == 40));
            assert(!hal.display.busy);
        }
        assert(hal._partial_refresh_count == 0);
        assert(mock::held(ESP_PM_CPU_FREQ_MAX) == 0);
        assert(mock::held(ESP_PM_NO_LIGHT_SLEEP) == 0);
    };
    power.idle();
    expectMainLocked();
    mock::on_wait = {};
    mock::on_display_wait = {};

    // Clock feedback blocks without periodic wakes and resumes on clock exit.
    mock::task = reinterpret_cast<void*>(2);
    power.waitForClockExit();
    assert(mock::waited_ticks == portMAX_DELAY);
    mock::clock_active = false;
    mock::task = reinterpret_cast<void*>(1);
    power.idle();
    assert(mock::feedback_notifications == 1 && mock::notified_task == reinterpret_cast<void*>(2));
    power.idle();
    assert(mock::feedback_notifications == 1);
    for (int cycle = 0; cycle < 4; ++cycle) {
        mock::clock_active = true;
        mock::task = reinterpret_cast<void*>(2);
        power.waitForClockExit();
        assert(mock::waited_ticks == portMAX_DELAY);
        mock::clock_active = false;
        mock::task = reinterpret_cast<void*>(1);
        power.idle();
        assert(mock::feedback_notifications == cycle + 2);
    }
    const auto takes = mock::take_count;
    power.waitForClockExit(); // Exit before the wait skips blocking.
    assert(mock::take_count == takes);
    puts("PASS production clock PM policy, deadlines, debug/failure gates, work locks, EPD completion and feedback wake");
}
