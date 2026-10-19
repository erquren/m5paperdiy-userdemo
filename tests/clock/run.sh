#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
tmp="$(mktemp -d /tmp/paperdiy-clock.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie)
"${CXX:-g++}" "${flags[@]}" main/clock/tests/time_policy_test.cpp -o "$tmp/policy"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/policy"
"${CXX:-g++}" "${flags[@]}" -pthread -I tests/clock/stubs main/clock/time_service.cpp tests/clock/time_service_test.cpp -o "$tmp/service"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/service"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/service" startup
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/service" init-failure
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/service" dns-lifecycle
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/service" dns-failure
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/service" network-gate
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/service" network-gate-dns
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/service" network-window
echo 'PASS: 9 executable clock scenarios (repeated stress-loop iterations are not counted as extra scenarios)'
