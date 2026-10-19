/* SPDX-License-Identifier: MIT */
#include "../time_policy.h"
#include <cassert>
#include <cstdio>

using namespace clock_policy;

std::int64_t epoch(int year, int month, int day, int hour, int minute, int second)
{
    std::tm value = {};
    value.tm_year = year - 1900;
    value.tm_mon = month - 1;
    value.tm_mday = day;
    value.tm_hour = hour;
    value.tm_min = minute;
    value.tm_sec = second;
    return timegm(&value);
}
void checkDate(std::int64_t utc, int year, int month, int day, int hour, int minute, int second)
{
    const auto value = chinaTime(utc);
    assert(value.tm_year + 1900 == year);
    assert(value.tm_mon + 1 == month);
    assert(value.tm_mday == day);
    assert(value.tm_hour == hour && value.tm_min == minute && value.tm_sec == second);
    assert(value.tm_isdst == 0);
}
int main()
{
    assert(!plausibleTime(0, 0));
    assert(!plausibleTime(1577836799, 0));
    assert(plausibleTime(1577836800, 0));
    assert(!plausibleTime(4228588800, 0));
    assert(!plausibleTime(1800000000, -1));
    assert(!plausibleTime(1800000000, 1000000));
    ClockAnchor clock;
    assert(!clock.valid && clock.epochSeconds(DAY_US) == 0);
    assert(!clock.accept(0, 0, 0) && !clock.valid);
    const auto before_midnight = epoch(2026, 10, 18, 15, 59, 59);
    assert(clock.accept(before_midnight, 900000, 100000));
    assert(clock.epochSeconds(100000) == before_midnight);
    assert(clock.epochSeconds(199999) == before_midnight);
    assert(clock.epochSeconds(200000) == before_midnight + 1);
    assert(clock.epochSeconds(99999) == before_midnight);  // snapshot race cannot rewind the anchor
    checkDate(clock.epochSeconds(100000), 2026, 10, 18, 23, 59, 59);
    checkDate(clock.epochSeconds(200000), 2026, 10, 19, 0, 0, 0);
    assert(clock.epochSeconds(DAY_US + 200000) == before_midnight + 86401);
    assert(!clock.accept(0, 0, DAY_US) && clock.valid);
    assert(clock.epochSeconds(DAY_US + 200000) == before_midnight + 86401);
    // Both correction directions replace the anchor without clamping wall time.
    assert(clock.accept(before_midnight - 1000, 0, DAY_US));
    assert(clock.epochSeconds(DAY_US) == before_midnight - 1000);
    assert(clock.accept(before_midnight + 1000, 0, DAY_US + SECOND_US));
    assert(clock.epochSeconds(DAY_US + SECOND_US) == before_midnight + 1000);
    checkDate(epoch(2026, 12, 31, 16, 0, 0), 2027, 1, 1, 0, 0, 0);
    checkDate(epoch(2028, 2, 28, 16, 0, 0), 2028, 2, 29, 0, 0, 0);
    checkDate(epoch(2028, 2, 29, 16, 0, 0), 2028, 3, 1, 0, 0, 0);
    checkDate(epoch(2027, 2, 28, 16, 0, 0), 2027, 3, 1, 0, 0, 0);
    assert(chinaTime(epoch(2026, 10, 18, 12, 0, 0)).tm_wday == 0);

    SyncPolicy policy;
    assert(!policy.due(0));
    std::int64_t now = 50 * DAY_US;  // past a 32-bit millisecond counter wrap
    policy.gotIp(now);
    assert(policy.due(now) && policy.attempts == 0);
    policy.started(now);
    assert(!policy.due(now));
    assert(!policy.timedOut(now + ATTEMPT_US - 1));
    assert(policy.timedOut(now + ATTEMPT_US));
    now += ATTEMPT_US;
    policy.failed(now);
    assert(policy.next_attempt_us == now + 30 * SECOND_US);
    assert(!policy.due(policy.next_attempt_us - 1));
    now = policy.next_attempt_us;
    assert(policy.due(now));
    policy.started(now);
    now += ATTEMPT_US;
    policy.failed(now);
    assert(policy.next_attempt_us == now + 120 * SECOND_US);
    now = policy.next_attempt_us;
    policy.started(now);
    now += ATTEMPT_US;
    policy.failed(now);
    assert(policy.next_attempt_us == now + COOLDOWN_US);
    assert(policy.attempts == 0 && !policy.due(policy.next_attempt_us - 1));
    // A fresh GOT_IP immediately overrides even an exhausted retry burst.
    policy.gotIp(++now);
    assert(policy.due(now));
    policy.started(now);
    policy.succeeded(++now);
    assert(policy.last_success_us == now);
    assert(policy.next_attempt_us == now + DAY_US);
    for (int i = 0; i < 1000; ++i) assert(!policy.due(now + DAY_US - 1));
    assert(policy.next_attempt_us == now + DAY_US);  // reads never slide the cadence
    assert(policy.due(now + DAY_US));
    policy.started(now + DAY_US);
    policy.failed(now + DAY_US + ATTEMPT_US);
    assert(policy.last_success_us == now);  // failure doesn't claim a new successful sync
    policy.disconnected();
    assert(!policy.network_available && !policy.syncing);
    assert(!policy.due(now + 100 * DAY_US));
    policy.gotIp(now + 100 * DAY_US);
    assert(policy.due(now + 100 * DAY_US));
    puts("PASS: production time policy, bounded retries, 24-hour cadence, offline ticking, corrections and China rollover");
}
