/* SPDX-License-Identifier: MIT */
#include "time_service.h"
#include "time_policy.h"

#include <cstring>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/dns.h>
#include <lwip/ip_addr.h>

namespace {
constexpr char TAG[] = "TimeService";
// Rotate the server between bounded attempts. A one-server config also works
// with the SDK default CONFIG_LWIP_SNTP_MAX_SERVERS=1.
constexpr const char* SERVERS[] = {"ntp.aliyun.com", "ntp.tencent.com", "pool.ntp.org"};
constexpr unsigned SERVER_COUNT = sizeof(SERVERS) / sizeof(SERVERS[0]);
}  // namespace

struct TimeService::Impl {
    struct NetworkNotice {
        std::uint32_t generation = 0;
        bool available = false;
    };
    struct SyncNotice {
        bool ready = false;
        std::uint32_t attempt = 0;
        std::int64_t epoch_seconds = 0;
        std::int64_t microseconds = 0;
        std::int64_t received_at = 0;
    };
    struct DnsNotice {
        bool ready = false;
        std::uint32_t attempt = 0;
        std::uint32_t network = 0;
        char address[16] = {};
    };
    struct DnsRequest {
        Impl* owner = nullptr;
        bool busy = false;
        std::uint32_t attempt = 0;
        std::uint32_t network = 0;
        const char* hostname = nullptr;
    };

    mutable portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    NetworkNotice network;
    SyncNotice sync;
    DnsNotice dns;
    // lwIP doesn't cancel in-flight DNS on SNTP stop. Resolve outside SNTP,
    // retaining each context until its one terminal callback. A fixed pool
    // bounds memory even under rapid disconnect/reconnect events.
    DnsRequest dns_requests[3];
    bool callback_active = false;
    std::uint32_t callback_network = 0;
    std::uint32_t callback_attempt = 0;
    std::int64_t callback_deadline = 0;
    Snapshot published;
    clock_policy::ClockAnchor published_anchor;

    // Main-task-only state. Callbacks touch only the small critical-section
    // inbox above; they never wait for an NTP reply or call into the UI/driver.
    clock_policy::SyncPolicy policy;
    clock_policy::ClockAnchor anchor;
    bool ready = false;
    bool sntp_initialized = false;
    std::uint32_t seen_network = 0;
    std::uint32_t attempt = 0;
    unsigned server_index = 0;
    char server_address[16] = {};
    esp_event_handler_instance_t wifi_handler = nullptr;
    esp_event_handler_instance_t ip_handler = nullptr;

