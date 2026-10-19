/* SPDX-License-Identifier: MIT */
#include "sdk_mock.h"
#include "../../main/network/captive_dns.h"
#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <thread>

namespace sdk {
std::int64_t now_us = 0;
bool driver_running = false;
bool stop_pending = false;
bool associated = false;
bool saved_credentials = true;
bool tcpip_context = false;
bool sntp_running = false;
bool defer_dns = false;
wifi_mode_t mode = WIFI_MODE_NULL;
unsigned starts = 0;
unsigned stops = 0;
unsigned connects = 0;
unsigned scans = 0;
unsigned ap_enables = 0;
unsigned dns_starts = 0;
unsigned dns_stops = 0;
unsigned sntp_starts = 0;
unsigned sntp_stops = 0;
unsigned lookups = 0;
std::atomic<unsigned> http_starts{0};
std::atomic<unsigned> http_stops{0};
std::atomic<bool> block_http_stop{false};
unsigned fail_stops = 0;
unsigned fail_starts = 0;
std::vector<std::string> operations;
namespace {
esp_netif_t station;
esp_netif_t access_point;
wifi_config_t station_config;
wifi_config_t ap_config;
struct Handler { esp_event_base_t base; int id; esp_event_handler_t callback; void* context; bool active; };
std::vector<Handler> handlers;
struct PendingDns { std::string hostname; dns_found_callback callback; void* context; };
std::vector<PendingDns> pending_dns;
esp_sntp_time_cb_t sync_callback = nullptr;
esp_sntp_time_cb_t last_sync_callback = nullptr;
const char* retained_address = nullptr;
constexpr unsigned char BSSID[] = {0, 1, 2, 3, 4, 5};
struct Queue {
    const unsigned capacity;
    const unsigned item_size;
    std::mutex mutex;
    std::condition_variable available;
    std::deque<std::vector<unsigned char>> items;
};
void* http_context = nullptr;
}
void emit(esp_event_base_t base, int id, void* data)
{
    for (const auto& h : handlers) {
        if (h.active && h.base == base && (h.id == id || h.id == ESP_EVENT_ANY_ID))
            h.callback(h.context, base, id, data);
    }
}
void finishStop()
{
    assert(stop_pending);
    stop_pending = false;
    emit(WIFI_EVENT, WIFI_EVENT_STA_STOP);
}
void associate()
{
    assert(driver_running);
    associated = true;
    station.up = true;
    wifi_event_sta_connected_t event;
    std::memcpy(event.ssid, station_config.sta.ssid, 32);
    event.ssid_len = strnlen(reinterpret_cast<const char*>(event.ssid), 32);
    std::memcpy(event.bssid, BSSID, sizeof(BSSID));
    emit(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &event);
}
void gotIp()
{
    station.up = true;
    station.ip = 0x04030201;
    ip_event_got_ip_t ip{&station, {{station.ip}, {}, {}}, true};
    emit(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
}
void disconnect()
{
    associated = false;
    station.up = false;
    wifi_event_sta_disconnected_t event;
    emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
}
void apStarted()
{
    assert(driver_running && mode == WIFI_MODE_APSTA);
    access_point.up = true;
    emit(WIFI_EVENT, WIFI_EVENT_AP_START);
}
void scanDone()
{
    wifi_event_sta_scan_done_t event;
    emit(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &event);
}
void reply(std::int64_t epoch)
{
    assert(sntp_running && sync_callback && retained_address);
    assert(std::strcmp(retained_address, "203.0.113.1") == 0);
    timeval value{static_cast<time_t>(epoch), 0};
    sync_callback(&value);
}
void lateReply()
{
    assert(last_sync_callback);
    timeval value{1792439199, 0};
    last_sync_callback(&value);
}
void finishDns(bool success)
{
    assert(!pending_dns.empty());
    auto pending = pending_dns.front();
    pending_dns.erase(pending_dns.begin());
    ip_addr_t address{0xcb007101};
    const bool before = tcpip_context;
    tcpip_context = true;
    pending.callback(pending.hostname.c_str(), success ? &address : nullptr, pending.context);
    tcpip_context = before;
}
void setNow(std::int64_t value) { assert(value >= now_us); now_us = value; }
}  // namespace sdk
using namespace sdk;

extern const unsigned char wifi_setup_html_start[] asm("_binary_wifi_setup_html_start") = "mock portal";
extern const unsigned char wifi_setup_html_end[] asm("_binary_wifi_setup_html_end") = "";
const char* esp_err_to_name(esp_err_t) { return "mock error"; }
std::int64_t esp_timer_get_time() { return now_us; }
esp_err_t esp_event_loop_create_default() { return ESP_OK; }
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, std::int32_t id, esp_event_handler_t cb,
                                             void* context, esp_event_handler_instance_t* instance)
{
    handlers.push_back({base, id, cb, context, true});
    *instance = reinterpret_cast<void*>(handlers.size());
    return ESP_OK;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, std::int32_t id,
                                               esp_event_handler_instance_t instance)
{
    auto& handler = handlers.at(reinterpret_cast<std::size_t>(instance) - 1);
    assert(handler.base == base && handler.id == id);
    handler.active = false;
    return ESP_OK;
}
esp_err_t esp_netif_init() { return ESP_OK; }
esp_netif_t* esp_netif_create_default_wifi_sta() { return &station; }
esp_netif_t* esp_netif_create_default_wifi_ap() { return &access_point; }
esp_netif_t* esp_netif_get_handle_from_ifkey(const char* key)
{
    assert(std::strcmp(key, "WIFI_STA_DEF") == 0);
    return &station;
}
bool esp_netif_is_netif_up(esp_netif_t* netif) { return netif->up; }
esp_err_t esp_netif_get_ip_info(esp_netif_t* netif, esp_netif_ip_info_t* output)
{
    output->ip.addr = netif->ip;
    return ESP_OK;
}
esp_err_t esp_netif_set_ip_info(esp_netif_t* netif, const esp_netif_ip_info_t* value)
{
    netif->ip = value->ip.addr;
    return ESP_OK;
}
esp_err_t esp_netif_set_dns_info(esp_netif_t*, int, const esp_netif_dns_info_t*) { return ESP_OK; }
esp_err_t esp_netif_dhcps_start(esp_netif_t*) { return ESP_OK; }
esp_err_t esp_netif_dhcps_stop(esp_netif_t*) { return ESP_OK; }
esp_err_t esp_netif_dhcps_option(esp_netif_t*, int, int, void*, std::size_t) { return ESP_OK; }
esp_err_t esp_netif_get_netif_impl_name(esp_netif_t*, char* name) { std::strcpy(name, "ap"); return ESP_OK; }
esp_err_t esp_netif_dhcps_get_clients_by_mac(esp_netif_t*, int, esp_netif_pair_mac_ip_t*) { return ESP_FAIL; }
esp_err_t esp_netif_tcpip_exec(esp_netif_callback_fn function, void* context)
{
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
    if (defer_dns) {
        for (const auto& pending : pending_dns) assert(pending.context != context);
        pending_dns.push_back({hostname, cb, context});
        assert(pending_dns.size() <= 3);
        return ERR_INPROGRESS;
    }
    address->addr = 0xcb007101;
    return ERR_OK;
}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t* config)
{
    assert(!sntp_running && tcpip_context && driver_running && associated);
    assert(!config->smooth_sync && !config->server_from_dhcp && !config->wait_for_sync && config->start);
    assert(config->num_of_servers == 1 && config->sync_cb);
    assert(std::strcmp(config->servers[0], "203.0.113.1") == 0);
    ++sntp_starts;
    operations.push_back("sntp_start");
    sntp_running = true;
    retained_address = config->servers[0];
    sync_callback = last_sync_callback = config->sync_cb;
    return ESP_OK;
}
void esp_netif_sntp_deinit()
{
    assert(sntp_running && tcpip_context);
    ++sntp_stops;
    operations.push_back("sntp_stop");
    sntp_running = false;
    sync_callback = nullptr;
    retained_address = nullptr;
}
esp_err_t esp_wifi_init(const wifi_init_config_t*) { return ESP_OK; }
esp_err_t esp_wifi_set_storage(int) { return ESP_OK; }
esp_err_t esp_wifi_set_mode(wifi_mode_t value)
{
    if (value == WIFI_MODE_APSTA) { ++ap_enables; operations.push_back("ap_enable"); }
    mode = value;
    return ESP_OK;
}
esp_err_t esp_wifi_get_mode(wifi_mode_t* value) { *value = mode; return ESP_OK; }
esp_err_t esp_wifi_set_config(int interface, const wifi_config_t* config)
{
    if (interface == WIFI_IF_STA) station_config = *config;
    else { assert(interface == WIFI_IF_AP); ap_config = *config; }
    return ESP_OK;
}
esp_err_t esp_wifi_get_config(int interface, wifi_config_t* config)
{
    assert(interface == WIFI_IF_STA);
    *config = station_config;
    return ESP_OK;
}
esp_err_t esp_wifi_start()
{
    assert(!driver_running && !stop_pending);
    ++starts;
    operations.push_back("wifi_start");
    if (fail_starts) { --fail_starts; return ESP_FAIL; }
    driver_running = true;
    emit(WIFI_EVENT, WIFI_EVENT_STA_START);
    return ESP_OK;
}
esp_err_t esp_wifi_stop()
{
    ++stops;
    operations.push_back("wifi_stop");
    if (fail_stops) { --fail_stops; return ESP_FAIL; }
    assert(driver_running);
    driver_running = false;
    associated = false;
    station.up = false;
    access_point.up = false;
    stop_pending = true;
    return ESP_OK;
}
esp_err_t esp_wifi_connect()
{
    assert(driver_running && !stop_pending);
    ++connects;
    operations.push_back("wifi_connect");
    return ESP_OK;
}
esp_err_t esp_wifi_disconnect()
{
    associated = false;
    station.up = false;
    wifi_event_sta_disconnected_t event;
    emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event);
    return ESP_OK;
}
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t*, bool)
{
    assert(driver_running && !stop_pending);
    ++scans;
    operations.push_back("wifi_scan");
    return ESP_OK;
}
esp_err_t esp_wifi_scan_stop() { return ESP_OK; }
esp_err_t esp_wifi_clear_ap_list() { return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_num(std::uint16_t* count) { *count = 0; return ESP_OK; }
esp_err_t esp_wifi_scan_get_ap_records(std::uint16_t*, wifi_ap_record_t*) { return ESP_OK; }
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t* result)
{
    if (!driver_running || !associated) return ESP_FAIL;
    std::memcpy(result->ssid, station_config.sta.ssid, 32);
    std::memcpy(result->bssid, BSSID, sizeof(BSSID));
    return ESP_OK;
}
esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t*) { return ESP_FAIL; }
esp_err_t esp_read_mac(unsigned char* mac, int)
{
    std::memcpy(mac, BSSID, sizeof(BSSID));
    return ESP_OK;
}
void esp_fill_random(void* data, std::size_t length) { std::memset(data, 0xAB, length); }
std::uint32_t esp_random() { return 0; }

