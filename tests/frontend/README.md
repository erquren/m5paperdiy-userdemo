# Wi-Fi provisioning frontend checks

These tests exercise `main/assets/wifi_setup.html` against a simulated device API. They do not flash firmware or verify radio behavior on a physical device.

## Dependency-free DOM smoke tests

Requires Node.js with `TextEncoder` and `AbortController` (Node 18+ recommended):

```sh
node test_wifi_setup_dom.js
```

Result in this build workspace: **12 scenarios passed**. The tests cover safe SSID text rendering, scan serialization and token handling, UTF-8 byte validation, hidden/open networks, password clearing, stale status responses, reconnect/retry behavior, success completion, and session-token refresh. The DOM fixture is intentionally minimal and does not validate CSS or browser layout.

## Browser and mobile-layout tests

Requires Playwright and an installed Chromium browser:

```sh
CHROMIUM_PATH=/usr/bin/chromium node test_wifi_setup.js
```

Set `WIFI_SETUP_HTML=/absolute/path/to/wifi_setup.html` if needed. Both scripts also detect the HTML when located beside this workspace's `repo/`, or when copied into a repository's `tests/frontend/` directory.

The browser suite uses only intercepted local test URLs and a mocked API. It creates two mobile screenshots in `SCREENSHOT_DIR` (defaults to the test script directory):

- `wifi-setup-mobile-mock-preview.png` (390 px viewport)
- `wifi-setup-small-mock-preview.png` (320 px viewport)

These images, if generated, are simulated webpage previews, not screenshots from the physical device.

**Browser suite status in this build workspace: not executed.** The installed Chromium could not launch because the sandbox denied its local socket operation. A sandbox escalation attempt failed in the runtime wrapper before Chromium launched, with a read-only mount error. There was no reviewer denial and no permission workaround. No screenshots were produced. The suite is provided for execution in an environment where the browser can launch normally.
