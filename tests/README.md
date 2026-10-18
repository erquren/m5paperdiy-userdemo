# Host checks

These checks do not require a connected PaperDIY. They do not replace radio,
DHCP, device heap, e-paper timing, or iOS/Android captive-window hardware tests.

From the repository root:

```sh
bash tests/button/run.sh
# Requires ESP-IDF v5.5.1 sources for its actual cJSON implementation:
export IDF_PATH=/path/to/esp-idf
bash tests/run_network_host.sh
node tests/frontend/test_wifi_setup_dom.js
python3 tests/check_integration.py
```

C++ checks require GCC/G++ with AddressSanitizer and UndefinedBehaviorSanitizer.
The script disables LeakSanitizer by default because the cloud executor does not
support its process inspection. AddressSanitizer and UBSan remain enabled.

- Button tests execute the production HAL loop with mocked GPIO/FreeRTOS.
- DNS tests exercise the production packet parser, including 100,000 random malformed inputs.
- Credential and JSON tests exercise the production validation helpers and SDK cJSON.
- Policy tests cover retry delays, deadlines and stale-event eligibility, including
  1,000 repeated non-mutating deadline reads. They do not simulate the full ESP-IDF driver.
- Frontend tests use a minimal Node DOM and simulated API. They do not validate layout.
- Integration checks are static assertions for app order, modal isolation and assets.

See `frontend/README.md` for the separate browser suite. That suite could not run
in the cloud environment because Chromium could not start; no browser-layout
or device-run success is claimed.