QueueHandle_t xQueueCreate(unsigned capacity, unsigned item_size)
{
    return new Queue{capacity, item_size, {}, {}, {}};
}
BaseType_t xQueueSend(QueueHandle_t handle, const void* item, TickType_t)
{
    auto& queue = *static_cast<Queue*>(handle);
    std::lock_guard<std::mutex> lock(queue.mutex);
    if (queue.items.size() >= queue.capacity) return pdFALSE;
    const auto* data = static_cast<const unsigned char*>(item);
    queue.items.emplace_back(data, data + queue.item_size);
    queue.available.notify_one();
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t handle, void* item, TickType_t timeout)
{
    auto& queue = *static_cast<Queue*>(handle);
    std::unique_lock<std::mutex> lock(queue.mutex);
    if (timeout == portMAX_DELAY) queue.available.wait(lock, [&] { return !queue.items.empty(); });
    if (queue.items.empty()) return pdFALSE;
    std::memcpy(item, queue.items.front().data(), queue.item_size);
    queue.items.pop_front();
    return pdTRUE;
}
BaseType_t xQueueReset(QueueHandle_t handle)
{
    auto& queue = *static_cast<Queue*>(handle);
    std::lock_guard<std::mutex> lock(queue.mutex);
    queue.items.clear();
    return pdTRUE;
}
SemaphoreHandle_t xSemaphoreCreateMutex() { return new std::mutex; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t) { static_cast<std::mutex*>(mutex)->lock(); return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) { static_cast<std::mutex*>(mutex)->unlock(); return pdTRUE; }
BaseType_t xTaskCreate(void (*callback)(void*), const char*, unsigned, void* context, unsigned, TaskHandle_t* task)
{
    auto* thread = new std::thread([callback, context] { callback(context); });
    *task = thread;
    thread->detach();
    return pdPASS;
}
void vTaskDelay(TickType_t) { std::this_thread::yield(); }

