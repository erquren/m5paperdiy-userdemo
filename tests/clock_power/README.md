# Clock automatic-light-sleep tests

Run `bash tests/clock_power/run.sh` from the repository root. The ASan/UBSan suite
compiles the production `clock_power.cpp` and `hal_display.cpp` against bounded
ESP-IDF/HAL/platform mocks. It does not substitute a second copy of the policy.

Covered:

- Clock-only quiet eligibility; normal mode, shutdown dialog/in-progress,
  refresh requests, active gestures, unavailable interrupts, USB Serial/JTAG,
  attached JTAG debugger, unsettled radio lifecycle, and background work gates.
- Fractional wall-clock-minute deadlines and earlier radio deadlines.
- A minute passing during a main frame/EPD refresh, and an SNTP sample accepted
  after the clock has rendered, both forcing another frame before long idle.
- A pending/racing button notification is consumed by the wait, never discarded
  before it; main CPU/awake locks are restored when the wait ends.
- Complete production partial/full EPD submit/wait/return sequence, including the
  maintenance full refresh after 40 partial refreshes, with the main locks
  deliberately absent so work locks must independently protect the waveform.
- Nested work guards and a TF-equivalent background guard vetoing idle sleep.
- Hidden clock feedback waits without a polling timeout and wakes on exit.
- Exact board GPIO retention set and failures on the first/last retention call.
- Missing button interrupts/mutex, PM-lock creation/acquire/release, either GPIO
  wake setup, system wake enable, PM configure and post-wake reacquire failure.
- PM-disabled builds retain the original awake polling fallback.

Implementation uses ESP-IDF automatic light sleep with FreeRTOS tickless idle.
The application waits for the next minute, button notification or radio deadline;
SDK timers, peripheral work, and the RTOS may wake the CPU earlier. It makes no
claim that the physical device wakes only once per minute. USB-connected debug
sessions intentionally remain awake.

Pinned-source timing check: M5GFX `Panel_EPD::task_update()` calls
`bus->powerControl(false)` after the final scanline transaction, then clears
`_display_busy` at its next loop top. `waitDisplay()` waits for that flag. Both
HAL flush paths now wait synchronously while holding the display mutex and
CPU_MAX/NO_LIGHT_SLEEP locks, so a prior waveform cannot race a new submission.
The board's separate EPD supply is left unchanged.

ESP32-S3's SDK GPIO-reset workaround selects `CONFIG_PM_SLP_DISABLE_GPIO=y`.
It remains enabled. The wake API exempts A/B from isolation; the application
explicitly preserves EPD output pins 6–18/45/46, TF output pins 38/39/47, and
externally pulled-up PMIC I2C pins 41/42. This keeps boost/OE low and chip-select
and bus idle levels driven while other pins retain the SDK's isolation. Flash
and PSRAM leakage workarounds remain unchanged. No deep sleep, RTC reset,
PSRAM shutdown, eFuse, security, or USB-disable changes are made.

Still required on hardware: battery-powered current measurements, USB reconnect
behavior, actual GPIO wake latency, B triple reliability after a long sleep,
long-A and A+B behavior, screen quality/waveform completion and overnight time
continuity. Host tests and a firmware cross-build do not prove these properties.
