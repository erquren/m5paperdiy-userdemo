/* SPDX-License-Identifier: MIT */
#pragma once
#include "test_sdk.h"
#include <atomic>
#include <string>
#include <vector>

namespace sdk {
extern std::int64_t now_us;
extern bool driver_running;
extern bool stop_pending;
extern bool associated;
extern bool saved_credentials;
extern bool tcpip_context;
extern bool sntp_running;
extern bool defer_dns;
extern wifi_mode_t mode;
extern unsigned starts;
extern unsigned stops;
extern unsigned connects;
extern unsigned scans;
extern unsigned ap_enables;
extern unsigned dns_starts;
extern unsigned dns_stops;
extern unsigned sntp_starts;
extern unsigned sntp_stops;
extern unsigned lookups;
extern std::atomic<unsigned> http_starts;
extern std::atomic<unsigned> http_stops;
extern std::atomic<bool> block_http_stop;
extern unsigned fail_stops;
extern unsigned fail_starts;
extern std::vector<std::string> operations;
void emit(esp_event_base_t base, int id, void* data = nullptr);
void finishStop();
void gotIp();
void associate();
void disconnect();
void apStarted();
void scanDone();
void reply(std::int64_t epoch = 1792339199);
void lateReply();
void finishDns(bool success);
void setNow(std::int64_t value);
}
