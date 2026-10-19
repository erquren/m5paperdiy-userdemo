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

struct esp_netif_t {};
struct wifi_ap_record_t {};
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
