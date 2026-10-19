/* SPDX-License-Identifier: MIT */
#include "wifi_manager.h"
#include "captive_dns.h"
#include "wifi_request.h"
#include "wifi_policy.h"
#include "wifi_socket.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/inet.h>
#include <lwip/sockets.h>
#include <nvs.h>
#include <nvs_flash.h>

extern const unsigned char wifi_setup_html_start[] asm("_binary_wifi_setup_html_start");
extern const unsigned char wifi_setup_html_end[] asm("_binary_wifi_setup_html_end");

namespace {
constexpr char TAG[] = "WifiManager";
constexpr char AP_IP[] = "192.168.4.1";
constexpr std::int64_t SESSION_MS = 300000;
constexpr std::int64_t CONNECT_MS = 25000;
constexpr std::int64_t SUCCESS_MS = 10000;
std::int64_t nowMs() { return esp_timer_get_time() / 1000; }
void wipe(void* data, std::size_t length)
{
    volatile unsigned char* p = static_cast<volatile unsigned char*>(data);
    while (length--) *p++ = 0;
}
struct Credentials {
    char ssid[33] = {};
    char password[65] = {};
    bool valid() const { return wifi_validation::credentials(ssid, std::strlen(ssid), password, std::strlen(password)); }
};
struct StoredCredentials {
    std::uint32_t magic = 0x57494631;
    std::uint8_t version = 1;
    std::uint8_t ssid_length = 0;
    std::uint8_t password_length = 0;
    std::uint8_t reserved = 0;
    char ssid[32] = {};
    char password[64] = {};
};
static_assert(sizeof(StoredCredentials) == 104, "Stable NVS record layout");
class Lock {
public:
    explicit Lock(SemaphoreHandle_t mutex) : _mutex(mutex) { if (_mutex) xSemaphoreTake(_mutex, portMAX_DELAY); }
    ~Lock() { if (_mutex) xSemaphoreGive(_mutex); }
private:
    SemaphoreHandle_t _mutex;
};
void randomHex(char* out, unsigned bytes)
{
    unsigned char random[16];
    esp_fill_random(random, bytes);
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < bytes; ++i) {
        out[i * 2] = digits[random[i] >> 4];
        out[i * 2 + 1] = digits[random[i] & 15];
    }
    out[bytes * 2] = 0;
    wipe(random, sizeof(random));
}
esp_err_t jsonReply(httpd_req_t* request, const char* status, const char* data)
{
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(request, data, HTTPD_RESP_USE_STRLEN);
}
esp_err_t jsonObject(httpd_req_t* request, cJSON* object)
{
    if (!object) return jsonReply(request, "500 Internal Server Error", "{\"error\":\"Out of memory\"}");
    char* body = cJSON_PrintUnformatted(object);
    cJSON_Delete(object);
    if (!body) return jsonReply(request, "500 Internal Server Error", "{\"error\":\"Out of memory\"}");
    const esp_err_t result = jsonReply(request, "200 OK", body);
    cJSON_free(body);
    return result;
}
}  // namespace

struct WifiManager::Impl {
    enum class CommandType : std::uint8_t { Scan, Connect, Exit };
    struct Command {
        CommandType type = CommandType::Scan;
        bool from_web = false;
        char token[33] = {};
        Credentials credentials;
    };
    enum class EventType : std::uint8_t { Associated, Disconnected, GotIp, ScanDone, ApStarted, ApStopped };
    struct Event {
        EventType type;
        std::uint32_t generation;
        std::uint32_t status;
        char ssid[33];
        unsigned char bssid[6];
    };

    SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    QueueHandle_t commands = nullptr;
    QueueHandle_t events = nullptr;
    QueueHandle_t service_jobs = nullptr;
    TaskHandle_t service_task = nullptr;
    esp_event_handler_instance_t wifi_event = nullptr;
    esp_event_handler_instance_t ip_event = nullptr;
    esp_netif_t* sta = nullptr;
    esp_netif_t* ap = nullptr;
    httpd_handle_t http = nullptr;
    CaptiveDns dns;
    Snapshot published;
    char csrf[33] = {};
    Snapshot state;
    bool ready = false;
    bool init_failed = false;
    bool radio_initialized = false;
    bool radio_started = false;
    bool clock_mode = false;
    bool clock_sync_enabled = false;
    bool pending_provisioning = false;
    // Do not restart until the old driver's STOP event has crossed the default
    // event loop. Events stamped after stop were requested cannot become a new
    // connection/scan attempt simply because generation was incremented.
    bool radio_stop_pending = false;
    bool radio_stop_seen = false;
    std::uint32_t radio_stop_generation = 0;
    std::int64_t radio_stop_retry_at = 0;
    std::atomic<bool> accept_radio_events{false};
    std::atomic<std::uint32_t> stopped_generation{0};
    std::int64_t radio_start_retry_at = 0;
    bool linked = false;
    bool associated = false;
    bool candidate = false;
    bool persistence_error = false;
    bool attempt_active = false;
    bool connect_queued = false;
    bool wait_disconnect = false;
    bool scan_pending = false;
    bool scan_cancel_pending = false;
    std::int64_t scan_cancel_deadline = 0;
    std::int64_t scan_deadline = 0;
    bool ap_teardown = false;
    bool ap_disabled = true;
    bool ap_starting = false;
    bool ap_start_seen = false;
    std::int64_t ap_start_deadline = 0;
    bool stop_submitted = false;
    std::int64_t ap_stop_retry_at = 0;
    unsigned ap_stop_attempts = 0;
    std::atomic<bool> http_stopped{false};
    std::atomic<bool> event_overflow{false};
    std::atomic<std::uint32_t> generation{0};
    std::uint32_t active_generation = 0;
    unsigned attempts = 0;
    unsigned backoff_bursts = 0;
    unsigned char expected_bssid[6] = {};
    std::int64_t connect_at = 0;
    std::int64_t attempt_deadline = 0;
    std::int64_t disconnect_deadline = 0;
    std::int64_t retry_at = 0;
    std::int64_t session_deadline = 0;
    std::int64_t success_deadline = 0;
    Credentials stored;
    Credentials target;

