#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
tmp="$(mktemp -d /tmp/paperdiy-clock-power.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unused-variable -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie)
"${CXX:-g++}" "${flags[@]}" -I tests/clock_power/stubs -I main \
    main/clock/clock_power.cpp main/hal/hal_display.cpp tests/clock_power/power_test.cpp -o "$tmp/power"
for scenario in normal no-button-wake no-mutex create-failure initial-lock-failure gpio-a-failure gpio-b-failure \
    first-retention-failure last-retention-failure wake-enable-failure config-failure work-lock-failure release-failure reacquire-failure; do
    ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/power" "$scenario"
done
"${CXX:-g++}" "${flags[@]}" -DTEST_PM_DISABLED -I tests/clock_power/stubs -I main \
    main/clock/clock_power.cpp main/hal/hal_display.cpp tests/clock_power/power_test.cpp -o "$tmp/disabled"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/disabled" pm-disabled
