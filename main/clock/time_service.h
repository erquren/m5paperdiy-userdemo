/* SPDX-License-Identifier: MIT */
#pragma once

#include <cstdint>
#include <ctime>

// begin()/tick() belong to the main application task, after WifiManager begins.
// snapshot() is safe from other tasks. This service observes station events;
// it never starts, stops, connects or otherwise owns the Wi-Fi driver.
class TimeService {
public:
    struct Snapshot {
        bool valid = false;
        bool syncing = false;
        // A station IP permits an SNTP attempt; it does not prove Internet access.
        bool network_available = false;
        std::int64_t epoch_seconds = 0;  // UTC, zero until an accepted SNTP reply.
        std::tm local_time = {};        // China Standard Time, UTC+8, no DST.
        // Changes on sync/status changes, not on each ticking second.
        std::uint32_t revision = 0;
    };

    static TimeService& instance();
    bool begin();
    void tick();
    Snapshot snapshot() const;

    TimeService(const TimeService&) = delete;
    TimeService& operator=(const TimeService&) = delete;

private:
    TimeService();
    struct Impl;
    Impl* _impl;
};