    void publish()
    {
        Lock lock(mutex);
        state.radio_on = radio_started;
        state.radio_idle = ready && !radio_started && !radio_stop_pending && !ap_teardown && !pending_provisioning;
        state.station_connected = linked && radio_started && !radio_stop_pending;
        state.clock_mode = clock_mode;
        state.has_saved_credentials = stored.valid();
        const bool same_networks = state.networks.size() == published.networks.size() &&
            std::equal(state.networks.begin(), state.networks.end(), published.networks.begin(),
                [](const Network& a, const Network& b) { return a.ssid == b.ssid && a.rssi == b.rssi && a.secure == b.secure; });
        if (state.state == published.state && state.message == published.message && state.ssid == published.ssid &&
            state.ap_ssid == published.ap_ssid && state.ap_password == published.ap_password &&
            state.scan_busy == published.scan_busy && state.provisioning == published.provisioning &&
            state.radio_on == published.radio_on && state.radio_idle == published.radio_idle &&
            state.station_connected == published.station_connected &&
            state.clock_mode == published.clock_mode && state.has_saved_credentials == published.has_saved_credentials && same_networks) return;
        state.revision = published.revision + 1;
        published = state;
    }
    void status(const char* name, const char* message)
    {
        state.state = name;
        state.message = message;
        publish();
    }
    Snapshot read() const
    {
        Lock lock(mutex);
        Snapshot copy = published;
        copy.seconds_remaining = wifi_policy::remaining(copy.provisioning, nowMs(), session_deadline, success_deadline);
        return copy;
    }
    void deadlines(std::int64_t session, std::int64_t success)
    {
        Lock lock(mutex);
        session_deadline = session;
        success_deadline = success;
    }
    void touch()
    {
        if (state.provisioning && !success_deadline) deadlines(nowMs() + SESSION_MS, 0);
    }
    void clearScan()
    {
        if (state.scan_busy) {
            scan_cancel_pending = true;
            scan_cancel_deadline = nowMs() + 2000;
            esp_wifi_scan_stop();
        }
        esp_wifi_clear_ap_list();
        state.scan_busy = false;
        scan_pending = false;
    }
    void cancelConnection()
    {
        const bool was_active = wait_disconnect || attempt_active || linked || associated;
        attempt_active = false;
        connect_queued = false;
        associated = false;
        linked = false;
        retry_at = 0;
        generation.fetch_add(1);
        wait_disconnect = was_active;
        disconnect_deadline = nowMs() + 2000;
        if (was_active) {
            const esp_err_t error = esp_wifi_disconnect();
            if (error != ESP_OK) wait_disconnect = false;
        }
    }
    void scheduleConnect(const Credentials& credentials, bool submitted)
    {
        clearScan();
        cancelConnection();
        target = credentials;
        candidate = submitted;
        persistence_error = false;
        attempts = 0;
        backoff_bursts = 0;
        connect_queued = true;
        connect_at = nowMs() + 200;
        state.ssid = target.ssid;
        status("connecting", "Connecting to Wi-Fi...");
    }
    void beginAttempt()
    {
        if (!radio_started || radio_stop_pending || (clock_mode && !clock_sync_enabled)) return;
        state.ssid = target.ssid;
        if (!target.valid()) { connect_queued = false; return; }
        wifi_config_t config = {};
        std::memcpy(config.sta.ssid, target.ssid, std::strlen(target.ssid));
        std::memcpy(config.sta.password, target.password, std::strlen(target.password));
        config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
        config.sta.threshold.authmode = target.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        config.sta.pmf_cfg.capable = true;
        config.sta.pmf_cfg.required = false;
        config.sta.failure_retry_cnt = 0;
        const esp_err_t configured = esp_wifi_set_config(WIFI_IF_STA, &config);
        wipe(&config, sizeof(config));
        connect_queued = false;
        associated = false;
        active_generation = generation.fetch_add(1) + 1;
        ++attempts;
        attempt_active = true;
        attempt_deadline = nowMs() + CONNECT_MS;
        status("connecting", "Connecting to Wi-Fi...");
        if (configured != ESP_OK || esp_wifi_connect() != ESP_OK) failed("Unable to start Wi-Fi connection");
    }
    void failed(const char* message)
    {
        attempt_active = false;
        associated = false;
        linked = false;
        connect_queued = false;
        if (candidate || state.provisioning) {
            candidate = false;
            wipe(&target, sizeof(target));
            retry_at = 0;
            status("error", message);
            return;
        }
        if (stored.valid() && (!clock_mode || clock_sync_enabled)) {
            target = stored;
            retry_at = nowMs() + wifi_policy::retryDelay(attempts, backoff_bursts, esp_random());
            if (attempts >= 3) backoff_bursts = std::min(backoff_bursts + 1, 4u);
        }
        status("error", message);
    }
    bool loadCredentials()
    {
        nvs_handle_t handle;
        if (nvs_open("wifi_setup", NVS_READONLY, &handle) != ESP_OK) return false;
        StoredCredentials record;
        std::size_t length = sizeof(record);
        const esp_err_t error = nvs_get_blob(handle, "credentials", &record, &length);
        nvs_close(handle);
        const bool valid = error == ESP_OK && length == sizeof(record) && record.magic == 0x57494631 &&
            record.version == 1 && record.reserved == 0 && record.ssid_length <= 32 && record.password_length <= 64 &&
            wifi_validation::credentials(record.ssid, record.ssid_length, record.password, record.password_length);
        if (valid) {
            std::memcpy(stored.ssid, record.ssid, record.ssid_length);
            std::memcpy(stored.password, record.password, record.password_length);
        }
        wipe(&record, sizeof(record));
        return valid;
    }
    bool saveCredentials()
    {
        StoredCredentials record;
        record.ssid_length = std::strlen(target.ssid);
        record.password_length = std::strlen(target.password);
        std::memcpy(record.ssid, target.ssid, record.ssid_length);
        std::memcpy(record.password, target.password, record.password_length);
        nvs_handle_t handle;
        esp_err_t error = nvs_open("wifi_setup", NVS_READWRITE, &handle);
        if (error == ESP_OK) {
            error = nvs_set_blob(handle, "credentials", &record, sizeof(record));
            if (error == ESP_OK) error = nvs_commit(handle);
            nvs_close(handle);
        }
        wipe(&record, sizeof(record));
        if (error != ESP_OK) { ESP_LOGE(TAG, "Credential persistence failed: %s", esp_err_to_name(error)); return false; }
        stored = target;
        return true;
    }
    static void eventCallback(void* context, esp_event_base_t base, std::int32_t id, void* data)
    {
        auto* self = static_cast<Impl*>(context);
        Event event = {};
        event.generation = self->generation.load();
        if (base == WIFI_EVENT && id == WIFI_EVENT_STA_STOP) {
            // A dedicated inbox cannot lose the restart boundary if the normal
            // event queue was full when suspension began.
            self->stopped_generation.store(event.generation);
            return;
        }
        if (!self->accept_radio_events.load()) return;
        else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) event.type = EventType::ApStarted;
        else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STOP) event.type = EventType::ApStopped;
        else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) event.type = EventType::GotIp;
        else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_CONNECTED && data) {
            event.type = EventType::Associated;
            const auto* item = static_cast<wifi_event_sta_connected_t*>(data);
            std::memcpy(event.ssid, item->ssid, std::min<std::size_t>(item->ssid_len, 32));
            std::memcpy(event.bssid, item->bssid, 6);
        } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED && data) {
            event.type = EventType::Disconnected;
            event.status = static_cast<wifi_event_sta_disconnected_t*>(data)->reason;
        } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE && data) {
            event.type = EventType::ScanDone;
            event.status = static_cast<wifi_event_sta_scan_done_t*>(data)->status;
        } else return;
        if (xQueueSend(self->events, &event, 0) != pdTRUE) self->event_overflow.store(true);
    }
    static void serviceWorker(void* context)
    {
        auto* self = static_cast<Impl*>(context);
        httpd_handle_t server;
        while (true) {
            if (xQueueReceive(self->service_jobs, &server, portMAX_DELAY) != pdTRUE) continue;
            httpd_stop(server);
            self->http_stopped.store(true);
        }
    }
    void handleEvent(const Event& event)
    {
        if (!radio_started || radio_stop_pending || !accept_radio_events.load()) return;
        if (event.type == EventType::ApStarted) { if (ap_starting) ap_start_seen = true; return; }
        if (event.type == EventType::ApStopped) { if (!ap_starting) ap_start_seen = false; return; }
        if (event.type == EventType::ScanDone) {
            if (scan_cancel_pending) {
                scan_cancel_pending = false;
                esp_wifi_clear_ap_list();
                return;
            }
            if (!state.scan_busy) { esp_wifi_clear_ap_list(); return; }
            state.scan_busy = false;
            if (event.status != 0) { esp_wifi_clear_ap_list(); status("error", "Wi-Fi scan failed"); publish(); return; }
            std::uint16_t count = 0;
            if (esp_wifi_scan_get_ap_num(&count) != ESP_OK) { esp_wifi_clear_ap_list(); status("error", "Wi-Fi scan failed"); publish(); return; }
            count = std::min<std::uint16_t>(count, 32);
            std::vector<wifi_ap_record_t> records(count);
            const esp_err_t result = count ? esp_wifi_scan_get_ap_records(&count, records.data()) : ESP_OK;
            esp_wifi_clear_ap_list();
            if (result != ESP_OK) { status("error", "Wi-Fi scan failed"); publish(); return; }
            state.networks.clear();
            for (unsigned i = 0; i < count; ++i) {
                const std::size_t size = strnlen(reinterpret_cast<const char*>(records[i].ssid), 32);
                if (!size || !wifi_validation::utf8(reinterpret_cast<const char*>(records[i].ssid), size)) continue;
                Network network{std::string(reinterpret_cast<const char*>(records[i].ssid), size), records[i].rssi,
                                records[i].authmode != WIFI_AUTH_OPEN};
                const auto existing = std::find_if(state.networks.begin(), state.networks.end(),
                    [&network](const Network& value) { return value.ssid == network.ssid && value.secure == network.secure; });
                if (existing == state.networks.end()) state.networks.push_back(network);
                else if (existing->rssi < network.rssi) *existing = network;
            }
            std::sort(state.networks.begin(), state.networks.end(), [](const Network& a, const Network& b) { return a.rssi > b.rssi; });
            if (linked && persistence_error) status("error", "Connected, but saving failed. Retry setup.");
            else status(linked ? "connected" : (state.provisioning ? "provisioning" : "idle"), linked ? "Connected to Wi-Fi" : "Choose a Wi-Fi network");
            publish();
            return;
        }
        if (event.type == EventType::Disconnected) {
            if (wait_disconnect) { wait_disconnect = false; connect_at = nowMs() + 150; return; }
            if (event.generation != generation.load()) return;
            if (attempt_active || linked) {
                if (success_deadline) deadlines(session_deadline, 0);
                failed(event.status == WIFI_REASON_AUTH_FAIL || event.status == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT ||
                    event.status == WIFI_REASON_HANDSHAKE_TIMEOUT ? "Connection failed. Check the password." :
                    "Wi-Fi connection lost or unavailable");
            }
            return;
        }
        if (!wifi_policy::currentAttempt(attempt_active, wait_disconnect, event.generation, active_generation)) return;
        if (event.type == EventType::Associated) {
            if (target.ssid != std::string(event.ssid)) return;
            associated = true;
            std::memcpy(expected_bssid, event.bssid, 6);
            return;
        }
        if (event.type == EventType::GotIp && wifi_policy::mayAcceptIp(attempt_active, associated, wait_disconnect, event.generation, active_generation)) {
            wifi_ap_record_t connected = {};
            wifi_config_t config = {};
            esp_netif_ip_info_t ip = {};
            const bool correct = esp_wifi_sta_get_ap_info(&connected) == ESP_OK &&
                esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK && esp_netif_get_ip_info(sta, &ip) == ESP_OK && ip.ip.addr &&
                std::memcmp(connected.bssid, expected_bssid, 6) == 0 &&
                std::strncmp(reinterpret_cast<const char*>(connected.ssid), target.ssid, 32) == 0 &&
                std::memcmp(config.sta.ssid, target.ssid, 32) == 0 &&
                std::memcmp(config.sta.password, target.password, 64) == 0;
            wipe(&config, sizeof(config));
            if (!correct) return;
            attempt_active = false;
            linked = true;
            attempts = 0;
            backoff_bursts = 0;
            retry_at = 0;
            bool saved = true;
            if (candidate) saved = saveCredentials();
            candidate = false;
            persistence_error = !saved;
            status(saved ? "connected" : "error", saved ? "Connected to Wi-Fi" : "Connected, but saving failed. Retry setup.");
            if (state.provisioning && saved) deadlines(session_deadline, nowMs() + SUCCESS_MS);
        }
    }
    void launchScan()
    {
        if (clock_mode || !radio_started || radio_stop_pending) { scan_pending = false; return; }
        if (state.scan_busy || scan_cancel_pending || attempt_active || connect_queued || wait_disconnect) return;
        scan_pending = false;
        wifi_scan_config_t config = {};
        config.show_hidden = false;
        config.scan_type = WIFI_SCAN_TYPE_ACTIVE;
        config.scan_time.active.max = 120;
        if (esp_wifi_scan_start(&config, false) != ESP_OK) { status("error", "Unable to start Wi-Fi scan"); return; }
        state.scan_busy = true;
        scan_deadline = nowMs() + 12000;
        status("scanning", "Scanning Wi-Fi networks...");
        publish();
    }

    // All service requests are accepted only on an AP-local socket. Host and
    // Origin checks additionally prevent DNS-rebinding/cross-origin requests.
    bool requestAllowed(httpd_req_t* request, bool api)
    {
        sockaddr_storage address = {};
        socklen_t size = sizeof(address);
        std::uint32_t local_ip;
        if (getsockname(httpd_req_to_sockfd(request), reinterpret_cast<sockaddr*>(&address), &size) != 0 ||
            !wifi_socket::ipv4Address(reinterpret_cast<sockaddr*>(&address), size, local_ip) ||
            local_ip != inet_addr(AP_IP)) return false;
        { Lock lock(mutex); if (!published.provisioning || wifi_policy::expired(true, nowMs(), session_deadline, success_deadline)) return false; }
        if (!api) return true;
        char host[48] = {};
        if (httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host)) != ESP_OK ||
            (std::strcmp(host, AP_IP) != 0 && std::strcmp(host, "192.168.4.1:80") != 0)) return false;
        if (httpd_req_get_hdr_value_len(request, "Origin")) {
            char origin[64];
            if (httpd_req_get_hdr_value_str(request, "Origin", origin, sizeof(origin)) != ESP_OK ||
                (std::strcmp(origin, "http://192.168.4.1") != 0 && std::strcmp(origin, "http://192.168.4.1:80") != 0)) return false;
        }
        char site[24];
        if (httpd_req_get_hdr_value_len(request, "Sec-Fetch-Site") &&
            (httpd_req_get_hdr_value_str(request, "Sec-Fetch-Site", site, sizeof(site)) != ESP_OK || std::strcmp(site, "cross-site") == 0)) return false;
        return true;
    }
    static esp_err_t handleGet(httpd_req_t* request)
    {
        auto* self = static_cast<Impl*>(request->user_ctx);
        const bool api = std::strncmp(request->uri, "/api/", 5) == 0;
        if (!self->requestAllowed(request, api)) return jsonReply(request, "403 Forbidden", "{\"error\":\"Setup is available only through its hotspot\"}");
        if (!api) {
            char host[48] = {};
            httpd_req_get_hdr_value_str(request, "Host", host, sizeof(host));
            if (std::strcmp(request->uri, "/") != 0 ||
                (std::strcmp(host, AP_IP) != 0 && std::strcmp(host, "192.168.4.1:80") != 0)) {
                httpd_resp_set_status(request, "302 Found");
                httpd_resp_set_hdr(request, "Location", "http://192.168.4.1/");
                httpd_resp_set_hdr(request, "Cache-Control", "no-store");
                return httpd_resp_send(request, "Open http://192.168.4.1/", HTTPD_RESP_USE_STRLEN);
            }
            httpd_resp_set_type(request, "text/html; charset=utf-8");
            httpd_resp_set_hdr(request, "Cache-Control", "no-store");
            httpd_resp_set_hdr(request, "X-Content-Type-Options", "nosniff");
            httpd_resp_set_hdr(request, "X-Frame-Options", "DENY");
            httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'; img-src 'self' data:; frame-ancestors 'none'; form-action 'self'");
            return httpd_resp_send(request, reinterpret_cast<const char*>(wifi_setup_html_start), wifi_setup_html_end - wifi_setup_html_start);
        }
        const Snapshot view = self->read();
        cJSON* root = cJSON_CreateObject();
        if (!root) return jsonReply(request, "500 Internal Server Error", "{\"error\":\"Out of memory\"}");
        if (std::strcmp(request->uri, "/api/session") == 0) {
            Lock lock(self->mutex);
            cJSON_AddStringToObject(root, "token", self->csrf);
        } else if (std::strcmp(request->uri, "/api/status") == 0) {
            cJSON_AddStringToObject(root, "state", view.state.c_str());
            cJSON_AddStringToObject(root, "message", view.message.c_str());
            cJSON_AddStringToObject(root, "ssid", view.ssid.c_str());
            cJSON_AddNumberToObject(root, "seconds_remaining", view.seconds_remaining);
        } else if (std::strcmp(request->uri, "/api/scan") == 0) {
            cJSON_AddBoolToObject(root, "busy", view.scan_busy);
            cJSON* networks = cJSON_AddArrayToObject(root, "networks");
            for (const auto& network : view.networks) {
                cJSON* item = cJSON_CreateObject();
                if (!item || !networks) { cJSON_Delete(item); break; }
                cJSON_AddStringToObject(item, "ssid", network.ssid.c_str());
                cJSON_AddNumberToObject(item, "rssi", network.rssi);
                cJSON_AddBoolToObject(item, "secure", network.secure);
                cJSON_AddItemToArray(networks, item);
            }
        } else { cJSON_Delete(root); return jsonReply(request, "404 Not Found", "{\"error\":\"Not found\"}"); }
        return jsonObject(request, root);
    }
    static esp_err_t handlePost(httpd_req_t* request)
    {
        auto* self = static_cast<Impl*>(request->user_ctx);
        if (!self->requestAllowed(request, true)) return jsonReply(request, "403 Forbidden", "{\"error\":\"Setup request rejected\"}");
        const bool connect = std::strcmp(request->uri, "/api/connect") == 0;
        const bool scan = std::strcmp(request->uri, "/api/scan") == 0;
        const bool exit = std::strcmp(request->uri, "/api/exit") == 0;
        if (!connect && !scan && !exit) return jsonReply(request, "404 Not Found", "{\"error\":\"Not found\"}");
        char content_type[64] = {};
        if (httpd_req_get_hdr_value_str(request, "Content-Type", content_type, sizeof(content_type)) != ESP_OK ||
            std::strncmp(content_type, "application/json", 16) != 0 || (content_type[16] && content_type[16] != ';'))
            return jsonReply(request, "415 Unsupported Media Type", "{\"error\":\"Use application/json\"}");
        if (request->content_len == 0 || request->content_len > 512) return jsonReply(request, "413 Payload Too Large", "{\"error\":\"Request must be 1 to 512 bytes\"}");
        char body[513] = {};
        std::size_t received = 0;
        const auto receive_deadline = nowMs() + 3000;
        while (received < request->content_len) {
            if (nowMs() >= receive_deadline) { wipe(body, sizeof(body)); return ESP_FAIL; }
            const int count = httpd_req_recv(request, body + received, request->content_len - received);
            if (count <= 0) { wipe(body, sizeof(body)); return ESP_FAIL; }
            received += count;
        }
        wifi_request::Request input;
        const bool valid = wifi_request::parse(body, received, connect, input);
        wipe(body, sizeof(body));
        if (!valid) { wipe(&input, sizeof(input)); return jsonReply(request, "400 Bad Request", "{\"error\":\"Invalid request, SSID or password\"}"); }
        bool authorized;
        { Lock lock(self->mutex); authorized = self->published.provisioning && wifi_validation::equalToken(input.token, self->csrf); }
        if (!authorized) { wipe(&input, sizeof(input)); return jsonReply(request, "403 Forbidden", "{\"error\":\"Session expired\"}"); }
        const Snapshot view = self->read();
        if ((connect || scan) && view.state == "connecting") {
            wipe(&input, sizeof(input));
            return jsonReply(request, "409 Conflict", "{\"error\":\"Wait for the current connection attempt\"}");
        }
        Command command;
        command.type = connect ? CommandType::Connect : (scan ? CommandType::Scan : CommandType::Exit);
        command.from_web = true;
        std::memcpy(command.token, input.token, sizeof(command.token));
        std::memcpy(command.credentials.ssid, input.ssid, sizeof(input.ssid));
        std::memcpy(command.credentials.password, input.password, sizeof(input.password));
        wipe(&input, sizeof(input));
        const bool queued = xQueueSend(self->commands, &command, 0) == pdTRUE;
        wipe(&command, sizeof(command));
        return queued ? jsonReply(request, "202 Accepted", "{\"ok\":true}") :
            jsonReply(request, "503 Service Unavailable", "{\"error\":\"Busy. Please retry\"}");
    }
    static esp_err_t openSocket(httpd_handle_t server, int socket)
    {
        auto* self = static_cast<Impl*>(httpd_get_global_user_ctx(server));
        sockaddr_storage local = {}, peer = {};
        std::uint32_t local_ip, peer_ip;
        socklen_t local_size = sizeof(local), peer_size = sizeof(peer);
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &local_size) != 0 ||
            getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &peer_size) != 0 ||
            !wifi_socket::ipv4Address(reinterpret_cast<sockaddr*>(&local), local_size, local_ip) ||
            !wifi_socket::ipv4Address(reinterpret_cast<sockaddr*>(&peer), peer_size, peer_ip) ||
            local_ip != inet_addr(AP_IP)) return ESP_FAIL;
        { Lock lock(self->mutex); if (!self->published.provisioning) return ESP_FAIL; }
        struct ifreq interface_request = {};
        if (esp_netif_get_netif_impl_name(self->ap, interface_request.ifr_name) != ESP_OK ||
            setsockopt(socket, SOL_SOCKET, SO_BINDTODEVICE, &interface_request, sizeof(interface_request)) != 0)
            return ESP_FAIL;
        // A DHCP lease belonging to a currently associated AP station is also
        // required, so an overlapping STA subnet cannot reach the portal.
        wifi_sta_list_t stations = {};
        if (esp_wifi_ap_get_sta_list(&stations) != ESP_OK || stations.num != 1) return ESP_FAIL;
        esp_netif_pair_mac_ip_t lease = {};
        std::memcpy(lease.mac, stations.sta[0].mac, sizeof(lease.mac));
        if (esp_netif_dhcps_get_clients_by_mac(self->ap, 1, &lease) != ESP_OK ||
            lease.ip.addr != peer_ip) return ESP_FAIL;
        return ESP_OK;
    }
    bool startHttp()
    {
        httpd_config_t config = HTTPD_DEFAULT_CONFIG();
        config.stack_size = 6144;
        config.max_uri_handlers = 4;
        config.max_open_sockets = 4;
        config.lru_purge_enable = true;
        config.recv_wait_timeout = 2;
        config.send_wait_timeout = 2;
        config.uri_match_fn = httpd_uri_match_wildcard;
        config.global_user_ctx = this;
        config.global_user_ctx_free_fn = [](void*) {};  // Singleton storage is not heap-owned by HTTPD.
        config.open_fn = openSocket;
        if (httpd_start(&http, &config) != ESP_OK) return false;
        httpd_uri_t get = {};
        get.uri = "/*"; get.method = HTTP_GET; get.handler = handleGet; get.user_ctx = this;
        httpd_uri_t post = {};
        post.uri = "/api/*"; post.method = HTTP_POST; post.handler = handlePost; post.user_ctx = this;
        return httpd_register_uri_handler(http, &get) == ESP_OK && httpd_register_uri_handler(http, &post) == ESP_OK;
    }
    void startServicesWhenReady()
    {
        if (clock_mode || !radio_started || radio_stop_pending || !ap_starting || !state.provisioning) return;
        if (ap_start_seen && esp_netif_is_netif_up(ap)) {
            ap_starting = false;
            const esp_err_t dhcp = esp_netif_dhcps_start(ap);
            if ((dhcp != ESP_OK && dhcp != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) || !dns.start() || !startHttp()) {
                closePortal();
                status("error", "Unable to start setup web service");
                return;
            }
            status("provisioning", "Join hotspot, then open 192.168.4.1");
            scan_pending = true;
        } else if (nowMs() >= ap_start_deadline) {
            closePortal();
            status("error", "Setup hotspot startup timed out");
        }
    }
    void closePortal()
    {
        if (pending_provisioning) {
            pending_provisioning = false;
            state.provisioning = false;
            publish();
        }
        if (ap_teardown) return;
        if (!state.provisioning && !http && !ap_teardown) return;
        const bool cancel = attempt_active || connect_queued || candidate;
        ap_starting = false;
        ap_start_seen = false;
        state.provisioning = false;
        state.ap_ssid.clear();
        if (!state.ap_password.empty()) wipe(&state.ap_password[0], state.ap_password.size());
        state.ap_password.clear();
        { Lock lock(mutex); if (!published.ap_password.empty()) wipe(&published.ap_password[0], published.ap_password.size()); }
        { Lock lock(mutex); wipe(csrf, sizeof(csrf)); session_deadline = 0; success_deadline = 0; }
        dns.stop();
        clearScan();
        if (cancel) { cancelConnection(); candidate = false; wipe(&target, sizeof(target)); }
        ap_teardown = true;
        ap_disabled = false;
        ap_stop_retry_at = 0;
        ap_stop_attempts = 0;
        stop_submitted = false;
        http_stopped.store(http == nullptr);
        publish();
        if (!linked) status("idle", "Wi-Fi setup closed");
        // HTTPD header parsing can be held by a slow peer before URI dispatch.
        // Revoke access and stop DNS first, request HTTP shutdown, then remove
        // the AP immediately; never keep its radio alive waiting for a peer.
        if (http && xQueueSend(service_jobs, &http, 0) == pdTRUE) stop_submitted = true;
        disableHotspot();
    }
    void disableHotspot()
    {
        if (ap_disabled || nowMs() < ap_stop_retry_at) return;
        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
            ap_stop_attempts = std::min(ap_stop_attempts + 1, 5u);
            ap_stop_retry_at = nowMs() + std::min(30000u, 1000u << ap_stop_attempts);
            status("error", "Unable to stop setup hotspot. Retrying.");
            return;
        }
        ap_disabled = true;
    }
    void completeTeardown()
    {
        if (!ap_teardown) return;
        if (http && !stop_submitted) {
            if (xQueueSend(service_jobs, &http, 0) == pdTRUE) stop_submitted = true;
        }
        disableHotspot();
        if (!http_stopped.load() || !ap_disabled) return;
        http = nullptr;
        ap_teardown = false;
        stop_submitted = false;
        if (!linked && stored.valid() && !clock_mode && radio_started && !radio_stop_pending && !pending_provisioning)
            scheduleConnect(stored, false);
        else publish();
    }
    bool wantsRadio() const { return !clock_mode || clock_sync_enabled; }
    void stopRadio()
    {
        if (!radio_initialized || (!radio_started && !radio_stop_pending)) return;
        if (!radio_stop_pending) {
            accept_radio_events.store(false);
            clearScan();
            cancelConnection();
            scan_cancel_pending = false;
            scan_cancel_deadline = 0;
            state.networks.clear();
            candidate = false;
            wipe(&target, sizeof(target));
            wait_disconnect = false;
            radio_stop_pending = radio_started;
            radio_stop_seen = false;
            radio_stop_generation = generation.load();
            radio_stop_retry_at = 0;
        }
        if (!radio_started || nowMs() < radio_stop_retry_at) return;
        const esp_err_t result = esp_wifi_stop();
        if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_STARTED) {
            radio_stop_retry_at = nowMs() + 1000;
            status("error", "Unable to power down Wi-Fi. Retrying.");
            return;
        }
        radio_started = false;
        // NOT_STARTED means there is no live driver and no STOP event to await.
        if (result == ESP_ERR_WIFI_NOT_STARTED) radio_stop_seen = true;
        ap_disabled = true;
        ap_start_seen = false;
        status("radio_off", "Clock mode: Wi-Fi powered off");
    }
    void advanceRadio()
    {
        if (!wantsRadio() || (radio_stop_pending && radio_started)) stopRadio();
        if (radio_stop_pending) {
            radio_stop_seen = radio_stop_seen || stopped_generation.load() == radio_stop_generation;
            if (radio_started || !radio_stop_seen) return;
            radio_stop_pending = false;
            radio_stop_seen = false;
            publish();
        }
        if (!wantsRadio() || radio_started || ap_teardown || nowMs() < radio_start_retry_at) return;
        if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK || esp_wifi_start() != ESP_OK) {
            radio_start_retry_at = nowMs() + 5000;
            status("error", "Unable to restart Wi-Fi");
            return;
        }
        radio_started = true;
        radio_start_retry_at = 0;
        accept_radio_events.store(true);
        if (!pending_provisioning && stored.valid()) scheduleConnect(stored, false);
        else if (!pending_provisioning) status("idle", "Hold button A to set up Wi-Fi");
        else publish();
    }
};

