#include "captive_dns.h"

#include <cerrno>
#include <cstring>

#ifdef ESP_PLATFORM
#include "esp_netif.h"
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

constexpr std::size_t kHeaderLength = 12;
constexpr std::size_t kMaxPacketLength = 512;
constexpr std::uint32_t kApAddress = 0xc0a80401U;  // 192.168.4.1, host order
constexpr unsigned kPacketsPerTick = 4;

std::uint16_t read16(const std::uint8_t* p)
{
    return static_cast<std::uint16_t>((static_cast<unsigned>(p[0]) << 8) | p[1]);
}

void write16(std::uint8_t* p, std::uint16_t value)
{
    p[0] = static_cast<std::uint8_t>(value >> 8);
    p[1] = static_cast<std::uint8_t>(value);
}

bool readQuestionName(const std::uint8_t* request, std::size_t length,
                      std::size_t& offset)
{
    const std::size_t start = offset;
    while (offset < length) {
        const std::uint8_t labelLength = request[offset++];
        // Reject compression pointers and reserved label encodings. A single
        // question never needs compression, and following pointers invites
        // cycles and out-of-packet reads.
        if ((labelLength & 0xc0U) != 0 || labelLength > length - offset) {
            return false;
        }
        if (labelLength == 0) {
            return offset - start <= 255;
        }
        offset += labelLength;
        if (offset - start >= 255) {
            return false;
        }
    }
    return false;
}

bool readOptionalOpt(const std::uint8_t* request, std::size_t length,
                     std::size_t offset)
{
    // An OPT owner must be the root. Ten bytes follow its one-byte name.
    if (length - offset < 11 || request[offset++] != 0) {
        return false;
    }
    if (read16(request + offset) != 41 || request[offset + 4] != 0 ||
        request[offset + 5] != 0) {  // OPT, no extended RCODE, EDNS version 0
        return false;
    }
    const std::size_t optionsLength = read16(request + offset + 8);
    offset += 10;
    if (optionsLength != length - offset) {
        return false;
    }
    // Check each EDNS option's length even though the captive response does
    // not echo OPT or use any of its options.
    while (offset < length) {
        if (length - offset < 4) {
            return false;
        }
        const std::size_t optionLength = read16(request + offset + 2);
        offset += 4;
        if (optionLength > length - offset) {
            return false;
        }
        offset += optionLength;
    }
    return true;
}

}  // namespace

std::size_t captive_dns::buildDnsResponse(const std::uint8_t* request,
                                         std::size_t requestLength,
                                         std::uint8_t* response,
                                         std::size_t responseCapacity)
{
    if (request == nullptr || response == nullptr ||
        requestLength < kHeaderLength || requestLength > kMaxPacketLength) {
        return 0;
    }
    const std::uint16_t flags = read16(request + 2);
    // Only ordinary queries with RD, AD and/or CD flags are supported.
    // In particular, never respond to a response, a truncated query, a
    // nonzero opcode/RCODE, or a reserved header flag.
    if ((flags & static_cast<std::uint16_t>(~0x0130U)) != 0 ||
        read16(request + 4) != 1 || read16(request + 6) != 0 ||
        read16(request + 8) != 0 || read16(request + 10) > 1) {
        return 0;
    }

    std::size_t offset = kHeaderLength;
    if (!readQuestionName(request, requestLength, offset) ||
        requestLength - offset < 4) {
        return 0;
    }
    const std::uint16_t type = read16(request + offset);
    const std::uint16_t queryClass = read16(request + offset + 2);
    const std::size_t questionEnd = offset + 4;
    const bool hasOpt = read16(request + 10) == 1;
    if (hasOpt ? !readOptionalOpt(request, requestLength, questionEnd)
               : questionEnd != requestLength) {
        return 0;
    }

    const bool answerA = type == 1 && queryClass == 1;  // A, Internet class
    const std::size_t responseLength = questionEnd + (answerA ? 16 : 0);
    if (responseCapacity < responseLength) {
        return 0;
    }

    std::memmove(response, request, questionEnd);
    // QR + AA, echo RD/CD. We do not recurse, validate DNSSEC, or set AD.
    write16(response + 2, static_cast<std::uint16_t>(0x8400U | (flags & 0x0110U)));
    write16(response + 4, 1);
    write16(response + 6, answerA ? 1 : 0);
    write16(response + 8, 0);
    write16(response + 10, 0);

    if (answerA) {
        // Name points to the already validated question at offset 12.
        // TTL is zero so the captive answer will not outlive provisioning.
        constexpr std::uint8_t answer[] = {
            0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x04,
            192, 168, 4, 1
        };
        std::memcpy(response + questionEnd, answer, sizeof(answer));
    }
    return responseLength;
}

