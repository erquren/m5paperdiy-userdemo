/* SPDX-License-Identifier: MIT */
#pragma once
#include <cstdint>
#include <cstring>
#ifdef ESP_PLATFORM
#include <lwip/sockets.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif
namespace wifi_socket {
// IDF HTTPD uses a dual-stack IPv6 listener when CONFIG_LWIP_IPV6 is enabled.
// Normalize only IPv4 and IPv4-mapped IPv6; native IPv6 is never a setup route.
inline bool ipv4Address(const sockaddr* address, socklen_t length, std::uint32_t& result)
{
    result = 0;
    if (!address || length < sizeof(sockaddr)) return false;
    if (address->sa_family == AF_INET && length >= sizeof(sockaddr_in)) {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address);
        result = ipv4->sin_addr.s_addr;
        return true;
    }
    if (address->sa_family == AF_INET6 && length >= sizeof(sockaddr_in6)) {
        const auto* ipv6 = reinterpret_cast<const sockaddr_in6*>(address);
        const auto* bytes = ipv6->sin6_addr.s6_addr;
        for (unsigned i = 0; i < 10; ++i) if (bytes[i] != 0) return false;
        if (bytes[10] != 0xff || bytes[11] != 0xff) return false;
        std::memcpy(&result, bytes + 12, sizeof(result));
        return true;
    }
    return false;
}
}  // namespace wifi_socket
