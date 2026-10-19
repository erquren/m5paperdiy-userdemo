# Clock host checks

Run from the repository root:

```sh
bash tests/clock/run.sh
```

The suite uses GCC/G++ with AddressSanitizer and UndefinedBehaviorSanitizer.
`-no-pie` avoids the cloud runner's intermittent ASan shadow-address collision;
LeakSanitizer is disabled by default because its process inspection is unsupported.

- `main/clock/tests/time_policy_test.cpp` exercises the production scheduling and
  accepted-time anchor helpers, including exact deadlines, finite three-attempt
  bursts, hourly retry cooldown, long uptime, both correction directions, and
  China midnight, month, leap-day, year and weekday boundaries.
- `time_service_test.cpp` compiles the actual `main/clock/time_service.cpp` against
  small ESP-IDF/FreeRTOS stubs. It checks immediate GOT_IP handling, association
  without DHCP, stale interface IP after disconnect, callback/session fences,
  callback at the deadline, invalid responses, failure and startup cleanup,
  server rotation, daily cadence measured from successful response, offline
  ticking, reconnects, and concurrent snapshot reads during 1,000 sync cycles.
- The `startup` scenario checks attaching the observer after an IP was acquired.
- The `init-failure` scenario checks partial observer-registration cleanup and
  SNTP initialization errors without deinitializing an instance we do not own.
- The `dns-lifecycle` scenario defers resolver callbacks across disconnects,
  reconnects and deadlines, then exhausts all three stable callback contexts.
  Late successes/failures cannot start SNTP or affect a newer attempt; a DNS
  lookup and its subsequent SNTP request share the same 30-second deadline.
- The `dns-failure` scenario checks immediate resolver errors and bounded
  retry/cooldown behavior without leaking a callback context.
- The `network-gate` scenario checks synchronous disable before startup and
  during SNTP, callbacks during teardown, late GOT_IP, retained offline time,
  success-only monotonic metadata, idempotence, and live association/netif/IP
  checks when reenabled. A disconnect during interface observation wins.
- The `network-gate-dns` scenario checks deferred DNS and queued results across
  disabled/reopened windows, pool exhaustion, and reuse after terminal callbacks.
- The `network-window` scenario checks exact absolute window deadlines before
  the next tick, late DNS and delayed TCP/IP dispatch, shortening/clearing an
  active deadline without restarting attempts, and preserving timely replies.

The runner reports nine executable scenarios: policy, normal service, startup,
initialization failure, deferred DNS lifecycle, immediate DNS failure, synchronous
network gating, deferred DNS across network-gate cycles, and absolute windows.
The 1,000 repeated concurrency iterations do not inflate that scenario count.

The tests mock packet transport and event scheduling. They do not prove live DNS,
NTP server reachability, UDP behavior, Wi-Fi reliability, device oscillator drift,
or visual e-paper behavior. The concurrency test is a lock-use smoke test under
ASan/UBSan, not a ThreadSanitizer run. Device acceptance must also cover a router
with no Internet/NTP access, disconnect/reconnect, overnight rollover, and modal
setup/shutdown behavior.

The service uses ESP-IDF v5.5.1's `esp_netif_sntp_init()` and
`esp_netif_sntp_deinit()` to marshal SNTP operations to the TCP/IP thread. It stops
SNTP after success/timeout so the SDK's automatic hourly update cannot override
the application's monotonic 24-hour policy. Initialization and deinitialization
both run on the TCP/IP thread to serialize the SDK's callback storage lifetime.
Three servers are tried in rotation:
`ntp.aliyun.com`, `ntp.tencent.com`, `pool.ntp.org`. One is configured at a time,
so the default single-server SDK setting is sufficient.

The service resolves IPv4 names separately using lwIP's asynchronous DNS API on
the TCP/IP thread. A fixed three-slot pool retains each context until its terminal
callback; cancellation invalidates the generation instead of reusing live memory.
Only a numeric address is given to native SNTP. This avoids ESP-IDF v5.5.1/lwIP's
uncancelled SNTP DNS callback after `sntp_stop()`. Exhausting the pool follows the
normal retry cooldown, so pending lookups cannot cause unbounded allocation.

Clock validity requires an accepted, plausible response in the current attempt.
SNTP is unauthenticated; this is a usability clock, not a security time source.
No date is fabricated from build time, IP acquisition, or saved NVS. After a full
power cycle or deep-sleep reset it remains invalid until another successful
synchronization. A clock that has synchronized keeps ticking while powered and
offline; this service does not preserve its monotonic anchor across a reset.

`setNetworkEnabled(false)` is a synchronous, main-task-only fence for deliberate
radio-off windows. It stops owned SNTP and suppresses new DNS/SNTP work and late
GOT_IP until reenabled, while preserving accepted time and success metadata.
An accepted response awaiting the next tick is retained when the gate closes.
Reenabling may observe the already-connected station; pass `false` as the second
argument to wait for a later valid GOT_IP instead. Interface observation checks
actual STA association, netif-up state, and a nonzero current IP. The service
only reads driver status and never starts, stops, or creates Wi-Fi.
An optional third argument sets the window's absolute monotonic deadline. It
clamps callback eligibility and TCP/IP work even if the main task runs late.
An expired gate closes on the next tick; a new window needs an explicit enable.
