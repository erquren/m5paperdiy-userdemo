/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstdint>
#include <mutex>
#include <sys/time.h>

using portMUX_TYPE = std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(mux) ((mux)->lock())
#define portEXIT_CRITICAL(mux) ((mux)->unlock())
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_INVALID_STATE = 0x103;
const char* esp_err_to_name(esp_err_t);
#define ESP_LOGI(tag, format, ...) ((void)(tag))
#define ESP_LOGW(tag, format, ...) ((void)(tag))

using esp_event_base_t = const char*;
inline constexpr char WIFI_EVENT[] = "wifi";
inline constexpr char IP_EVENT[] = "ip";
constexpr int ESP_EVENT_ANY_ID = -1;
constexpr int WIFI_EVENT_STA_DISCONNECTED = 1;
constexpr int WIFI_EVENT_STA_STOP = 2;
constexpr int WIFI_EVENT_STA_CONNECTED = 3;
constexpr int IP_EVENT_STA_GOT_IP = 10;
constexpr int IP_EVENT_STA_LOST_IP = 11;
using esp_event_handler_t = void (*)(void*, esp_event_base_t, std::int32_t, void*);
using esp_event_handler_instance_t = void*;
esp_err_t esp_event_handler_instance_register(esp_event_base_t, std::int32_t, esp_event_handler_t, void*, esp_event_handler_instance_t*);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t, std::int32_t, esp_event_handler_instance_t);

struct esp_netif_t { bool up = false; std::uint32_t ip = 0; };
struct wifi_ap_record_t;
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t*);
struct esp_ip4_addr_t { std::uint32_t addr = 0; };
struct esp_netif_ip_info_t { esp_ip4_addr_t ip, netmask, gw; };
struct ip_event_got_ip_t { esp_netif_t* esp_netif; esp_netif_ip_info_t ip_info; bool ip_changed; };
esp_netif_t* esp_netif_get_handle_from_ifkey(const char*);
bool esp_netif_is_netif_up(esp_netif_t*);
esp_err_t esp_netif_get_ip_info(esp_netif_t*, esp_netif_ip_info_t*);
using esp_netif_callback_fn = esp_err_t (*)(void*);
esp_err_t esp_netif_tcpip_exec(esp_netif_callback_fn, void*);
struct ip_addr_t { std::uint32_t addr = 0; };
#define IP_IS_V4(address) ((address) != nullptr)
inline bool ip_addr_isany(const ip_addr_t* address) { return address->addr == 0; }
char* ipaddr_ntoa_r(const ip_addr_t*, char*, int);
using err_t = int;
constexpr err_t ERR_OK = 0;
constexpr err_t ERR_ARG = -16;
constexpr err_t ERR_INPROGRESS = -5;
constexpr unsigned char LWIP_DNS_ADDRTYPE_IPV4 = 0;
using dns_found_callback = void (*)(const char*, const ip_addr_t*, void*);
err_t dns_gethostbyname_addrtype(const char*, ip_addr_t*, dns_found_callback, void*, unsigned char);
std::int64_t esp_timer_get_time();
using esp_sntp_time_cb_t = void (*)(struct timeval*);
struct esp_sntp_config_t {
    bool smooth_sync = false;
    bool server_from_dhcp = false;
    bool wait_for_sync = true;
    bool start = true;
    esp_sntp_time_cb_t sync_cb = nullptr;
    bool renew_servers_after_new_IP = false;
    int ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    std::size_t index_of_first_server = 0;
    std::size_t num_of_servers = 1;
    const char* servers[1] = {};
};
inline esp_sntp_config_t defaultSntp(const char* server)
{
    esp_sntp_config_t result;
    result.servers[0] = server;
    return result;
}
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server) defaultSntp(server)
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t*);
void esp_netif_sntp_deinit();

