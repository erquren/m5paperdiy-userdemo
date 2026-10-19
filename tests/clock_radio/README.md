# Production clock/radio lifecycle host tests

Run from the repository root:

```sh
bash tests/clock_radio/run.sh
```

The runner compiles the actual `main/network/wifi_manager.cpp`,
`main/clock/clock_radio.cpp`, and `main/clock/time_service.cpp` together. It uses
C++17, `-Wall -Wextra -Werror`, AddressSanitizer, and UndefinedBehaviorSanitizer.
Every scenario runs in a fresh process to reset the production singletons.
No policy implementation is copied into the tests. The same scenarios check
that the light-sleep eligibility API remains false through pending STOP, active
calibration, and blocked HTTP teardown; becomes true only after full quiescence;
and reports the correct daily-success or failed-hour wake deadline.

## Executable scenarios (17)

1. `manager-suspend-resume`: entering clock mode calls `esp_wifi_stop`; queued
   scans, new requests, and late disconnect/IP/scan/AP events cannot reconnect,
   scan, enable AP, or claim a station connection. Normal exit waits for
   `STA_STOP`, then restores connection and manual scanning.
2. `daily-success`: a valid clock powers down on entry; the next window starts
   exactly 24 hours after an accepted SNTP reply. GOT_IP is not success. A second
   accepted reply closes the radio and resets the monotonic daily anchor even
   when its wall-clock correction goes backwards. Offline time still advances.
3. `combined-window`: delayed STOP delivery and late association consume the
   same 90-second budget as SNTP. A reply at the cutoff is rejected; SNTP stops
   before the radio. Failed calibration retries after one hour.
4. `got-ip-not-success`: GOT_IP without the manager's matching association
   validation neither starts SNTP nor establishes a valid time.
5. `portal-closes`: entering the clock revokes an active AP session and its
   displayed credentials, stops DNS/HTTP and the radio, and rejects late portal
   notifications.
6. `long-a-stop-boundary`: opening setup immediately after exiting clock mode
   queues the portal request until the asynchronous STOP boundary completes.
7. `no-credentials`: an unsynchronized device without saved credentials remains
   radio-off for 48 simulated hours, with no automatic AP or scan.
8. `reentry-cooldown`: a completed normal exit and subsequent reentry preserve
   the failed window's original one-hour retry deadline.
9. `overflow-stop-boundary`: a saturated event queue cannot lose the independent
   STOP acknowledgement needed to safely restart the driver.
10. `stop-failure-retry`: a failed `esp_wifi_stop` keeps `radio_on=true`, prevents
    reconnects/scans, and retries only after one second; success publishes off.
11. `late-dns`: a delayed DNS result after the window closes cannot start SNTP.
12. `pre-cutoff-delayed-consume`: a reply received just before the cutoff remains
    a real success when the main task consumes it just afterwards. The next
    daily deadline uses receipt time, not processing time or failed-hour retry.
13. `portal-reopen-http-blocked`: a new portal request remains pending while the
    previous HTTP shutdown is blocked; it does not expire at a zero deadline
    and launches once the old server has actually stopped.
14. `restart-failure-retry`: a failed `esp_wifi_start` stays honestly off, retries
    after five seconds, and subsequently restores normal connectivity.
15. `reconnect-window-bound`: repeated disconnect/reconnect and GOT_IP cycles
    cannot reset or extend the 90-second radio window.
16. `rapid-ui-transitions`: immediate exit/reenter gestures before a main-loop
    tick cause no radio churn and cannot bypass the failed-hour cooldown.

17. `early-clock-entry`: a clock gesture before the network application first
    opens initializes both services, discovers saved credentials, and calibrates
    immediately rather than incorrectly waiting one hour.

## Mock boundary and limitations

The SDK shim controls the monotonic clock and delivers captured production ESP
callbacks. It records actual calls to driver start/stop/connect/scan/AP APIs and
SNTP init/deinit. Starting or connecting while an old driver STOP is still
pending is an assertion failure. NVS supplies a fixed saved-network fixture.
FreeRTOS queues and mutexes use host equivalents; the production HTTP teardown
worker runs on a host thread and can be deliberately blocked in `httpd_stop`.
Its singleton-lifetime allocations are excluded from leak checking, matching
the existing clock host runner.

Only hardware/network transports are mocked. Captive DNS socket transport and
HTTP transport are stubbed. HTTP handlers are never dispatched in these tests;
`stubs/cJSON.h` is a deliberately nonfunctional linker shim, not a replacement
JSON parser or security test. Production JSON parsing and captive-portal input
validation are covered by `tests/run_network_host.sh` with real cJSON.

These tests prove production control flow and driver-call ordering on a host.
They do not measure board current, verify ESP-IDF event ordering on hardware,
perform live Wi-Fi/DHCP/DNS/NTP exchanges, or validate phone/browser portal UX.
A firmware build and on-device power/lifecycle checks remain separate evidence.
