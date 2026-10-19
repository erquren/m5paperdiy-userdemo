/* SPDX-License-Identifier: MIT */
#pragma once
// HTTP JSON parsing is outside these driver-lifecycle tests. No HTTP handler
// is dispatched; the separately tested production parser uses real cJSON.
#include <cstddef>
struct cJSON { cJSON* child = nullptr; cJSON* next = nullptr; char* string = nullptr; char* valuestring = nullptr; };
inline cJSON* cJSON_ParseWithLengthOpts(const char*, std::size_t, const char**, bool) { return nullptr; }
inline bool cJSON_IsObject(const cJSON*) { return false; }
inline bool cJSON_IsString(const cJSON*) { return false; }
inline void cJSON_Delete(cJSON*) {}
inline cJSON* cJSON_CreateObject() { return nullptr; }
inline char* cJSON_PrintUnformatted(const cJSON*) { return nullptr; }
inline void cJSON_free(void*) {}
inline cJSON* cJSON_AddStringToObject(cJSON*, const char*, const char*) { return nullptr; }
inline cJSON* cJSON_AddNumberToObject(cJSON*, const char*, double) { return nullptr; }
inline cJSON* cJSON_AddBoolToObject(cJSON*, const char*, bool) { return nullptr; }
inline cJSON* cJSON_AddArrayToObject(cJSON*, const char*) { return nullptr; }
inline void cJSON_AddItemToArray(cJSON*, cJSON*) {}