bool CaptiveDns::start()
{
    if (fd_ >= 0) {
        return true;
    }
    const int candidate = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (candidate < 0) {
        return false;
    }
    const int flags = ::fcntl(candidate, F_GETFL, 0);
    if (flags < 0 || ::fcntl(candidate, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(candidate);
        return false;
    }

#ifdef ESP_PLATFORM
    // Local-address binding alone is not sufficient if the station's network
    // overlaps the provisioning subnet. Bind the UDP PCB to the AP netif too.
    esp_netif_t* ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    struct ifreq interfaceRequest = {};
    if (ap == nullptr ||
        esp_netif_get_netif_impl_name(ap, interfaceRequest.ifr_name) != ESP_OK ||
        ::setsockopt(candidate, SOL_SOCKET, SO_BINDTODEVICE,
                     &interfaceRequest, sizeof(interfaceRequest)) != 0) {
        ::close(candidate);
        return false;
    }
#endif

    struct sockaddr_in local = {};
    local.sin_family = AF_INET;
    local.sin_port = htons(53);
    local.sin_addr.s_addr = htonl(kApAddress);
    if (::bind(candidate, reinterpret_cast<const struct sockaddr*>(&local),
               sizeof(local)) != 0) {
        ::close(candidate);
        return false;
    }
    fd_ = candidate;
    return true;
}

void CaptiveDns::tick()
{
    if (fd_ < 0) {
        return;
    }
    // The extra byte lets recvfrom detect every oversized datagram even when
    // the network stack truncates it to the receive buffer's size.
    std::uint8_t request[kMaxPacketLength + 1];
    std::uint8_t response[kMaxPacketLength];
    for (unsigned packet = 0; packet < kPacketsPerTick; ++packet) {
        struct sockaddr_in sender = {};
        socklen_t senderLength = sizeof(sender);
        const ssize_t received = ::recvfrom(fd_, request, sizeof(request), MSG_DONTWAIT,
            reinterpret_cast<struct sockaddr*>(&sender), &senderLength);
        if (received < 0) {
            // No queued packet, or a socket error. Never retry in a tight
            // loop; the owner may stop/restart the socket with its AP.
            return;
        }
        if (received == 0 || static_cast<std::size_t>(received) > kMaxPacketLength ||
            senderLength != sizeof(sender) || sender.sin_family != AF_INET ||
            sender.sin_port == 0) {
            continue;
        }
        const std::uint32_t sourceAddress = ntohl(sender.sin_addr.s_addr);
        const std::uint32_t host = sourceAddress & 0xffU;
        if ((sourceAddress & 0xffffff00U) != (kApAddress & 0xffffff00U) ||
            host == 0 || host == 1 || host == 255) {
            continue;
        }
        const std::size_t responseLength = captive_dns::buildDnsResponse(
            request, static_cast<std::size_t>(received), response, sizeof(response));
        if (responseLength != 0) {
            // Dropping on send-buffer pressure is deliberate; clients retry
            // DNS and rendering/event processing must never wait for them.
            (void)::sendto(fd_, response, responseLength, MSG_DONTWAIT,
                reinterpret_cast<const struct sockaddr*>(&sender), senderLength);
        }
    }
}

void CaptiveDns::stop()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}
