/* SPDX-License-Identifier: MIT */
#include "../../main/clock/time_service.h"
#include "../../main/clock/time_policy.h"
#include "test_sdk.h"
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
std::atomic<std::int64_t> now{0};
esp_netif_t station;
bool station_up = false;
bool station_associated = false;
std::uint32_t station_ip = 0;
bool disconnect_during_ip_read = false;
struct Handler { esp_event_base_t base; esp_event_handler_t callback; void* context; bool active; };
std::vector<Handler> handlers;
unsigned registration_calls = 0;
unsigned registration_fail_at = 0;
bool disconnect_during_registration = false;
unsigned starts = 0;
unsigned stops = 0;
unsigned lookups = 0;
bool tcpip_context = false;
unsigned tcpip_calls_before_advance = 0;
std::int64_t advance_tcpip_to_us = 0;
enum class DnsMode { Immediate, Deferred, Failure };
DnsMode dns_mode = DnsMode::Immediate;
struct PendingDns { std::string hostname; dns_found_callback callback; void* context; };
std::vector<PendingDns> pending_dns;
std::vector<std::string> hostnames;
esp_err_t next_init_result = ESP_OK;
bool initialized = false;
esp_sntp_time_cb_t callback = nullptr;
esp_sntp_time_cb_t last_callback = nullptr;
std::vector<std::string> servers;
const char* retained_server_address = nullptr;
bool reply_during_stop = false;
constexpr std::int64_t BASE_EPOCH = 1792339199;  // 2026-10-18 15:59:59 UTC
void emit(esp_event_base_t base, int id, void* data = nullptr)
{
    for (const auto& handler : handlers)
        if (handler.active && handler.base == base) handler.callback(handler.context, base, id, data);
}
void notifyGotIp()
{
    ip_event_got_ip_t ip{&station, {{station_ip}, {}, {}}, true};
    emit(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
}
void gotIp()
{
    station_up = true;
    station_associated = true;
    station_ip = 0x01020304;
    notifyGotIp();
}
void disconnect()
{
    station_up = false;
    station_associated = false;
    // Deliberately retain stale IP, as a disconnected interface can do.
    emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED);
}
void reply(std::int64_t epoch = BASE_EPOCH, std::int64_t microseconds = 0)
{
    assert(callback);
    assert(retained_server_address && std::string(retained_server_address) == servers.back());
    timeval value{static_cast<time_t>(epoch), static_cast<suseconds_t>(microseconds)};
    callback(&value);
}
void lateReply()
{
    assert(last_callback);
    timeval value{static_cast<time_t>(BASE_EPOCH + 100000), 0};
    last_callback(&value);
}
void completeDns(std::size_t index, bool success)
{
    const auto pending = pending_dns.at(index);
    pending_dns.erase(pending_dns.begin() + index);
    ip_addr_t address{0xcb007101};
    const bool before = tcpip_context;
    tcpip_context = true;
    pending.callback(pending.hostname.c_str(), success ? &address : nullptr, pending.context);
    tcpip_context = before;
}
}
const char* esp_err_to_name(esp_err_t) { return "mock error"; }
std::int64_t esp_timer_get_time() { return now.load(); }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, std::int32_t id, esp_event_handler_t cb, void* context,
                                             esp_event_handler_instance_t* instance)
{
    assert(id == ESP_EVENT_ANY_ID);
    ++registration_calls;
    if (registration_calls == registration_fail_at) return ESP_FAIL;
    handlers.push_back({base, cb, context, true});
    *instance = reinterpret_cast<void*>(handlers.size());
    if (disconnect_during_registration && base == WIFI_EVENT) {
        disconnect_during_registration = false;
        cb(context, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, nullptr);
    }
    return ESP_OK;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, std::int32_t id, esp_event_handler_instance_t instance)
{
    assert(id == ESP_EVENT_ANY_ID);
    const auto index = reinterpret_cast<std::size_t>(instance) - 1;
    assert(handlers.at(index).base == base);
    handlers.at(index).active = false;
    return ESP_OK;
}
esp_netif_t* esp_netif_get_handle_from_ifkey(const char* key)
{
    assert(std::strcmp(key, "WIFI_STA_DEF") == 0);
    return &station;
}
bool esp_netif_is_netif_up(esp_netif_t* netif) { assert(netif == &station); return station_up; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t*) { return station_associated ? ESP_OK : ESP_FAIL; }
esp_err_t esp_netif_get_ip_info(esp_netif_t* netif, esp_netif_ip_info_t* value)
{
    assert(netif == &station);
    value->ip.addr = station_ip;
    if (disconnect_during_ip_read) {
        disconnect_during_ip_read = false;
        disconnect();
    }
    return ESP_OK;
}
esp_err_t esp_netif_tcpip_exec(esp_netif_callback_fn function, void* context)
{
    if (tcpip_calls_before_advance && --tcpip_calls_before_advance == 0) now = advance_tcpip_to_us;
    const bool before = tcpip_context;
    tcpip_context = true;
    const auto result = function(context);
    tcpip_context = before;
    return result;
}
char* ipaddr_ntoa_r(const ip_addr_t* address, char* output, int size)
{
    assert(tcpip_context && address->addr == 0xcb007101);
    std::snprintf(output, size, "203.0.113.1");
    return output;
}
err_t dns_gethostbyname_addrtype(const char* hostname, ip_addr_t* address, dns_found_callback cb,
                               void* context, unsigned char type)
{
    assert(tcpip_context && type == LWIP_DNS_ADDRTYPE_IPV4);
    ++lookups;
    hostnames.emplace_back(hostname);
    if (dns_mode == DnsMode::Deferred) {
        // Contexts of in-flight DNS requests must never be reused.
        for (const auto& pending : pending_dns) assert(pending.context != context);
        pending_dns.push_back({hostname, cb, context});
        assert(pending_dns.size() <= 3);
        return ERR_INPROGRESS;
    }
    if (dns_mode == DnsMode::Failure) return ERR_ARG;
    address->addr = 0xcb007101;
    return ERR_OK;
}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t* config)
{
    assert(!initialized && tcpip_context);
    ++starts;
    assert(!config->smooth_sync && !config->server_from_dhcp && !config->wait_for_sync && config->start);
    assert(config->num_of_servers == 1 && config->sync_cb);
    // A literal address cannot create native SNTP's uncancelled async DNS.
    assert(std::strcmp(config->servers[0], "203.0.113.1") == 0);
    servers.emplace_back(config->servers[0]);
    if (next_init_result != ESP_OK) {
        const auto result = next_init_result;
        next_init_result = ESP_OK;
        return result;
    }
    initialized = true;
    retained_server_address = config->servers[0];
    callback = last_callback = config->sync_cb;
    return ESP_OK;
}
void esp_netif_sntp_deinit()
{
    assert(initialized && tcpip_context);
    ++stops;
    if (reply_during_stop) lateReply();  // stop must fence callbacks before the TCP/IP barrier
    initialized = false;
    callback = nullptr;
    retained_server_address = nullptr;
}