WifiManager::WifiManager()
{
    static Impl implementation;
    _impl = &implementation;
}
WifiManager& WifiManager::instance()
{
    static WifiManager manager;
    return manager;
}

bool WifiManager::begin()
{
    auto& s = *_impl;
    if (s.ready) return true;
    if (s.init_failed) return false;
    if (!s.mutex) {
        s.init_failed = true;
        s.state.state = s.published.state = "error";
        s.state.message = s.published.message = "Not enough memory for Wi-Fi";
        return false;
    }
    auto fail = [&](const char* message) { s.init_failed = true; s.status("error", message); return false; };
    // Never erase the shared partition: it also contains factory-boot state.
    if (nvs_flash_init() != ESP_OK) return fail("Wi-Fi storage initialization failed");
    esp_err_t error = esp_netif_init();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return fail("Network initialization failed");
    error = esp_event_loop_create_default();
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) return fail("Network event initialization failed");
    s.commands = xQueueCreate(8, sizeof(Impl::Command));
    s.events = xQueueCreate(32, sizeof(Impl::Event));
    s.service_jobs = xQueueCreate(1, sizeof(httpd_handle_t));
    if (!s.commands || !s.events || !s.service_jobs) return fail("Not enough memory for Wi-Fi");
    if (xTaskCreate(Impl::serviceWorker, "wifi_service", 3072, &s, 4, &s.service_task) != pdPASS)
        return fail("Unable to start Wi-Fi service worker");
    s.sta = esp_netif_create_default_wifi_sta();
    s.ap = esp_netif_create_default_wifi_ap();
    if (!s.sta || !s.ap) return fail("Unable to create Wi-Fi interfaces");
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&config) != ESP_OK) return fail("Wi-Fi driver initialization failed");
    s.radio_initialized = true;
    if (esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK || esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK)
        return fail("Wi-Fi station configuration failed");
    if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, Impl::eventCallback, &s, &s.wifi_event) != ESP_OK ||
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, Impl::eventCallback, &s, &s.ip_event) != ESP_OK)
        return fail("Wi-Fi event registration failed");
    if (esp_wifi_start() != ESP_OK) return fail("Wi-Fi station startup failed");
    s.radio_started = true;
    s.accept_radio_events.store(true);
    s.ready = true;
    if (s.loadCredentials()) s.scheduleConnect(s.stored, false);
    else s.status("idle", "Hold button A to set up Wi-Fi");
    return true;
}