esp_err_t nvs_flash_init() { return ESP_OK; }
esp_err_t nvs_open(const char*, int, nvs_handle_t* handle) { *handle = 1; return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void* data, std::size_t* length)
{
    if (!saved_credentials) return ESP_FAIL;
    struct Stored {
        std::uint32_t magic = 0x57494631;
        std::uint8_t version = 1;
        std::uint8_t ssid_length = 4;
        std::uint8_t password_length = 8;
        std::uint8_t reserved = 0;
        char ssid[32] = "test";
        char password[64] = "password";
    } record;
    static_assert(sizeof(record) == 104, "NVS fixture layout");
    assert(*length == sizeof(record));
    std::memcpy(data, &record, sizeof(record));
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, std::size_t) { return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
void nvs_close(nvs_handle_t) {}

bool CaptiveDns::start() { ++dns_starts; fd_ = 1; return true; }
void CaptiveDns::stop() { if (fd_ != -1) { ++dns_stops; fd_ = -1; } }
void CaptiveDns::tick() {}
esp_err_t httpd_start(httpd_handle_t* handle, const httpd_config_t* config)
{
    ++http_starts;
    http_context = config->global_user_ctx;
    *handle = http_context;
    return ESP_OK;
}
esp_err_t httpd_stop(httpd_handle_t)
{
    while (block_http_stop.load()) std::this_thread::yield();
    ++http_stops;
    return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t*) { return ESP_OK; }
void* httpd_get_global_user_ctx(httpd_handle_t) { return http_context; }
bool httpd_uri_match_wildcard(const char*, const char*, std::size_t) { return true; }
int httpd_req_to_sockfd(httpd_req_t*) { return -1; }
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*, const char*, char*, std::size_t) { return ESP_FAIL; }
std::size_t httpd_req_get_hdr_value_len(httpd_req_t*, const char*) { return 0; }
int httpd_req_recv(httpd_req_t*, char*, std::size_t) { return -1; }
esp_err_t httpd_resp_set_status(httpd_req_t*, const char*) { return ESP_OK; }
esp_err_t httpd_resp_set_type(httpd_req_t*, const char*) { return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char*, const char*) { return ESP_OK; }
esp_err_t httpd_resp_send(httpd_req_t*, const char*, int) { return ESP_OK; }
