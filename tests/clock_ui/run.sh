#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
tmp="$(mktemp -d /tmp/paperdiy-clock-ui.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
if [[ -n "${CLOCK_RENDER_DIR:-}" ]]; then mkdir -p "$CLOCK_RENDER_DIR"; fi
flags=(-std=c++17 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie)
"${CXX:-g++}" "${flags[@]}" main/clock/clock_fonts.cpp tests/clock_ui/render_test.cpp -o "$tmp/render"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/render" "${CLOCK_RENDER_DIR:-$tmp}"
"${CXX:-g++}" "${flags[@]}" -I tests/clock_ui/stubs -I main -I main/apps \
    main/clock/clock_fonts.cpp main/apps/app_clock/app_clock.cpp main/apps/app_wifi_scan/app_wifi_scan.cpp \
    tests/clock_ui/modal_test.cpp -o "$tmp/modal"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/modal"