    static void networkCallback(void* context, esp_event_base_t base, std::int32_t id, void* data)
    {
        auto& self = *static_cast<Impl*>(context);
        bool available;
        if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            const auto* event = static_cast<const ip_event_got_ip_t*>(data);
            if (!event || !event->ip_info.ip.addr) return;
            available = true;
        } else if ((base == WIFI_EVENT && (id == WIFI_EVENT_STA_DISCONNECTED || id == WIFI_EVENT_STA_STOP)) ||
                   (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP)) {
            available = false;
        } else return;
        portENTER_CRITICAL(&self.mux);
        self.network.available = available;
        ++self.network.generation;
        portEXIT_CRITICAL(&self.mux);
    }
    static void syncCallback(timeval* value)
    {
        if (!value || !clock_policy::plausibleTime(value->tv_sec, value->tv_usec)) return;
        auto& self = *TimeService::instance()._impl;
        const std::int64_t now = esp_timer_get_time();
        portENTER_CRITICAL(&self.mux);
        if (self.callback_active && self.network.available &&
            self.network.generation == self.callback_network && now < self.callback_deadline) {
            self.sync = {true, self.callback_attempt, static_cast<std::int64_t>(value->tv_sec),
                         static_cast<std::int64_t>(value->tv_usec), now};
            // Exactly one sample is accepted per attempt.
            self.callback_active = false;
        }
        portEXIT_CRITICAL(&self.mux);
    }
    static void dnsFound(const char*, const ip_addr_t* address, void* context)
    {
        auto& request = *static_cast<DnsRequest*>(context);
        auto& self = *request.owner;
        char numeric[16] = {};
        if (address && IP_IS_V4(address) && !ip_addr_isany(address)) {
            ipaddr_ntoa_r(address, numeric, sizeof(numeric));
        }
        const auto now = esp_timer_get_time();
        portENTER_CRITICAL(&self.mux);
        if (self.callback_active && self.network.available && now < self.callback_deadline &&
            self.callback_attempt == request.attempt && self.network.generation == request.network) {
            self.dns.ready = true;
            self.dns.attempt = request.attempt;
            self.dns.network = request.network;
            std::memcpy(self.dns.address, numeric, sizeof(numeric));
        }
        request.busy = false;
        portEXIT_CRITICAL(&self.mux);
        // Do not read request after releasing the lock: the main task may now
        // reuse this slot. Stale results never touch SNTP, its timers or its PCB.
    }
    static esp_err_t lookupOnTcpip(void* context)
    {
        auto& request = *static_cast<DnsRequest*>(context);
        auto& self = *request.owner;
        portENTER_CRITICAL(&self.mux);
        const bool current = self.callback_active && self.network.available &&
            self.callback_attempt == request.attempt && self.network.generation == request.network;
        portEXIT_CRITICAL(&self.mux);
        if (!current) {
            dnsFound(nullptr, nullptr, &request);
            return ESP_OK;
        }
        ip_addr_t result = {};
        const err_t error = dns_gethostbyname_addrtype(request.hostname, &result, dnsFound,
                                                     &request, LWIP_DNS_ADDRTYPE_IPV4);
        if (error != ERR_INPROGRESS) dnsFound(nullptr, error == ERR_OK ? &result : nullptr, &request);
        return ESP_OK;
    }
    static esp_err_t deinitOnTcpip(void*)
    {
        // Keep ESP-NETIF's callback storage teardown on the same thread as its
        // sync callback. Its nested TCP/IP call is direct when already there.
        esp_netif_sntp_deinit();
        return ESP_OK;
    }
    void stopSntp()
    {
        portENTER_CRITICAL(&mux);
        callback_active = false;
        sync.ready = false;
        dns.ready = false;
        portEXIT_CRITICAL(&mux);
        if (sntp_initialized) {
            // Use the ESP-NETIF API, which marshals lwIP operations to its
            // TCP/IP thread. Do not call raw lwIP sntp_init()/sntp_stop().
            esp_netif_tcpip_exec(deinitOnTcpip, nullptr);
            sntp_initialized = false;
        }
    }
    void startAttempt(std::int64_t now)
    {
        policy.started(now);
        ++attempt;
        DnsRequest* request = nullptr;
        portENTER_CRITICAL(&mux);
        sync.ready = false;
        dns.ready = false;
        callback_active = true;
        callback_network = seen_network;
        callback_attempt = attempt;
        callback_deadline = policy.attempt_deadline_us;
        for (auto& slot : dns_requests) {
            if (!slot.busy) {
                slot = {this, true, attempt, seen_network, SERVERS[server_index]};
                request = &slot;
                break;
            }
        }
        portEXIT_CRITICAL(&mux);
        server_index = (server_index + 1) % SERVER_COUNT;
        if (!request || esp_netif_tcpip_exec(lookupOnTcpip, request) != ESP_OK) {
            if (request) {
                portENTER_CRITICAL(&mux);
                request->busy = false;
                portEXIT_CRITICAL(&mux);
            }
            stopSntp();
            policy.failed(esp_timer_get_time());
            ESP_LOGW(TAG, "Unable to begin time-server DNS lookup; retry deferred");
        }
    }
    void startSntp(const DnsNotice& notice)
    {
        // A numeric address takes lwIP's immediate ipaddr_aton() path, so
        // deinitializing SNTP cannot leave its own asynchronous DNS callback.
        std::memcpy(server_address, notice.address, sizeof(server_address));
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server_address);
        config.wait_for_sync = false;
        config.sync_cb = syncCallback;
        const esp_err_t error = esp_netif_tcpip_exec([](void* context) -> esp_err_t {
            return esp_netif_sntp_init(static_cast<esp_sntp_config_t*>(context));
        }, &config);
        if (error == ESP_OK) {
            sntp_initialized = true;
        } else {
            // A failed init owns no SNTP instance; never deinit another owner's
            // service on ESP_ERR_INVALID_STATE.
            stopSntp();
            policy.failed(esp_timer_get_time());
            ESP_LOGW(TAG, "Unable to start time sync: %s", esp_err_to_name(error));
        }
    }
    void publish(bool changed_time = false)
    {
        portENTER_CRITICAL(&mux);
        if (changed_time || published.valid != anchor.valid || published.syncing != policy.syncing ||
            published.network_available != policy.network_available) {
            ++published.revision;
            published.valid = anchor.valid;
            published.syncing = policy.syncing;
            published.network_available = policy.network_available;
            published_anchor = anchor;
        }
        portEXIT_CRITICAL(&mux);
    }
};

