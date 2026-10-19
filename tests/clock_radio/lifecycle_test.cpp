/* SPDX-License-Identifier: MIT */
#include "../../main/network/wifi_manager.h"
#include "../../main/clock/clock_radio.h"
#include "../../main/clock/time_service.h"
#include "../../main/clock/time_policy.h"
#include "sdk_mock.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace {
constexpr std::int64_t SECOND = 1000000;
constexpr std::int64_t HOUR = 3600 * SECOND;
constexpr std::int64_t DAY = 24 * HOUR;
auto& wifi = WifiManager::instance();
auto& time_service = TimeService::instance();
auto& radio = ClockRadio::instance();

void boot()
{
    assert(wifi.begin());
    assert(time_service.begin());
    assert(sdk::driver_running && sdk::starts == 1);
    radio.tick();
}
void at(std::int64_t value) { sdk::setNow(value); radio.tick(); }
void managerAt(std::int64_t value) { sdk::setNow(value); wifi.tick(); }
void finishStop()
{
    sdk::finishStop();
    radio.tick();
}
void connectNormally()
{
    at(sdk::now_us + SECOND);
    assert(sdk::connects > 0);
    sdk::associate();
    sdk::gotIp();
    radio.tick();
    assert(wifi.snapshot().station_connected);
    assert(sdk::sntp_running);
}
std::int64_t syncNormally()
{
    connectNormally();
    sdk::setNow(sdk::now_us + 2 * SECOND);
    sdk::reply();
    radio.tick();
    const auto result = time_service.snapshot();
    assert(result.valid && result.sync_generation == 1);
    assert(result.last_successful_sync_us == sdk::now_us);
    assert(!sdk::sntp_running);
    return result.last_successful_sync_us;
}
void assertQuiet(unsigned starts, unsigned connects, unsigned scans, unsigned aps)
{
    assert(sdk::starts == starts);
    assert(sdk::connects == connects);
    assert(sdk::scans == scans);
    assert(sdk::ap_enables == aps);
}
void managerSuspendResume()
{
    boot();
    connectNormally();
    // The TimeService gate is disabled explicitly because this scenario tests
    // WifiManager in isolation, including its public ownership API.
    time_service.setNetworkEnabled(false);
    wifi.requestScan();  // Queued work before the mode transition is revoked.
    wifi.setClockMode(true);
    assert(sdk::stops == 1 && !sdk::driver_running && sdk::stop_pending);
    assert(wifi.snapshot().clock_mode && !wifi.snapshot().radio_on);
    assert(!wifi.snapshot().station_connected);
    assert(!wifi.startProvisioning());
    for (unsigned i = 0; i < 64; ++i) {
        wifi.requestScan();
        sdk::disconnect();
        sdk::gotIp();
        sdk::scanDone();
        sdk::emit(WIFI_EVENT, WIFI_EVENT_AP_START);
    }
    managerAt(sdk::now_us + 2 * HOUR);
    assertQuiet(1, 1, 0, 0);
    assert(!wifi.snapshot().station_connected && !wifi.snapshot().provisioning);
    // Exiting the clock cannot race the asynchronous STA_STOP boundary.
    wifi.setClockMode(false);
    wifi.tick();
    assertQuiet(1, 1, 0, 0);
    sdk::finishStop();
    wifi.tick();
    assert(sdk::starts == 2 && sdk::driver_running);
    managerAt(sdk::now_us + SECOND);
    assert(sdk::connects == 2);
    sdk::associate(); sdk::gotIp(); wifi.tick();
    assert(wifi.snapshot().station_connected && !wifi.snapshot().clock_mode);
    wifi.requestScan(); wifi.tick();
    assert(sdk::scans == 1);  // Ordinary scan behavior is restored.
}
void successfulDailyCadence()
{
    boot();
    const auto first_sync = syncNormally();
    const auto first_epoch = time_service.snapshot().epoch_seconds;
    radio.setActive(true);
    assert(radio.active() && sdk::stops == 1 && !sdk::driver_running);
    assert(!radio.sleepSafe() && !wifi.snapshot().radio_idle);
    finishStop();
    assert(radio.sleepSafe() && wifi.snapshot().radio_idle);
    assert(radio.nextWakeDeadlineUs() == first_sync + DAY);
    at(first_sync + DAY - 1);
    assertQuiet(1, 1, 0, 0);
    assert(time_service.snapshot().epoch_seconds == first_epoch + 86399);
    // Exact 24 h due from accepted SNTP, not GOT_IP or opening the clock.
    at(first_sync + DAY);
    assert(sdk::starts == 2 && sdk::driver_running);
    assert(!radio.sleepSafe() && radio.nextWakeDeadlineUs() == sdk::now_us);
    connectNormally();
    assert(time_service.snapshot().sync_generation == 1);  // GOT_IP is not sync success.
    assert(sdk::driver_running);
    sdk::setNow(sdk::now_us + 3 * SECOND);
    const auto second_sync = sdk::now_us;
    sdk::reply(1792339199 + 100);  // Wall-clock backwards step must not shift cadence.
    radio.tick();
    assert(time_service.snapshot().sync_generation == 2);
    assert(time_service.snapshot().last_successful_sync_us == second_sync);
    assert(sdk::stops == 2 && !sdk::driver_running && !sdk::sntp_running);
    finishStop();
    const auto stops = sdk::stops;
    sdk::lateReply(); sdk::gotIp(); sdk::disconnect(); sdk::scanDone();
    wifi.requestScan();
    at(second_sync + DAY - 1);
    assertQuiet(2, 2, 0, 0);
    assert(sdk::stops == stops && time_service.snapshot().sync_generation == 2);
    at(second_sync + DAY);
    assert(sdk::starts == 3);
}
void unsynchronizedCombinedWindow()
{
    boot();
    radio.setActive(true);
    assert(sdk::stops == 1 && !sdk::driver_running);
    // Driver stop latency and association consume the same overall budget.
    at(20 * SECOND);
    assert(sdk::starts == 1);
    finishStop();
    assert(sdk::starts == 2);
    at(70 * SECOND);
    assert(sdk::connects == 1);
    sdk::associate(); sdk::gotIp(); radio.tick();
    assert(sdk::sntp_running && !time_service.snapshot().valid);
    at(90 * SECOND - 1);
    assert(sdk::driver_running);
    sdk::setNow(90 * SECOND);
    sdk::reply();  // A reply at the combined cutoff must not establish trust.
    radio.tick();
    assert(sdk::stops == 2 && !sdk::driver_running && !sdk::sntp_running);
    assert(!time_service.snapshot().valid && time_service.snapshot().sync_generation == 0);
    assert(!radio.sleepSafe() && !wifi.snapshot().radio_idle);
    const auto stop_position = std::find(sdk::operations.rbegin(), sdk::operations.rend(), "wifi_stop");
    assert(stop_position != sdk::operations.rend());
    const auto sntp_position = std::find(sdk::operations.rbegin(), sdk::operations.rend(), "sntp_stop");
    assert(sntp_position != sdk::operations.rend() && sntp_position > stop_position);
    finishStop();
    assert(radio.sleepSafe() && wifi.snapshot().radio_idle);
    assert(radio.nextWakeDeadlineUs() == 90 * SECOND + HOUR);
    at(90 * SECOND + HOUR - 1);
    assert(sdk::starts == 2);
    at(90 * SECOND + HOUR);
    assert(sdk::starts == 3);
}
void noIpSuccess()
{
    boot();
    radio.setActive(true);
    finishStop();
    at(SECOND);
    // GOT_IP without the matching association cannot enable SNTP or close a window.
    sdk::gotIp(); radio.tick();
    assert(!wifi.snapshot().station_connected && !time_service.snapshot().valid);
    assert(sdk::sntp_starts == 0 && sdk::driver_running);
    at(90 * SECOND);
    assert(!sdk::driver_running && sdk::stops == 2);
    assert(!time_service.snapshot().valid);
}
void portalCloses()
{
    boot(); syncNormally();
    assert(wifi.startProvisioning());
    sdk::apStarted();
    radio.tick();
    assert(wifi.provisioning() && sdk::http_starts == 1 && sdk::dns_starts == 1);
    assert(sdk::scans == 1);
    sdk::block_http_stop.store(true);
    radio.setActive(true);
    assert(!wifi.provisioning());
    assert(wifi.snapshot().ap_ssid.empty() && wifi.snapshot().ap_password.empty());
    assert(!sdk::driver_running && sdk::stops == 1 && sdk::dns_stops == 1);
    assert(!radio.sleepSafe() && !wifi.snapshot().radio_idle);
    finishStop();
    // STOP alone is insufficient while the old HTTP server is still closing.
    assert(!radio.sleepSafe() && !wifi.snapshot().radio_idle && sdk::http_stops == 0);
    sdk::block_http_stop.store(false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!radio.sleepSafe() && std::chrono::steady_clock::now() < deadline) {
        radio.tick();
        std::this_thread::yield();
    }
    assert(sdk::http_stops == 1);
    assert(radio.sleepSafe() && wifi.snapshot().radio_idle);
    // Late AP/scan notifications cannot resurrect the setup services.
    sdk::emit(WIFI_EVENT, WIFI_EVENT_AP_START);
    sdk::scanDone(); wifi.requestScan(); radio.tick();
    assert(sdk::http_starts == 1 && sdk::ap_enables == 1 && sdk::scans == 1);
}
void longAQueuedAcrossStop()
{
    boot(); syncNormally();
    radio.setActive(true);
    assert(sdk::stop_pending);
    radio.setActive(false);  // Same order used before opening long-A setup UI.
    assert(!radio.active());
    assert(wifi.startProvisioning());
    assert(sdk::starts == 1 && sdk::ap_enables == 0);
    radio.tick();
    assert(sdk::starts == 1 && sdk::ap_enables == 0);
    finishStop();
    assert(sdk::starts == 2 && sdk::ap_enables == 1 && wifi.provisioning());
    sdk::apStarted(); radio.tick();
    assert(sdk::http_starts == 1 && sdk::dns_starts == 1);
}
void noCredentialsStayOff()
{
    sdk::saved_credentials = false;
    boot();
    radio.setActive(true);
    finishStop();
    for (unsigned i = 1; i <= 48; ++i) at(i * HOUR);
    assertQuiet(1, 0, 0, 0);
    assert(!time_service.snapshot().valid && !wifi.provisioning());
}
void reentryPreservesCooldown()
{
    boot(); radio.setActive(true); finishStop();
    at(SECOND);
    assert(sdk::starts == 2 && sdk::connects == 1);
    at(90 * SECOND);
    finishStop();
    at(100 * SECOND);
    radio.setActive(false);
    radio.tick();
    assert(sdk::starts == 3);
    radio.setActive(true);
    assert(sdk::stops == 3);
    finishStop();
    at(90 * SECOND + HOUR - 1);
    assert(sdk::starts == 3);
    at(90 * SECOND + HOUR);
    assert(sdk::starts == 4);
}
void overflowDoesNotLoseStop()
{
    boot();
    // Saturate the manager's 32-entry queue, then stop before draining it.
    for (unsigned i = 0; i < 64; ++i) sdk::scanDone();
    wifi.setClockMode(true);
    wifi.setClockSyncEnabled(true);
    sdk::finishStop();
    wifi.tick();
    assert(sdk::starts == 2 && sdk::driver_running);
    assert(sdk::scans == 0 && sdk::ap_enables == 0);
}
void stopFailureRetriesSafely()
{
    boot();
    sdk::fail_stops = 1;
    wifi.setClockMode(true);
    assert(sdk::stops == 1 && sdk::driver_running);
    assert(wifi.snapshot().radio_on && !wifi.snapshot().station_connected);
    wifi.requestScan();
    managerAt(SECOND - 1);
    assert(sdk::stops == 1 && sdk::connects == 0 && sdk::scans == 0);
    managerAt(SECOND);
    assert(sdk::stops == 2 && !sdk::driver_running);
    assert(!wifi.snapshot().radio_on);
    sdk::finishStop(); wifi.tick();
    managerAt(HOUR);
    assert(sdk::starts == 1 && sdk::connects == 0 && sdk::scans == 0);
}
void reconnectsCannotExtendWindow()
{
    boot(); radio.setActive(true); finishStop();
    connectNormally();
    for (const auto second : {10, 30, 50, 70}) {
        at(second * SECOND);
        sdk::disconnect(); radio.tick();
        assert(!wifi.snapshot().station_connected && !sdk::sntp_running);
        at((second + 2) * SECOND);  // Normal reconnect backoff queues an attempt.
        at((second + 3) * SECOND);
        sdk::associate(); sdk::gotIp(); radio.tick();
        assert(wifi.snapshot().station_connected && sdk::sntp_running);
        assert(!time_service.snapshot().valid);
    }
    assert(sdk::connects == 5 && sdk::sntp_starts == 5);
    at(90 * SECOND - 1);
    assert(sdk::driver_running);
    at(90 * SECOND);
    assert(!sdk::driver_running && !sdk::sntp_running && sdk::stops == 2);
    assert(!time_service.snapshot().valid);
    finishStop();
    at(90 * SECOND + HOUR - 1);
    assert(sdk::starts == 2);
    at(90 * SECOND + HOUR);
    assert(sdk::starts == 3);
}
void earlyClockEntryInitializesServices()
{
    // A clock gesture may precede the network application's first onOpen.
    radio.setActive(true);
    assert(radio.active() && wifi.snapshot().has_saved_credentials);
    assert(sdk::starts == 1 && sdk::stops == 1 && !sdk::driver_running);
    finishStop();
    assert(sdk::starts == 2 && sdk::driver_running);
    connectNormally();
    sdk::reply(); radio.tick();
    assert(time_service.snapshot().valid && time_service.snapshot().sync_generation == 1);
    assert(sdk::stops == 2 && !sdk::driver_running);
}
void rapidUiTransitionsStayQuiet()
{
    boot(); radio.setActive(true); finishStop();
    at(90 * SECOND); finishStop();
    for (unsigned i = 0; i < 10; ++i) {
        radio.setActive(false);
        radio.setActive(true);  // Resume is cancelled before the main loop tick.
        radio.tick();
    }
    assert(sdk::starts == 2 && sdk::stops == 2 && !sdk::driver_running);
    at(90 * SECOND + HOUR - 1);
    assert(sdk::starts == 2);
    at(90 * SECOND + HOUR);
    assert(sdk::starts == 3);
}
void acceptedReplyBeforeCutoff()
{
    boot(); radio.setActive(true); finishStop();
    // Association can consume most of the total budget.
    at(70 * SECOND);
    sdk::associate(); sdk::gotIp(); radio.tick();
    assert(sdk::sntp_running);
    const auto receipt = 90 * SECOND - 1;
    sdk::setNow(receipt);
    sdk::reply();
    // The callback arrived on time, but the main task consumes it afterwards.
    at(90 * SECOND + 100000);
    assert(time_service.snapshot().valid);
    assert(time_service.snapshot().sync_generation == 1);
    assert(time_service.snapshot().last_successful_sync_us == receipt);
    assert(!sdk::driver_running && !sdk::sntp_running && sdk::stops == 2);
    finishStop();
    at(receipt + HOUR);  // Successful receipt must not take the failed-hour path.
    assert(sdk::starts == 2);
    at(receipt + DAY - 1);
    assert(sdk::starts == 2);
    at(receipt + DAY);
    assert(sdk::starts == 3);
}
void portalReopenWaitsForHttp()
{
    boot();
    assert(wifi.startProvisioning()); sdk::apStarted(); radio.tick();
    assert(sdk::http_starts == 1 && sdk::ap_enables == 1);
    sdk::block_http_stop.store(true);
    wifi.stopProvisioning();
    assert(!wifi.provisioning() && sdk::dns_stops == 1);
    assert(wifi.startProvisioning());
    assert(wifi.provisioning() && wifi.snapshot().ap_password.empty());
    for (unsigned i = 1; i <= 5; ++i) at(i * SECOND);
    assert(wifi.provisioning());  // Pending session is not expired at deadline zero.
    assert(sdk::ap_enables == 1 && sdk::http_starts == 1 && sdk::http_stops == 0);
    sdk::block_http_stop.store(false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (sdk::ap_enables < 2 && std::chrono::steady_clock::now() < deadline) {
        radio.tick();
        std::this_thread::yield();
    }
    assert(sdk::http_stops == 1 && sdk::ap_enables == 2 && wifi.provisioning());
    assert(!wifi.snapshot().ap_ssid.empty() && !wifi.snapshot().ap_password.empty());
    sdk::apStarted(); radio.tick();
    assert(sdk::http_starts == 2 && sdk::dns_starts == 2);
}
void restartFailureIsBounded()
{
    boot(); syncNormally(); radio.setActive(true); finishStop();
    sdk::fail_starts = 1;
    radio.setActive(false);
    radio.tick();
    const auto failed_at = sdk::now_us;
    assert(sdk::starts == 2 && !sdk::driver_running && !wifi.snapshot().radio_on);
    at(failed_at + 5 * SECOND - 1);
    assert(sdk::starts == 2 && !sdk::driver_running);
    at(failed_at + 5 * SECOND);
    assert(sdk::starts == 3 && sdk::driver_running && wifi.snapshot().radio_on);
    connectNormally();
    assert(wifi.snapshot().station_connected);
}
void lateDnsCannotReviveSntp()
{
    boot(); radio.setActive(true); finishStop();
    at(SECOND);
    sdk::defer_dns = true;
    sdk::associate(); sdk::gotIp(); radio.tick();
    assert(sdk::lookups == 1 && sdk::sntp_starts == 0);
    at(90 * SECOND);
    assert(!sdk::driver_running);
    sdk::finishDns(true); radio.tick();
    assert(sdk::sntp_starts == 0 && !time_service.snapshot().valid);
}
}
int main(int argc, char** argv)
{
    assert(argc == 2);
    const char* scenario = argv[1];
    if (!std::strcmp(scenario, "manager-suspend-resume")) managerSuspendResume();
    else if (!std::strcmp(scenario, "daily-success")) successfulDailyCadence();
    else if (!std::strcmp(scenario, "combined-window")) unsynchronizedCombinedWindow();
    else if (!std::strcmp(scenario, "got-ip-not-success")) noIpSuccess();
    else if (!std::strcmp(scenario, "portal-closes")) portalCloses();
    else if (!std::strcmp(scenario, "long-a-stop-boundary")) longAQueuedAcrossStop();
    else if (!std::strcmp(scenario, "no-credentials")) noCredentialsStayOff();
    else if (!std::strcmp(scenario, "reentry-cooldown")) reentryPreservesCooldown();
    else if (!std::strcmp(scenario, "overflow-stop-boundary")) overflowDoesNotLoseStop();
    else if (!std::strcmp(scenario, "stop-failure-retry")) stopFailureRetriesSafely();
    else if (!std::strcmp(scenario, "late-dns")) lateDnsCannotReviveSntp();
    else if (!std::strcmp(scenario, "pre-cutoff-delayed-consume")) acceptedReplyBeforeCutoff();
    else if (!std::strcmp(scenario, "portal-reopen-http-blocked")) portalReopenWaitsForHttp();
    else if (!std::strcmp(scenario, "restart-failure-retry")) restartFailureIsBounded();
    else if (!std::strcmp(scenario, "reconnect-window-bound")) reconnectsCannotExtendWindow();
    else if (!std::strcmp(scenario, "rapid-ui-transitions")) rapidUiTransitionsStayQuiet();
    else if (!std::strcmp(scenario, "early-clock-entry")) earlyClockEntryInitializesServices();
    else { assert(false && "unknown scenario"); }
    std::printf("PASS: production clock/radio lifecycle: %s\n", scenario);
}