#define ESP_LOGE(tag, format, ...) ((void)(tag))
constexpr esp_err_t ESP_ERR_WIFI_NOT_STARTED = 0x3002;
constexpr esp_err_t ESP_ERR_WIFI_NOT_INIT = 0x3001;
constexpr esp_err_t ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED = 0x5000;
constexpr int WIFI_EVENT_STA_START = 4;
constexpr int WIFI_EVENT_AP_START = 5;
constexpr int WIFI_EVENT_AP_STOP = 6;
constexpr int WIFI_EVENT_SCAN_DONE = 7;
struct wifi_event_sta_connected_t { unsigned char ssid[32] = {}; unsigned char ssid_len = 0; unsigned char bssid[6] = {}; };
struct wifi_event_sta_disconnected_t { unsigned char reason = 0; };
struct wifi_event_sta_scan_done_t { std::uint32_t status = 0; };
constexpr unsigned WIFI_REASON_AUTH_FAIL = 202;
constexpr unsigned WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT = 15;
constexpr unsigned WIFI_REASON_HANDSHAKE_TIMEOUT = 204;
esp_err_t esp_event_loop_create_default();

enum wifi_mode_t { WIFI_MODE_NULL, WIFI_MODE_STA, WIFI_MODE_AP, WIFI_MODE_APSTA };
constexpr int WIFI_IF_STA = 0;
constexpr int WIFI_IF_AP = 1;
constexpr int WIFI_STORAGE_RAM = 1;
constexpr int WIFI_ALL_CHANNEL_SCAN = 1;
constexpr int WIFI_CONNECT_AP_BY_SIGNAL = 1;
constexpr int WIFI_AUTH_OPEN = 0;
constexpr int WIFI_AUTH_WPA2_PSK = 3;
constexpr int WIFI_SCAN_TYPE_ACTIVE = 1;
struct wifi_pmf_config_t { bool capable = false; bool required = false; };
struct wifi_sta_config_t {
    unsigned char ssid[32] = {};
    unsigned char password[64] = {};
    int scan_method = 0;
    int sort_method = 0;
    struct { int authmode = 0; } threshold;
    wifi_pmf_config_t pmf_cfg;
    unsigned failure_retry_cnt = 0;
};
struct wifi_ap_config_t {
    unsigned char ssid[32] = {};
    unsigned char password[64] = {};
    unsigned ssid_len = 0;
    int authmode = 0;
    unsigned max_connection = 0;
    unsigned channel = 0;
    wifi_pmf_config_t pmf_cfg;
};
struct wifi_config_t { wifi_sta_config_t sta; wifi_ap_config_t ap; };
struct wifi_ap_record_t {
    unsigned char ssid[33] = {};
    unsigned char bssid[6] = {};
    int rssi = 0;
    int authmode = 0;
};
struct wifi_scan_config_t {
    bool show_hidden = false;
    int scan_type = 0;
    struct { struct { unsigned max = 0; } active; } scan_time;
};
struct wifi_sta_list_t { unsigned num = 0; struct { unsigned char mac[6]; } sta[1]; };
struct wifi_init_config_t {};
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
esp_err_t esp_wifi_init(const wifi_init_config_t*);
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(wifi_mode_t);
esp_err_t esp_wifi_get_mode(wifi_mode_t*);
esp_err_t esp_wifi_set_config(int, const wifi_config_t*);
esp_err_t esp_wifi_get_config(int, wifi_config_t*);
esp_err_t esp_wifi_start();
esp_err_t esp_wifi_stop();
esp_err_t esp_wifi_connect();
esp_err_t esp_wifi_disconnect();
esp_err_t esp_wifi_scan_start(const wifi_scan_config_t*, bool);
esp_err_t esp_wifi_scan_stop();
esp_err_t esp_wifi_clear_ap_list();
esp_err_t esp_wifi_scan_get_ap_num(std::uint16_t*);
esp_err_t esp_wifi_scan_get_ap_records(std::uint16_t*, wifi_ap_record_t*);
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t*);
esp_err_t esp_wifi_ap_get_sta_list(wifi_sta_list_t*);

