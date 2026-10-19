# Production button-loop tests

Run from the repository root:

```sh
bash tests/button/run.sh
```

The harness compiles the actual `main/hal/hal_button.cpp`, with GPIO and FreeRTOS
stubs only. Each original gesture scenario runs with normal polling and again
with clock-only interrupt-driven idle waiting. During active gestures it advances
the production debounce/input task in two-millisecond steps; while the task is
blocked, simulated GPIO interrupts wake it. It polls the public action methods,
including single-B before triple-B to catch accidental TF-action leakage. The default build enables
AddressSanitizer and UndefinedBehaviorSanitizer, with warnings treated as errors.
LeakSanitizer alone is disabled for the executor's ptrace sandbox.

## B triple-click contract

- `Hal::wasButtonBTripleClicked()` consumes a one-shot event after the third
  debounced B-only release, with both buttons released.
- Every qualifying hold is at most 400 ms. Consecutive releases are at most
  400 ms apart, and the first-to-third release span is at most 800 ms. Boundaries
  are inclusive and use unsigned FreeRTOS tick differences for rollover safety.
- A single or double B click retains the pre-existing coalesced single-B action
  800 ms after the final debounced release. A valid triple replaces that pending
  action, so `wasButtonBClicked()` does not fire for the gesture.
- Fourth and later rapid clicks are absorbed. Recognition re-arms after 400 ms
  with neither button held. This guard survives application calls to
  `suppressButtonActionsUntilRelease()` so switching modes cannot leak a tail
  click. A fresh triple after that idle interval produces another event.
- A held B cannot repeat into a triple. A B already held when the task starts
  does not count toward a triple. An A press, an A+B chord, explicit suppression,
  or shutdown cancels partially accumulated B clicks.
- Recognized but unconsumed triples remain pending across further short actions;
  the existing single event slot does not queue arbitrary histories. A chord,
  long-A event, explicit suppression, or shutdown can supersede a pending triple.
- A short press, the three-second A-alone long press, and existing unarmed
  A+B last-release fallback are preserved. The app's armed-chord suppression and
  shutdown still clear pending actions and suppress release clicks.

## Coverage

The original 62 gesture cases retain all 26 original production-loop tests and add 36 tests for
triple recognition, single/double fallbacks, inclusive timing limits, slow
clicks, held/startup B, bounce, fourth/sixth clicks, quiet-boundary re-arming,
repeated gestures, mode suppression, A interruption, chords/shutdown, delayed
consumers, interaction state, rollover, and A behavior after a triple. All 62
run in both normal and clock-idle modes with their original assertions.

Eleven additional cases cover indefinite idle blocking, main-task wake and sticky
idle invalidation, first presses racing the final sample/wait or interrupt rearm,
post-triple rearming, normal-mode restoration, suppression/shutdown while blocked,
an existing ISR service, missing A/B handlers, and rearm failure fallback.

## Clock-only idle behavior

- `setButtonIdleMode(true)` enables waiting only when both raw and debounced pins
  are released and all pending/debounce/gesture/suppression work is settled.
- GPIO3/4 use low-level interrupts, matching ESP-IDF's GPIO sleep-wake trigger.
  The first press masks runtime interrupts and notifies the input and main tasks.
  Two-millisecond polling handles the complete gesture, including release.
- Once fully idle, the task rearms low-level interrupts, rechecks raw pins, and
  waits without clearing a notification that may have raced the recheck.
- Interrupt setup or rearm failure keeps polling and reports wake readiness as
  false, so the main power policy cannot enter its long idle wait.
- Disabling idle mode or requesting suppression/shutdown wakes blocked input.
  This layer does not power off any rail or enter sleep itself.

`results.txt` records the latest successful run. These are HAL logic tests, not
proof of physical button timing, application routing, clock rendering, Wi-Fi
behavior, or complete hardware shutdown. Those need integration/hardware tests.
