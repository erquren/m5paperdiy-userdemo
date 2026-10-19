/* SPDX-License-Identifier: MIT */
#pragma once

#include <algorithm>
#include <cstdint>
#include <ctime>

namespace clock_policy {
constexpr std::int64_t SECOND_US = 1000000;
constexpr std::int64_t DAY_US = 24LL * 60 * 60 * SECOND_US;
constexpr std::int64_t ATTEMPT_US = 30LL * SECOND_US;
constexpr std::int64_t COOLDOWN_US = 60LL * 60 * SECOND_US;
constexpr unsigned MAX_ATTEMPTS = 3;
constexpr std::int64_t CHINA_OFFSET_SECONDS = 8 * 60 * 60;

// Reject an uninitialized/obviously invalid clock. ESP-IDF's SNTP era handling
// supports dates through 2104. No build date or retained epoch establishes trust.
inline bool plausibleTime(std::int64_t seconds, std::int64_t microseconds)
{
    return seconds >= 1577836800LL && seconds < 4228588800LL &&
           microseconds >= 0 && microseconds < SECOND_US;
}

inline std::tm chinaTime(std::int64_t epoch_seconds)
{
    const std::time_t shifted = static_cast<std::time_t>(epoch_seconds + CHINA_OFFSET_SECONDS);
    std::tm result = {};
    gmtime_r(&shifted, &result);
    return result;
}

// This is the production scheduling policy. All inputs are monotonic boot
// microseconds, never wall time, so NTP forward/backward steps cannot alter the
// 24-hour cadence. Only a successful response resets that daily deadline.
struct SyncPolicy {
    bool network_available = false;
    bool syncing = false;
    unsigned attempts = 0;
    std::int64_t next_attempt_us = 0;
    std::int64_t attempt_deadline_us = 0;
    std::int64_t last_success_us = 0;

    void gotIp(std::int64_t now)
    {
        network_available = true;
        syncing = false;
        attempts = 0;
        next_attempt_us = now;
        attempt_deadline_us = 0;
    }
    void disconnected()
    {
        network_available = false;
        syncing = false;
        attempts = 0;
        attempt_deadline_us = 0;
    }
    bool due(std::int64_t now) const { return network_available && !syncing && now >= next_attempt_us; }
    void started(std::int64_t now)
    {
        syncing = true;
        ++attempts;
        attempt_deadline_us = now + ATTEMPT_US;
    }
    bool timedOut(std::int64_t now) const { return syncing && now >= attempt_deadline_us; }
    void failed(std::int64_t now)
    {
        syncing = false;
        attempt_deadline_us = 0;
        if (attempts >= MAX_ATTEMPTS) {
            attempts = 0;
            next_attempt_us = now + COOLDOWN_US;
        } else {
            next_attempt_us = now + (attempts <= 1 ? 30LL : 120LL) * SECOND_US;
        }
    }
    void succeeded(std::int64_t received_at)
    {
        syncing = false;
        attempts = 0;
        attempt_deadline_us = 0;
        last_success_us = received_at;
        next_attempt_us = received_at + DAY_US;
    }
};

// Use our accepted sample as the anchor instead of trusting the system's default
// epoch or an out-of-date SNTP callback. It ticks normally offline and accepts
// both forward and backward corrections at the next successful synchronization.
struct ClockAnchor {
    bool valid = false;
    std::int64_t epoch_us = 0;
    std::int64_t monotonic_us = 0;

    bool accept(std::int64_t seconds, std::int64_t microseconds, std::int64_t received_at)
    {
        if (!plausibleTime(seconds, microseconds)) return false;
        epoch_us = seconds * SECOND_US + microseconds;
        monotonic_us = received_at;
        valid = true;
        return true;
    }
    std::int64_t epochSeconds(std::int64_t now) const
    {
        if (!valid) return 0;
        return (epoch_us + std::max<std::int64_t>(0, now - monotonic_us)) / SECOND_US;
    }
};
}  // namespace clock_policy
