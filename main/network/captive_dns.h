#pragma once

#include <cstddef>
#include <cstdint>

namespace captive_dns {

// Builds a small captive-portal DNS response without sockets or allocation.
// Returns its byte length, or zero for unsupported/malformed requests or a
// short output buffer. Input and output may overlap. Only one uncompressed
// question and an optional well-formed EDNS(0) OPT record are accepted.
std::size_t buildDnsResponse(const std::uint8_t* request,
                             std::size_t requestLength,
                             std::uint8_t* response,
                             std::size_t responseCapacity);

}  // namespace captive_dns

// Call start(), tick(), and stop() from the same owning task. The ESP-IDF
// implementation requires the default AP netif, configured as 192.168.4.1/24.
class CaptiveDns {
public:
    CaptiveDns() = default;
    ~CaptiveDns() { stop(); }
    CaptiveDns(const CaptiveDns&) = delete;
    CaptiveDns& operator=(const CaptiveDns&) = delete;

    bool start();
    void tick();
    void stop();

private:
    int fd_ = -1;
};
