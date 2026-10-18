#include "../wifi_socket.h"
#include <arpa/inet.h>
#include <cassert>
#include <iostream>
int main()
{
    std::uint32_t value;
    sockaddr_in ipv4 = {};
    ipv4.sin_family = AF_INET;
    inet_pton(AF_INET, "192.168.4.1", &ipv4.sin_addr);
    assert(wifi_socket::ipv4Address(reinterpret_cast<sockaddr*>(&ipv4), sizeof(ipv4), value));
    assert(value == ipv4.sin_addr.s_addr);
    sockaddr_in6 ipv6 = {};
    ipv6.sin6_family = AF_INET6;
    inet_pton(AF_INET6, "::ffff:192.168.4.1", &ipv6.sin6_addr);
    assert(wifi_socket::ipv4Address(reinterpret_cast<sockaddr*>(&ipv6), sizeof(ipv6), value));
    assert(value == ipv4.sin_addr.s_addr);
    inet_pton(AF_INET6, "::ffff:192.168.4.2", &ipv6.sin6_addr);
    assert(wifi_socket::ipv4Address(reinterpret_cast<sockaddr*>(&ipv6), sizeof(ipv6), value));
    assert(value != ipv4.sin_addr.s_addr);
    inet_pton(AF_INET6, "fe80::1", &ipv6.sin6_addr);
    assert(!wifi_socket::ipv4Address(reinterpret_cast<sockaddr*>(&ipv6), sizeof(ipv6), value));
    inet_pton(AF_INET6, "::192.168.4.1", &ipv6.sin6_addr);
    assert(!wifi_socket::ipv4Address(reinterpret_cast<sockaddr*>(&ipv6), sizeof(ipv6), value));
    assert(!wifi_socket::ipv4Address(reinterpret_cast<sockaddr*>(&ipv4), 1, value));
    assert(!wifi_socket::ipv4Address(reinterpret_cast<sockaddr*>(&ipv6), sizeof(sockaddr), value));
    assert(!wifi_socket::ipv4Address(nullptr, 0, value));
    std::cout << "Wi-Fi socket address: 11 checks passed\n";
}