int main(int argc, char** argv)
{
    using namespace clock_policy;
    auto& service = TimeService::instance();
    assert(!service.snapshot().valid && service.snapshot().epoch_seconds == 0);
    assert(service.nextMinuteDeadlineUs() == now.load() + 60 * SECOND_US);
    service.tick();
    if (argc > 1 && std::strcmp(argv[1], "startup") == 0) {
        station_up = true;
        station_associated = true;
        station_ip = 0x01020304;
        assert(service.begin());
        assert(starts == 1 && service.snapshot().syncing && !service.snapshot().valid);
        reply();
        service.tick();
        assert(service.snapshot().valid);
        puts("PASS: already-connected startup observes station without creating another Wi-Fi owner");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "init-failure") == 0) {
        registration_fail_at = 2;
        disconnect_during_registration = true;
        assert(!service.begin());
        assert(handlers.size() == 1 && !handlers[0].active);
        station_up = true;  // IP acquired while the failed observer was absent
        station_associated = true;
        station_ip = 0x01020304;
        next_init_result = ESP_ERR_INVALID_STATE;
        assert(service.begin());
        assert(starts == 1 && stops == 0 && !service.snapshot().valid && !service.snapshot().syncing);
        now = 30 * SECOND_US;
        service.tick();
        assert(starts == 2 && service.snapshot().syncing);
        reply();
        service.tick();
        assert(service.snapshot().valid && stops == 1);
        puts("PASS: event-registration cleanup, SNTP init failure/backoff, no deinit of unowned SNTP");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "network-gate") == 0) {
        // A pre-begin gate must prevent startup observation and late GOT_IP.
        station_up = station_associated = true;
        station_ip = 0x01020304;
        service.setNetworkEnabled(false);
        assert(service.begin());
        notifyGotIp();
        service.tick();
        assert(starts == 0 && lookups == 0 && !service.snapshot().network_available);
        assert(service.snapshot().sync_generation == 0 && service.snapshot().last_successful_sync_us == 0);
        service.setNetworkEnabled(false);
        now = 10 * SECOND_US;
        service.setNetworkEnabled(true, false);
        service.tick();
        assert(starts == 0);  // suppress observation until a valid new event
        notifyGotIp();
        service.tick();
        assert(starts == 1 && service.snapshot().syncing);
        assert(service.snapshot().sync_generation == 0);  // GOT_IP is not success
        now += 2 * SECOND_US;
        reply(BASE_EPOCH, 123000);
        reply_during_stop = true;
        service.setNetworkEnabled(false);  // retain a reply accepted before the fence
        auto accepted = service.snapshot();
        assert(stops == 1 && !initialized && accepted.valid && !accepted.syncing && !accepted.network_available);
        assert(accepted.epoch_seconds == BASE_EPOCH && accepted.sync_generation == 1);
        assert(accepted.last_successful_sync_us == now.load());
        now += SECOND_US;
        lateReply();  // no tick is needed to fence callbacks after disabling
        notifyGotIp();
        service.tick();
        assert(starts == 1 && lookups == 1 && !service.snapshot().network_available);
        assert(service.snapshot().epoch_seconds == BASE_EPOCH + 1);
        assert(service.snapshot().sync_generation == accepted.sync_generation);
        assert(service.snapshot().last_successful_sync_us == accepted.last_successful_sync_us);

        service.setNetworkEnabled(true, false);
        assert(starts == 1);
        notifyGotIp();
        service.tick();
        assert(starts == 2);
        service.setNetworkEnabled(false);  // active SNTP is stopped synchronously
        assert(stops == 2 && !initialized && !service.snapshot().syncing);
        service.setNetworkEnabled(false);
        assert(stops == 2 && service.snapshot().sync_generation == 1);
        lateReply();
        service.tick();
        assert(service.snapshot().sync_generation == 1);

        // A stopped driver can leave both cached IP and netif-up state behind.
        station_associated = false;
        assert(station_up && station_ip);
        service.setNetworkEnabled(true);
        notifyGotIp();
        service.tick();
        assert(starts == 2 && !service.snapshot().network_available);
        service.setNetworkEnabled(false);
        station_associated = true;
        station_up = false;
        service.setNetworkEnabled(true);
        notifyGotIp();
        service.tick();
        assert(starts == 2);  // association without an up interface is insufficient
        service.setNetworkEnabled(false);
        station_up = true;
        station_ip = 0;
        service.setNetworkEnabled(true);
        notifyGotIp();
        service.tick();
        assert(starts == 2);  // association without DHCP is insufficient
        station_ip = 0x01020304;
        esp_netif_t other_station;
        ip_event_got_ip_t wrong_interface{&other_station, {{station_ip}, {}, {}}, true};
        emit(IP_EVENT, IP_EVENT_STA_GOT_IP, &wrong_interface);
        ip_event_got_ip_t old_address{&station, {{0x05060708}, {}, {}}, true};
        emit(IP_EVENT, IP_EVENT_STA_GOT_IP, &old_address);
        service.tick();
        assert(starts == 2);

        service.setNetworkEnabled(false);
        disconnect_during_ip_read = true;
        service.setNetworkEnabled(true);
        assert(starts == 2 && !service.snapshot().network_available);
        service.setNetworkEnabled(false);
        station_up = station_associated = true;
        now += 5 * SECOND_US;
        service.setNetworkEnabled(true);  // valid already-connected observation
        assert(starts == 3 && service.snapshot().syncing);
        assert(service.snapshot().sync_generation == 1);
        reply(BASE_EPOCH - 3600);  // success metadata follows monotonic receive time
        service.tick();
        assert(service.snapshot().sync_generation == 2);
        assert(service.snapshot().last_successful_sync_us == now.load());
        assert(service.snapshot().epoch_seconds == BASE_EPOCH - 3600);
        service.setNetworkEnabled(true);
        service.tick();
        assert(starts == 3);  // enabling twice does not bypass the daily cadence
        service.setNetworkEnabled(false);
        now += 3 * DAY_US;
        for (int i = 0; i < 1000; ++i) { notifyGotIp(); service.tick(); }
        assert(starts == 3 && service.snapshot().sync_generation == 2);
        assert(service.snapshot().valid && !service.snapshot().network_available);
        puts("PASS: synchronous network gate, pre-begin disable, late callbacks/GOT_IP, live-interface observation and success metadata");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "network-gate-dns") == 0) {
        assert(service.begin());
        gotIp();
        service.tick();
        reply();
        service.tick();
        assert(service.snapshot().valid && service.snapshot().sync_generation == 1);
        assert(service.snapshot().last_successful_sync_us == 0);  // zero is a valid monotonic instant
        service.setNetworkEnabled(false);
        dns_mode = DnsMode::Deferred;
        now += SECOND_US;
        service.setNetworkEnabled(true);
        assert(pending_dns.size() == 1 && starts == 1);
        service.setNetworkEnabled(false);
        completeDns(0, true);  // late resolver response before any subsequent tick
        notifyGotIp();
        service.tick();
        assert(starts == 1 && !service.snapshot().syncing && !service.snapshot().network_available);

        service.setNetworkEnabled(true);
        assert(pending_dns.size() == 1);
        service.setNetworkEnabled(false);
        service.setNetworkEnabled(true);
        assert(pending_dns.size() == 2);
        completeDns(0, true);  // old generation cannot start SNTP in the new window
        service.tick();
        assert(starts == 1 && service.snapshot().syncing);
        completeDns(0, true);
        service.tick();
        assert(starts == 2);
        reply(BASE_EPOCH + 60);
        service.tick();
        assert(service.snapshot().sync_generation == 2);
        assert(service.snapshot().last_successful_sync_us == now.load());

        service.setNetworkEnabled(false);
        service.setNetworkEnabled(true);
        completeDns(0, true);  // resolved address waiting in inbox is fenced too
        service.setNetworkEnabled(false);
        service.setNetworkEnabled(true, false);
        service.tick();
        assert(starts == 2 && !service.snapshot().syncing && !service.snapshot().network_available);

        // Repeated toggles do not reuse contexts still owned by lwIP.
        for (int i = 0; i < 3; ++i) {
            service.setNetworkEnabled(false);
            service.setNetworkEnabled(true);
        }
        assert(pending_dns.size() == 3);
        const auto before_exhaustion = lookups;
        service.setNetworkEnabled(false);
        service.setNetworkEnabled(true);
        assert(lookups == before_exhaustion && !service.snapshot().syncing);
        service.setNetworkEnabled(false);
        completeDns(0, true);
        completeDns(0, false);
        completeDns(0, true);
        service.tick();
        assert(starts == 2 && service.snapshot().sync_generation == 2);
        service.setNetworkEnabled(true);
        completeDns(0, true);
        service.tick();
        reply(BASE_EPOCH + 120);
        service.tick();
        assert(starts == 3 && service.snapshot().sync_generation == 3);
        assert(service.snapshot().epoch_seconds == BASE_EPOCH + 120);
        puts("PASS: network-gate DNS fencing, pending inbox cancellation, repeated-window generations and bounded context reuse");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "network-window") == 0) {
        service.setNetworkEnabled(false);
        assert(service.begin());
        now = SECOND_US;
        const auto first_deadline = now.load() + 90 * SECOND_US;
        service.setNetworkEnabled(true, true, first_deadline);
        now = first_deadline - 5 * SECOND_US;  // late association leaves only 5s
        gotIp();
        service.tick();
        assert(starts == 1 && service.snapshot().syncing);
        now = first_deadline;
        reply();  // exact whole-window deadline, before the main loop runs
        service.tick();
        assert(!service.snapshot().valid && !service.snapshot().syncing && !service.snapshot().network_available);
        assert(stops == 1 && service.snapshot().sync_generation == 0);
        notifyGotIp();
        service.tick();
        assert(starts == 1);  // expiry closes the gate until a deliberate reopen
        service.setNetworkEnabled(true, true, first_deadline);
        assert(starts == 1);  // an already-expired window never starts work

        service.setNetworkEnabled(true, true, now.load() + 90 * SECOND_US);
        assert(starts == 2);
        const auto shortened_deadline = now.load() + 2 * SECOND_US;
        service.setNetworkEnabled(true, true, shortened_deadline);
        assert(starts == 2);  // updating an enabled gate does not restart it
        now = shortened_deadline;
        reply();
        service.tick();
        assert(!service.snapshot().valid && stops == 2);

        const auto short_deadline = now.load() + 2 * SECOND_US;
        service.setNetworkEnabled(true, true, short_deadline);
        assert(starts == 3);
        ++now;
        service.setNetworkEnabled(true);  // normal-mode restoration clears cutoff
        now = short_deadline + SECOND_US;
        reply(BASE_EPOCH);
        service.tick();
        assert(service.snapshot().valid && service.snapshot().sync_generation == 1);
        assert(service.snapshot().last_successful_sync_us == now.load());

        service.setNetworkEnabled(false);
        const auto accepted_deadline = now.load() + 5 * SECOND_US;
        service.setNetworkEnabled(true, true, accepted_deadline);
        now = accepted_deadline - 1;
        reply(BASE_EPOCH + 100);
        now = accepted_deadline + SECOND_US;
        service.tick();  // a timely reply remains accepted even if tick is late
        assert(service.snapshot().sync_generation == 2 && !service.snapshot().network_available);
        assert(service.snapshot().last_successful_sync_us == accepted_deadline - 1);
        assert(service.snapshot().epoch_seconds == BASE_EPOCH + 101);

        dns_mode = DnsMode::Deferred;
        const auto dns_deadline = now.load() + 5 * SECOND_US;
        service.setNetworkEnabled(true, true, dns_deadline);
        assert(pending_dns.size() == 1);
        const auto before_expired_dns = starts;
        now = dns_deadline;
        completeDns(0, true);
        notifyGotIp();
        service.tick();
        assert(starts == before_expired_dns && !service.snapshot().network_available);
        assert(service.snapshot().sync_generation == 2);

        dns_mode = DnsMode::Immediate;
        const auto dispatch_deadline = now.load() + 5 * SECOND_US;
        // The second dispatch is native SNTP init; it can wait behind TCP/IP
        // work until after the main task already checked the DNS result.
        tcpip_calls_before_advance = 2;
        advance_tcpip_to_us = dispatch_deadline;
        service.setNetworkEnabled(true, true, dispatch_deadline);
        assert(starts == before_expired_dns);
        service.tick();
        assert(!service.snapshot().network_available && !service.snapshot().syncing);
        const auto before_expired_lookup = lookups;
        tcpip_calls_before_advance = 1;
        advance_tcpip_to_us = now.load() + SECOND_US;
        service.setNetworkEnabled(true, true, advance_tcpip_to_us);
        service.tick();
        assert(lookups == before_expired_lookup && starts == before_expired_dns);
        assert(service.snapshot().sync_generation == 2);
        puts("PASS: exact whole-window deadline, late callbacks/DNS/TCP-IP dispatch, live deadline updates and accepted-response retention");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "dns-lifecycle") == 0) {
        assert(service.begin());
        dns_mode = DnsMode::Deferred;
        gotIp();
        service.tick();
        assert(lookups == 1 && starts == 0 && pending_dns.size() == 1 && service.snapshot().syncing);
        now += 5 * SECOND_US;
        disconnect();
        completeDns(0, true);  // late success before tick observes disconnect
        service.tick();
        assert(starts == 0 && !service.snapshot().syncing && !service.snapshot().network_available);
        gotIp();
        service.tick();
        ++now;
        gotIp();
        service.tick();
        assert(lookups == 3 && pending_dns.size() == 2);
        completeDns(0, true);  // old DNS cannot target the new attempt
        service.tick();
        assert(starts == 0 && service.snapshot().syncing);
        completeDns(0, false);  // current DNS failure takes bounded retry path
        service.tick();
        assert(starts == 0 && !service.snapshot().syncing);
        now += 30 * SECOND_US;
        service.tick();
        now += 29 * SECOND_US;
        completeDns(0, true);
        service.tick();
        assert(starts == 1);
        now += SECOND_US;
        reply();  // DNS already consumed 29s: no extra 30s NTP window
        service.tick();
        assert(stops == 1 && !service.snapshot().valid);
        gotIp();
        service.tick();
        now += ATTEMPT_US;
        service.tick();
        assert(pending_dns.size() == 1 && !service.snapshot().syncing);
        now += 30 * SECOND_US;
        service.tick();
        assert(pending_dns.size() == 2);
        completeDns(0, false);  // old failure after a newer attempt is harmless
        service.tick();
        assert(service.snapshot().syncing && starts == 1);
        completeDns(0, true);
        service.tick();
        reply();
        service.tick();
        assert(starts == 2 && service.snapshot().valid);
        disconnect();
        service.tick();
        for (int i = 0; i < 3; ++i) { gotIp(); service.tick(); }
        const auto before_exhaustion = lookups;
        gotIp();
        service.tick();
        assert(pending_dns.size() == 3 && lookups == before_exhaustion);
        assert(!service.snapshot().syncing && starts == 2);
        completeDns(0, true);
        completeDns(0, false);
        completeDns(0, true);
        service.tick();
        assert(starts == 2);  // no stale result resurrects a native SNTP client
        now += 30 * SECOND_US;
        service.tick();
        assert(pending_dns.size() == 1 && lookups == before_exhaustion + 1);
        completeDns(0, true);
        service.tick();
        reply(BASE_EPOCH + 100);
        service.tick();
        assert(starts == 3 && service.snapshot().epoch_seconds == BASE_EPOCH + 100);
        puts("PASS: deferred DNS lifecycle, generation fences, shared deadline, three-slot bounds and numeric-address lifetime");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "dns-failure") == 0) {
        assert(service.begin());
        dns_mode = DnsMode::Failure;
        gotIp();
        service.tick();
        assert(lookups == 1 && starts == 0 && !service.snapshot().syncing);
        now += 30 * SECOND_US;
        service.tick();
        assert(lookups == 2 && starts == 0);
        now += 120 * SECOND_US;
        service.tick();
        assert(lookups == 3 && starts == 0);
        now += COOLDOWN_US - 1;
        service.tick();
        assert(lookups == 3);
        ++now;
        service.tick();
        assert(lookups == 4 && starts == 0 && !service.snapshot().valid);
        dns_mode = DnsMode::Immediate;
        gotIp();
        service.tick();
        reply();
        service.tick();
        assert(service.snapshot().valid && starts == 1);
        puts("PASS: immediate DNS errors free contexts and follow bounded 30s/120s/1h retry policy");
        return 0;
    }
    assert(service.begin());
    assert(service.begin() && registration_calls == 2);  // idempotent
    assert(starts == 0 && !service.snapshot().network_available);
    emit(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED);
    service.tick();
    assert(starts == 0);  // association alone is not an IP or a trusted clock
    gotIp();
    service.tick();
    assert(starts == 1 && service.snapshot().network_available && service.snapshot().syncing);
    assert(!service.snapshot().valid);
    reply(0);
    reply(BASE_EPOCH, 1000000);
    service.tick();
    assert(!service.snapshot().valid);
    now = ATTEMPT_US - 1;
    service.tick();
    assert(stops == 0);
    now = ATTEMPT_US;
    lateReply();  // expired even if tick hasn't processed the deadline
    service.tick();
    assert(stops == 1 && !service.snapshot().syncing && !service.snapshot().valid);
    now = ATTEMPT_US + 30 * SECOND_US - 1;
    service.tick();
    assert(starts == 1);
    ++now;
    service.tick();
    assert(starts == 2);
    now += ATTEMPT_US;
    service.tick();
    assert(stops == 2);
    now += 120 * SECOND_US - 1;
    service.tick();
    assert(starts == 2);
    ++now;
    service.tick();
    assert(starts == 3);
    now += ATTEMPT_US;
    service.tick();
    assert(stops == 3);
    now += COOLDOWN_US - 1;
    service.tick();
    assert(starts == 3);
    ++now;
    service.tick();
    assert(starts == 4);
    assert(hostnames[0] == "ntp.aliyun.com" && hostnames[1] == "ntp.tencent.com" &&
           hostnames[2] == "pool.ntp.org" && hostnames[3] == hostnames[0]);

    // A disconnect fences replies immediately, even before the main loop runs.
    disconnect();
    reply();
    reply_during_stop = true;
    service.tick();
    assert(!service.snapshot().valid && !service.snapshot().syncing && !service.snapshot().network_available);
    now += 2 * DAY_US;
    for (int i = 0; i < 100; ++i) service.tick();
    assert(starts == 4);  // stale retained IP never restarts polling offline
    gotIp();
    service.tick();
    assert(starts == 5);
    now += SECOND_US;
    reply(BASE_EPOCH, 900000);
    service.tick();
    auto snapshot = service.snapshot();
    assert(snapshot.valid && snapshot.epoch_seconds == BASE_EPOCH);
    assert(snapshot.local_time.tm_hour == 23 && snapshot.local_time.tm_min == 59 && snapshot.local_time.tm_sec == 59);
    assert(snapshot.local_time.tm_mday == 18);
    const auto accepted_at = now.load();
    assert(service.nextMinuteDeadlineUs() == accepted_at + 100000);  // 23:59:59.900000 -> midnight
    const auto revision = snapshot.revision;
    now += 100000;
    snapshot = service.snapshot();
    assert(snapshot.epoch_seconds == BASE_EPOCH + 1 && snapshot.local_time.tm_mday == 19);
    assert(snapshot.local_time.tm_hour == 0 && snapshot.local_time.tm_min == 0);
    assert(snapshot.revision == revision);
    assert(service.nextMinuteDeadlineUs() == accepted_at + 60 * SECOND_US + 100000);
    now = accepted_at + DAY_US - 1;
    service.tick();
    assert(starts == 5);
    ++now;
    service.tick();
    assert(starts == 6);  // 24 hours from response, not request or civil midnight
    reply(BASE_EPOCH - 3600);  // backward NTP adjustment is accepted
    service.tick();
    assert(service.snapshot().epoch_seconds == BASE_EPOCH - 3600);
    assert(service.snapshot().revision > revision);
    now += SECOND_US;
    gotIp();  // fresh IP bypasses the newly scheduled 24-hour wait
    service.tick();
    assert(starts == 7);
    reply(BASE_EPOCH + 86400);  // forward NTP adjustment
    service.tick();
    assert(service.snapshot().epoch_seconds == BASE_EPOCH + 86400);
    disconnect();
    service.tick();
    now += DAY_US + 61 * SECOND_US;
    snapshot = service.snapshot();
    assert(snapshot.valid && !snapshot.network_available && snapshot.epoch_seconds == BASE_EPOCH + 2 * 86400 + 61);
    assert(starts == 7);

    // A sample received before a later disconnect remains a real successful sync.
    gotIp();
    service.tick();
    reply(BASE_EPOCH);
    disconnect();
    service.tick();
    assert(service.snapshot().valid && service.snapshot().epoch_seconds == BASE_EPOCH && !service.snapshot().network_available);
    gotIp();
    service.tick();
    const auto before_repeated_ip = starts;
    gotIp();
    reply(BASE_EPOCH + 100);  // old attempt invalidated as soon as GOT_IP is observed
    service.tick();
    assert(starts == before_repeated_ip + 1);
    assert(service.snapshot().epoch_seconds == BASE_EPOCH);
    reply(BASE_EPOCH + 200);
    service.tick();
    assert(service.snapshot().epoch_seconds == BASE_EPOCH + 200);
    gotIp();
    service.tick();
    emit(IP_EVENT, IP_EVENT_STA_LOST_IP);
    reply(BASE_EPOCH + 300);
    service.tick();
    assert(!service.snapshot().network_available && service.snapshot().epoch_seconds == BASE_EPOCH + 200);

    // Exercise production snapshot locking against main-task publish operations.
    std::atomic<bool> running{true};
    std::thread reader([&] {
        while (running.load()) {
            const auto current = service.snapshot();
            assert(current.valid && current.epoch_seconds >= BASE_EPOCH);
            assert(current.local_time.tm_year == 126);
        }
    });
    for (int i = 0; i < 1000; ++i) {
        now += SECOND_US;
        gotIp();
        service.tick();
        reply(BASE_EPOCH + i);
        service.tick();
    }
    running = false;
    reader.join();
    puts("PASS: production TimeService callbacks, retries, reconnects, stale IP/replies, 24-hour sync, offline ticking and snapshot concurrency");
}
