/* SPDX-License-Identifier: MIT */
#pragma once

#include <cstdint>
#include <ctime>

// begin()/tick()/setNetworkEnabled() belong to the main application task.
// begin() runs after WifiManager begins.
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
        // Monotonic boot time of the accepted response, never IP acquisition.
        // sync_generation distinguishes a valid response at boot time zero.
        std::int64_t last_successful_sync_us = 0;
        std::uint32_t sync_generation = 0;
        // Changes on sync/status changes, not on each ticking second.
        std::uint32_t revision = 0;
    };

    static TimeService& instance();
    bool begin();
    void tick();
    // Disabling synchronously fences callbacks/stops SNTP, but keeps time.
    // Reenable observes only a live associated STA with an up, addressed netif.
    // With observe_connected=false, wait for a subsequent valid GOT_IP instead.
    // deadline_us is an optional absolute monotonic cutoff for this window.
    // Repeated enabled calls update it without restarting the current attempt.
    // This gate never creates, starts, stops or connects the Wi-Fi driver.
    void setNetworkEnabled(bool enabled, bool observe_connected = true, std::int64_t deadline_us = 0);
    Snapshot snapshot() const;
    // Exact next minute boundary from the accepted fractional clock anchor.
    // Invalid clocks use a one-minute maintenance wake instead.
    std::int64_t nextMinuteDeadlineUs() const;

    TimeService(const TimeService&) = delete;
    TimeService& operator=(const TimeService&) = delete;

private:
    TimeService();
    struct Impl;
    Impl* _impl;
};
