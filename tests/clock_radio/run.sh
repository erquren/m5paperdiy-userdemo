#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
tmp="$(mktemp -d /tmp/paperdiy-clock-radio.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie -pthread)
"${CXX:-g++}" "${flags[@]}" -I tests/clock_radio/stubs -I main \
    main/network/wifi_manager.cpp main/clock/time_service.cpp main/clock/clock_radio.cpp \
    tests/clock_radio/sdk_mock.cpp tests/clock_radio/lifecycle_test.cpp -o "$tmp/lifecycle"
scenarios=(manager-suspend-resume daily-success combined-window got-ip-not-success portal-closes \
    long-a-stop-boundary no-credentials reentry-cooldown overflow-stop-boundary stop-failure-retry late-dns \
    pre-cutoff-delayed-consume portal-reopen-http-blocked restart-failure-retry reconnect-window-bound rapid-ui-transitions early-clock-entry)
for scenario in "${scenarios[@]}"; do
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/lifecycle" "$scenario"
done
echo "PASS: ${#scenarios[@]} production WifiManager + ClockRadio + TimeService scenarios (ASan/UBSan)"
