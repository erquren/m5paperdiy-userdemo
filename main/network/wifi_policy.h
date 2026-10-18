/* SPDX-License-Identifier: MIT */
#pragma once
#include <algorithm>
#include <cstdint>
namespace wifi_policy {
inline std::int64_t deadline(std::int64_t session, std::int64_t success)
{
    return success ? std::min(session, success) : session;
}
inline bool expired(bool provisioning, std::int64_t now, std::int64_t session, std::int64_t success)
{
    return provisioning && now >= deadline(session, success);
}
inline std::uint32_t remaining(bool provisioning, std::int64_t now, std::int64_t session, std::int64_t success)
{
    const auto end = deadline(session, success);
    return provisioning && end > now ? static_cast<std::uint32_t>((end - now + 999) / 1000) : 0;
}
// Three attempts maximum in a burst. Further bursts back off 60/120/240/300s.
inline std::uint32_t retryDelay(unsigned attempts, unsigned exhausted_bursts, unsigned jitter)
{
    if (attempts < 3) return attempts <= 1 ? 1500 : 5000;
    const auto seconds = std::min(300u, 60u << std::min(exhausted_bursts, 3u));
    return seconds * 1000 + jitter % 1000;
}
inline bool currentAttempt(bool active, bool waiting_disconnect, std::uint32_t event_generation, std::uint32_t active_generation)
{
    return active && !waiting_disconnect && event_generation == active_generation;
}
inline bool mayAcceptIp(bool active, bool associated, bool waiting_disconnect,
                        std::uint32_t event_generation, std::uint32_t active_generation)
{
    return associated && currentAttempt(active, waiting_disconnect, event_generation, active_generation);
}
}  // namespace wifi_policy