bool WifiManager::startProvisioning()
{
    if (!begin()) return false;
    auto& s = *_impl;
    if (s.clock_mode) return false;
    if (s.pending_provisioning) return true;
    if (s.ap_teardown || s.radio_stop_pending || !s.radio_started) {
        // Preserve the UI modal immediately, then start the real portal once
        // the old STOP boundary and asynchronous HTTP shutdown have completed.
        s.pending_provisioning = true;
        s.state.provisioning = true;
        s.status("provisioning", "Starting setup hotspot...");
        return true;
    }
    if (s.state.provisioning) return true;
    s.clearScan();
    s.cancelConnection();
    s.candidate = false;
    wipe(&s.target, sizeof(s.target));
    s.state.ssid.clear();
    s.state.networks.clear();
    unsigned char mac[6];
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) { s.status("error", "Unable to read hotspot identity"); return false; }
    char name[33];
    std::snprintf(name, sizeof(name), "PaperDIY-%02X%02X%02X", mac[3], mac[4], mac[5]);
    char password[17];
    char token[33];
    randomHex(password, 8);
    randomHex(token, 16);
    wifi_config_t config = {};
    std::memcpy(config.ap.ssid, name, std::strlen(name));
    config.ap.ssid_len = std::strlen(name);
    std::memcpy(config.ap.password, password, std::strlen(password));
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    config.ap.max_connection = 1;
    config.ap.channel = 1;
    config.ap.pmf_cfg.required = false;
    esp_netif_dhcps_stop(s.ap);
    esp_netif_ip_info_t info = {};
    info.ip.addr = inet_addr(AP_IP);
    info.gw.addr = info.ip.addr;
    info.netmask.addr = inet_addr("255.255.255.0");
    esp_netif_dns_info_t dns_info = {};
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    dns_info.ip.u_addr.ip4.addr = info.ip.addr;
    std::uint8_t offer_dns = 1;
    s.ap_starting = true;
    s.ap_disabled = false;
    s.ap_start_seen = false;
    s.ap_start_deadline = nowMs() + 5000;
    const bool configured = esp_netif_set_ip_info(s.ap, &info) == ESP_OK &&
        esp_netif_set_dns_info(s.ap, ESP_NETIF_DNS_MAIN, &dns_info) == ESP_OK &&
        esp_netif_dhcps_option(s.ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns)) == ESP_OK &&
        esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK && esp_wifi_set_config(WIFI_IF_AP, &config) == ESP_OK;
    wipe(&config, sizeof(config));
    if (!configured) {
        s.ap_starting = false;
        wipe(password, sizeof(password)); wipe(token, sizeof(token));
        esp_wifi_set_mode(WIFI_MODE_STA);
        s.status("error", "Unable to start setup hotspot");
        if (s.stored.valid()) s.retry_at = nowMs() + 2000;
        return false;
    }
    s.state.provisioning = true;
    s.state.ap_ssid = name;
    s.state.ap_password = password;
    { Lock lock(s.mutex); std::memcpy(s.csrf, token, sizeof(token)); }
    wipe(password, sizeof(password)); wipe(token, sizeof(token));
    s.deadlines(nowMs() + SESSION_MS, 0);
    s.publish();
    // The default AP_START handler creates/raises the lwIP interface. Do not
    // bind DNS or accept HTTP until that event has passed through our queue.
    s.status("provisioning", "Starting setup hotspot...");
    return true;
}