constexpr int ESP_MAC_WIFI_SOFTAP = 1;
esp_err_t esp_read_mac(unsigned char*, int);
void esp_fill_random(void*, std::size_t);
std::uint32_t esp_random();
esp_err_t esp_netif_init();
esp_netif_t* esp_netif_create_default_wifi_sta();
esp_netif_t* esp_netif_create_default_wifi_ap();
constexpr int ESP_IPADDR_TYPE_V4 = 0;
constexpr int ESP_NETIF_DNS_MAIN = 0;
constexpr int ESP_NETIF_OP_SET = 0;
constexpr int ESP_NETIF_DOMAIN_NAME_SERVER = 0;
struct esp_netif_dns_info_t { struct { int type = 0; struct { esp_ip4_addr_t ip4; } u_addr; } ip; };
struct esp_netif_pair_mac_ip_t { unsigned char mac[6] = {}; esp_ip4_addr_t ip; };
esp_err_t esp_netif_set_ip_info(esp_netif_t*, const esp_netif_ip_info_t*);
esp_err_t esp_netif_set_dns_info(esp_netif_t*, int, const esp_netif_dns_info_t*);
esp_err_t esp_netif_dhcps_start(esp_netif_t*);
esp_err_t esp_netif_dhcps_stop(esp_netif_t*);
esp_err_t esp_netif_dhcps_option(esp_netif_t*, int, int, void*, std::size_t);
esp_err_t esp_netif_get_netif_impl_name(esp_netif_t*, char*);
esp_err_t esp_netif_dhcps_get_clients_by_mac(esp_netif_t*, int, esp_netif_pair_mac_ip_t*);

using BaseType_t = int;
using UBaseType_t = unsigned;
using TickType_t = unsigned;
using QueueHandle_t = void*;
using SemaphoreHandle_t = void*;
using TaskHandle_t = void*;
constexpr BaseType_t pdTRUE = 1;
constexpr BaseType_t pdFALSE = 0;
constexpr BaseType_t pdPASS = 1;
constexpr TickType_t portMAX_DELAY = ~0u;
#define pdMS_TO_TICKS(ms) (ms)
QueueHandle_t xQueueCreate(unsigned, unsigned);
BaseType_t xQueueSend(QueueHandle_t, const void*, TickType_t);
BaseType_t xQueueReceive(QueueHandle_t, void*, TickType_t);
BaseType_t xQueueReset(QueueHandle_t);
SemaphoreHandle_t xSemaphoreCreateMutex();
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
BaseType_t xTaskCreate(void (*)(void*), const char*, unsigned, void*, unsigned, TaskHandle_t*);
void vTaskDelay(TickType_t);

using nvs_handle_t = unsigned;
constexpr int NVS_READONLY = 0;
constexpr int NVS_READWRITE = 1;
esp_err_t nvs_flash_init();
esp_err_t nvs_open(const char*, int, nvs_handle_t*);
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, std::size_t*);
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, std::size_t);
esp_err_t nvs_commit(nvs_handle_t);
void nvs_close(nvs_handle_t);

using httpd_handle_t = void*;
struct httpd_req_t { void* user_ctx; const char* uri; std::size_t content_len; };
using httpd_uri_func = esp_err_t (*)(httpd_req_t*);
constexpr int HTTP_GET = 0;
constexpr int HTTP_POST = 1;
constexpr int HTTPD_RESP_USE_STRLEN = -1;
struct httpd_uri_t { const char* uri; int method; httpd_uri_func handler; void* user_ctx; };
struct httpd_config_t {
    unsigned stack_size = 0;
    unsigned max_uri_handlers = 0;
    unsigned max_open_sockets = 0;
    bool lru_purge_enable = false;
    unsigned recv_wait_timeout = 0;
    unsigned send_wait_timeout = 0;
    bool (*uri_match_fn)(const char*, const char*, std::size_t) = nullptr;
    void* global_user_ctx = nullptr;
    void (*global_user_ctx_free_fn)(void*) = nullptr;
    esp_err_t (*open_fn)(httpd_handle_t, int) = nullptr;
};
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}
esp_err_t httpd_start(httpd_handle_t*, const httpd_config_t*);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t*);
void* httpd_get_global_user_ctx(httpd_handle_t);
bool httpd_uri_match_wildcard(const char*, const char*, std::size_t);
int httpd_req_to_sockfd(httpd_req_t*);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*, const char*, char*, std::size_t);
std::size_t httpd_req_get_hdr_value_len(httpd_req_t*, const char*);
int httpd_req_recv(httpd_req_t*, char*, std::size_t);
esp_err_t httpd_resp_set_status(httpd_req_t*, const char*);
esp_err_t httpd_resp_set_type(httpd_req_t*, const char*);
esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char*, const char*);
esp_err_t httpd_resp_send(httpd_req_t*, const char*, int);