TimeService::TimeService()
{
    static Impl implementation;
    _impl = &implementation;
}
TimeService& TimeService::instance()
{
    static TimeService service;
    return service;
}

bool TimeService::begin()
{
    auto& self = *_impl;
    if (self.ready) return true;
    // WifiManager is the only owner of network initialization. It must create
    // the default event loop first. Register observers before checking the
    // interface so an IP acquired during startup cannot fall through a gap.
    esp_err_t error = esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, Impl::networkCallback, &self, &self.wifi_handler);
    if (error != ESP_OK) return false;
    error = esp_event_handler_instance_register(
        IP_EVENT, ESP_EVENT_ANY_ID, Impl::networkCallback, &self, &self.ip_handler);
    if (error != ESP_OK) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, self.wifi_handler);
        self.wifi_handler = nullptr;
        return false;
    }
    portENTER_CRITICAL(&self.mux);
    const auto before_observation = self.network.generation;
    portEXIT_CRITICAL(&self.mux);
    esp_netif_t* station = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip = {};
    if (station && esp_netif_is_netif_up(station) && esp_netif_get_ip_info(station, &ip) == ESP_OK && ip.ip.addr) {
        portENTER_CRITICAL(&self.mux);
        // A notification during the query wins. This also handles retrying
        // begin() after a partial registration failure observed a disconnect.
        if (self.network.generation == before_observation && !self.network.available) {
            self.network.available = true;
            ++self.network.generation;
        }
        portEXIT_CRITICAL(&self.mux);
    }
    self.ready = true;
    tick();
    return true;
}

void TimeService::tick()
{
    auto& self = *_impl;
    if (!self.ready) return;
    Impl::NetworkNotice network;
    Impl::SyncNotice sync;
    portENTER_CRITICAL(&self.mux);
    network = self.network;
    sync = self.sync;
    self.sync.ready = false;
    portEXIT_CRITICAL(&self.mux);

    bool changed_time = false;
    // Process an already accepted reply before a later disconnect/GOT_IP.
    // A new GOT_IP still forces a fresh attempt even if the old one succeeded.
    if (sync.ready && self.policy.syncing && sync.attempt == self.attempt) {
        changed_time = self.anchor.accept(sync.epoch_seconds, sync.microseconds, sync.received_at);
        if (changed_time) {
            self.stopSntp();
            self.policy.succeeded(sync.received_at);
            ESP_LOGI(TAG, "Time synchronized (China UTC+8)");
        }
    }
    const std::int64_t now = esp_timer_get_time();
    if (network.generation != self.seen_network) {
        self.stopSntp();
        self.seen_network = network.generation;
        if (network.available) self.policy.gotIp(now);
        else self.policy.disconnected();
    }
    if (self.policy.timedOut(now)) {
        self.stopSntp();
        self.policy.failed(now);
        ESP_LOGW(TAG, "Time sync timed out; keeping the previous clock if available");
    }
    if (self.policy.due(now)) self.startAttempt(now);
    Impl::DnsNotice dns;
    portENTER_CRITICAL(&self.mux);
    dns = self.dns;
    self.dns.ready = false;
    const bool current_dns = self.callback_active && self.network.available &&
        self.network.generation == dns.network;
    portEXIT_CRITICAL(&self.mux);
    if (dns.ready && current_dns && self.policy.syncing && dns.attempt == self.attempt &&
        !self.policy.timedOut(esp_timer_get_time())) {
        if (dns.address[0]) self.startSntp(dns);
        else {
            self.stopSntp();
            self.policy.failed(esp_timer_get_time());
            ESP_LOGW(TAG, "Time-server DNS lookup failed; retry deferred");
        }
    }
    self.publish(changed_time);
}

TimeService::Snapshot TimeService::snapshot() const
{
    const auto& self = *_impl;
    Snapshot result;
    clock_policy::ClockAnchor anchor;
    portENTER_CRITICAL(&self.mux);
    result = self.published;
    anchor = self.published_anchor;
    portEXIT_CRITICAL(&self.mux);
    if (result.valid) {
        result.epoch_seconds = anchor.epochSeconds(esp_timer_get_time());
        result.local_time = clock_policy::chinaTime(result.epoch_seconds);
    }
    return result;
}
