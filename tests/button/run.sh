#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
binary="$(mktemp /tmp/paperdiy-buttons.XXXXXX)"
trap 'rm -f "$binary"' EXIT
${CXX:-g++} -std=c++17 -Wall -Wextra -Werror -pedantic -g -O1 \
    -fsanitize=address,undefined -fno-omit-frame-pointer -I stubs \
    button_tests.cpp -o "$binary"
# LeakSanitizer cannot inspect processes under this executor's ptrace sandbox.
# AddressSanitizer and UndefinedBehaviorSanitizer remain enabled.
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$binary"
