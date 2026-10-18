/* SPDX-License-Identifier: MIT */
#pragma once
#include "wifi_validation.h"
#include <cJSON.h>
#include <cstring>

namespace wifi_request {
inline void secureErase(void* data, std::size_t size)
{
    auto* p = static_cast<volatile unsigned char*>(data);
    while (size--) *p++ = 0;
}
struct BodyBuffer {
    char bytes[513] = {};
    ~BodyBuffer() { secureErase(bytes, sizeof(bytes)); }
};
struct Request {
    char ssid[33] = {};
    char password[65] = {};
    char token[33] = {};
};
inline bool parse(const char* body, std::size_t length, bool connect, Request& result)
{
    result = {};
    if (!length || length > 512 || !wifi_validation::jsonWithoutNul(body, length)) return false;
    BodyBuffer buffer;
    char* terminated = buffer.bytes;
    std::memcpy(terminated, body, length);
    const char* end = nullptr;
    cJSON* root = cJSON_ParseWithLengthOpts(terminated, length + 1, &end, true);
    if (!root) return false;
    bool valid = cJSON_IsObject(root) && end == terminated + length;
    const char* ssid = nullptr;
    const char* password = nullptr;
    const char* token = nullptr;
    for (const cJSON* item = root->child; valid && item; item = item->next) {
        if (!item->string || !cJSON_IsString(item) || !item->valuestring) { valid = false; break; }
        if (std::strcmp(item->string, "token") == 0 && !token) token = item->valuestring;
        else if (connect && std::strcmp(item->string, "ssid") == 0 && !ssid) ssid = item->valuestring;
        else if (connect && std::strcmp(item->string, "password") == 0 && !password) password = item->valuestring;
        else valid = false;
    }
    valid = valid && token && std::strlen(token) == 32;
    if (valid && connect) valid = ssid && password && wifi_validation::credentials(ssid, std::strlen(ssid), password, std::strlen(password));
    if (valid) {
        std::memcpy(result.token, token, 32);
        if (connect) {
            std::memcpy(result.ssid, ssid, std::strlen(ssid));
            std::memcpy(result.password, password, std::strlen(password));
        }
    }
    // The pre-parser permits only a flat object; wipe decoded string copies
    // before releasing cJSON storage. Malformed-parse internal frees remain
    // cJSON's responsibility; no secrets are logged or returned in errors.
    if (root->valuestring) secureErase(root->valuestring, std::strlen(root->valuestring));
    for (cJSON* item = root->child; item; item = item->next)
        if (item->valuestring) secureErase(item->valuestring, std::strlen(item->valuestring));
    cJSON_Delete(root);
    return valid;
}
}  // namespace wifi_request