void WifiManager::stopProvisioning() { _impl->closePortal(); }
bool WifiManager::provisioning() const { Lock lock(_impl->mutex); return _impl->published.provisioning; }
WifiManager::Snapshot WifiManager::snapshot() const { return _impl->read(); }
void WifiManager::requestScan()
{
    if (!_impl->commands) return;
    { Lock lock(_impl->mutex); if (_impl->published.clock_mode) return; }
    Impl::Command command;
    xQueueSend(_impl->commands, &command, 0);
}

void WifiManager::setClockMode(bool active)
{
    auto& s = *_impl;
    if (s.clock_mode == active) return;
    s.clock_mode = active;
    s.clock_sync_enabled = false;
    if (active) {
        s.closePortal();
        s.stopRadio();
    }
    // Resume only in tick(), after draining the old event inbox. A long-A
    // setup request can queue meanwhile without reviving a prior AP event.
    s.publish();
}

void WifiManager::setClockSyncEnabled(bool enabled)
{
    auto& s = *_impl;
    if (!s.clock_mode || s.clock_sync_enabled == enabled) return;
    s.clock_sync_enabled = enabled;
    if (!enabled) s.stopRadio();
    // Starting is deferred to tick(), after STOP events/portal teardown.
    s.publish();
}

void WifiManager::tick()
{
    auto& s = *_impl;
    if (!s.ready) return;
    if (s.event_overflow.exchange(false) && s.radio_started && !s.radio_stop_pending) {
        s.clearScan();
        s.cancelConnection();
        s.failed("Wi-Fi events interrupted. Please retry.");
    }
    Impl::Event event;
    for (unsigned i = 0; i < 32 && xQueueReceive(s.events, &event, 0) == pdTRUE; ++i) s.handleEvent(event);
    Impl::Command command;
    for (unsigned i = 0; i < 8 && xQueueReceive(s.commands, &command, 0) == pdTRUE; ++i) {
        bool current = !s.clock_mode && !command.from_web;
        if (command.from_web) {
            Lock lock(s.mutex);
            current = !s.clock_mode && s.published.provisioning && !wifi_policy::expired(true, nowMs(), s.session_deadline, s.success_deadline) &&
                wifi_validation::equalToken(command.token, s.csrf);
        }
        if (current) {
            if (command.type == Impl::CommandType::Exit) s.closePortal();
            else if (!s.ap_teardown && command.type == Impl::CommandType::Scan) {
                s.touch();
                if (!s.state.scan_busy) s.scan_pending = true;
            } else if (command.type == Impl::CommandType::Connect && s.state.provisioning && !s.attempt_active && !s.connect_queued && command.credentials.valid()) {
                s.deadlines(nowMs() + SESSION_MS, 0);
                s.scheduleConnect(command.credentials, true);
            }
        }
        wipe(&command, sizeof(command));
    }
    s.completeTeardown();
    s.advanceRadio();
    if (s.pending_provisioning && !s.clock_mode && s.radio_started && !s.radio_stop_pending && !s.ap_teardown) {
        s.pending_provisioning = false;
        s.state.provisioning = false;
        startProvisioning();
    }
    if (s.pending_provisioning || !s.radio_started || s.radio_stop_pending || (s.clock_mode && !s.clock_sync_enabled)) return;
    s.startServicesWhenReady();
    const auto now = nowMs();
    if (s.state.provisioning) {
        s.dns.tick();
        if (wifi_policy::expired(true, now, s.session_deadline, s.success_deadline)) s.closePortal();
    }
    if (s.wait_disconnect && now >= s.disconnect_deadline) {
        // Never start a new credential attempt without its old disconnect
        // boundary. A missing event fails closed instead of saving stale IPs.
        s.wait_disconnect = false;
        if (s.connect_queued) s.failed("Connection did not stop. Please retry.");
    }
    if (s.attempt_active && now >= s.attempt_deadline) {
        s.cancelConnection();
        s.failed("Connection timed out. Check Wi-Fi details.");
    }
    if (s.ap_teardown) return;
    if (s.state.scan_busy && now >= s.scan_deadline) {
        s.clearScan();
        s.status("error", "Wi-Fi scan timed out. Please retry.");
    }
    if (s.scan_cancel_pending && s.scan_pending && now >= s.scan_cancel_deadline) {
        // Do not let an arbitrarily late canceled DONE finish a newer scan.
        // A missing event fails this request; the fence clears on its DONE.
        s.scan_pending = false;
        s.status("error", "Previous scan is stopping. Please retry.");
    }
    if (s.connect_queued && !s.wait_disconnect && now >= s.connect_at) s.beginAttempt();
    if (s.retry_at && now >= s.retry_at && !s.state.provisioning && !s.wait_disconnect && !s.state.scan_busy) {
        s.retry_at = 0;
        if (s.attempts >= 3) s.attempts = 0;
        s.target = s.stored;
        s.connect_queued = true;
        s.connect_at = now + 200;
    }
    if (s.scan_pending && !s.clock_mode) s.launchScan();
}
