#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
tmp="$(mktemp -d /tmp/paperdiy-network.XXXXXX)"
trap 'rm -rf "$tmp"' EXIT
flags=(-std=c++17 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-omit-frame-pointer)
"${CXX:-g++}" "${flags[@]}" main/network/captive_dns.cpp main/network/tests/captive_dns_test.cpp -o "$tmp/dns"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/dns"
"${CXX:-g++}" "${flags[@]}" main/network/tests/wifi_validation_test.cpp -o "$tmp/validation"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/validation"
echo 'PASS: credential byte bounds, UTF-8, NUL rejection and token validation'
idf="${IDF_PATH:-../esp-idf}"
if [[ ! -f "$idf/components/json/cJSON/cJSON.c" ]]; then
    echo 'Set IDF_PATH to ESP-IDF v5.5.1 to run production JSON parser tests.' >&2
    exit 1
fi
"${CC:-gcc}" -std=c99 -g -O1 -fsanitize=address,undefined -I "$idf/components/json/cJSON" \
    -c "$idf/components/json/cJSON/cJSON.c" -o "$tmp/cjson.o"
"${CXX:-g++}" "${flags[@]}" -I "$idf/components/json/cJSON" \
    main/network/tests/wifi_request_test.cpp "$tmp/cjson.o" -o "$tmp/request"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/request"
"${CXX:-g++}" "${flags[@]}" main/network/tests/wifi_policy_test.cpp -o "$tmp/policy"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/policy"
"${CXX:-g++}" "${flags[@]}" main/network/tests/wifi_socket_test.cpp -o "$tmp/socket"
ASAN_OPTIONS="${ASAN_OPTIONS:-detect_leaks=0}" "$tmp/socket"
