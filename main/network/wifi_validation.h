/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wifi_validation {
inline bool utf8(const char* data, std::size_t size)
{
    for (std::size_t i = 0; i < size;) {
        const auto c = static_cast<unsigned char>(data[i++]);
        if (c == 0) return false;
        if (c < 0x80) continue;
        unsigned count = 0;
        std::uint32_t value = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { count = 1; value = c & 0x1f; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { count = 2; value = c & 0x0f; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { count = 3; value = c & 7; minimum = 0x10000; }
        else return false;
        if (i + count > size) return false;
        while (count--) {
            const auto next = static_cast<unsigned char>(data[i++]);
            if ((next & 0xc0) != 0x80) return false;
            value = (value << 6) | (next & 0x3f);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}
inline bool credentials(const char* ssid, std::size_t ssid_size, const char* password, std::size_t password_size)
{
    if (!ssid || !password || ssid_size == 0 || ssid_size > 32 || !utf8(ssid, ssid_size)) return false;
    if (password_size == 0) return true;
    if (password_size == 64) {
        for (std::size_t i = 0; i < password_size; ++i) {
            const char c = password[i];
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
        }
        return true;
    }
    return password_size >= 8 && password_size <= 63 && utf8(password, password_size);
}
// cJSON uses zero-terminated values. Refuse raw/escaped NUL before parsing,
// while allowing the literal text "\\u0000" represented by a JSON escaped '\\'.
inline bool jsonWithoutNul(const char* data, std::size_t size)
{
    if (!data || std::memchr(data, 0, size)) return false;
    bool in_string = false;
    unsigned object_depth = 0;
    for (std::size_t i = 0; i < size; ++i) {
        if (data[i] == '"') in_string = !in_string;
        else if (in_string && data[i] == '\\') {
            if (++i >= size) return false;
            if (data[i] == 'u' && i + 4 < size && std::memcmp(data + i + 1, "0000", 4) == 0) return false;
        } else if (in_string && static_cast<unsigned char>(data[i]) < 0x20) return false;
        else if (!in_string) {
            // Requests are flat objects of strings. Bound parser recursion
            // before cJSON sees untrusted input on the small HTTP task stack.
            if (data[i] == '{' && ++object_depth > 1) return false;
            if (data[i] == '}' && (object_depth == 0 || --object_depth != 0)) return false;
            if (data[i] == '[' || data[i] == ']') return false;
        }
    }
    return !in_string && object_depth == 0 && utf8(data, size);
}
inline bool equalToken(const char* a, const char* b)
{
    if (!a || !b || std::strlen(a) != 32 || std::strlen(b) != 32) return false;
    unsigned difference = 0;
    for (unsigned i = 0; i < 32; ++i) difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    return difference == 0;
}
}  // namespace wifi_validation
