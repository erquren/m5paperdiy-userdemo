// Host-only test; intentionally absent from the ESP-IDF component sources.
// Run from the repository root:
// g++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined
//   main/network/captive_dns.cpp main/network/tests/captive_dns_test.cpp
//   -o /tmp/captive_dns_test && /tmp/captive_dns_test (join the three lines)

#include "../captive_dns.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

using Packet = std::vector<std::uint8_t>;

Packet query(std::uint16_t type = 1)
{
    return {0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0,
            7, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0,
            static_cast<std::uint8_t>(type >> 8), static_cast<std::uint8_t>(type),
            0, 1};
}

std::size_t answer(const Packet& packet, std::array<std::uint8_t, 512>& out)
{
    return captive_dns::buildDnsResponse(packet.data(), packet.size(), out.data(), out.size());
}

void reject(const Packet& packet)
{
    std::array<std::uint8_t, 512> out;
    out.fill(0xa5);
    assert(answer(packet, out) == 0);
    for (const auto value : out) {
        assert(value == 0xa5);  // Rejected requests must not partially write.
    }
}

}  // namespace

int main()
{
    const Packet a = query();
    std::array<std::uint8_t, 512> out = {};
    assert(answer(a, out) == a.size() + 16);
    assert(out[0] == 0x12 && out[1] == 0x34);
    assert(out[2] == 0x85 && out[3] == 0x00);  // QR, AA, RD; no RA/AD
    assert(out[4] == 0 && out[5] == 1 && out[6] == 0 && out[7] == 1);
    assert(out[8] == 0 && out[9] == 0 && out[10] == 0 && out[11] == 0);
    assert(std::memcmp(out.data() + 12, a.data() + 12, a.size() - 12) == 0);
    const std::array<std::uint8_t, 16> expected = {
        0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4, 192, 168, 4, 1};
    assert(std::memcmp(out.data() + a.size(), expected.data(), expected.size()) == 0);

    // AAAA, HTTPS, TXT and ANY produce a valid empty NOERROR response.
    for (const auto type : {28, 65, 16, 255}) {
        Packet other = query(static_cast<std::uint16_t>(type));
        assert(answer(other, out) == other.size());
        assert(out[6] == 0 && out[7] == 0 && (out[3] & 0x0f) == 0);
    }
    Packet nonInternet = a;
    nonInternet.back() = 3;
    assert(answer(nonInternet, out) == nonInternet.size() && out[7] == 0);

    // Valid EDNS(0), with and without options, is accepted and not echoed.
    Packet edns = a;
    edns[11] = 1;
    edns.insert(edns.end(), {0, 0, 41, 0x10, 0, 0, 0, 0x80, 0, 0, 0});
    assert(answer(edns, out) == a.size() + 16 && out[11] == 0);
    Packet option = edns;
    option.back() = 6;
    option.insert(option.end(), {0, 10, 0, 2, 0x12, 0x34});
    assert(answer(option, out) == a.size() + 16);
    option.back() = 0x56;
    option[option.size() - 3] = 3;  // Option length exceeds remaining bytes.
    reject(option);
    Packet invalidEdns = edns;
    invalidEdns[a.size() + 6] = 1;  // Unsupported EDNS version.
    reject(invalidEdns);
    invalidEdns = edns;
    invalidEdns[a.size() + 5] = 1;  // Extended response code in a query.
    reject(invalidEdns);
    invalidEdns = edns;
    invalidEdns[a.size() + 2] = 1;  // Additional record is not OPT.
    reject(invalidEdns);

    // Every truncation of a normal question and the OPT record is rejected.
    for (std::size_t length = 0; length < a.size(); ++length) {
        reject(Packet(a.begin(), a.begin() + length));
    }
    for (std::size_t length = a.size(); length < edns.size(); ++length) {
        reject(Packet(edns.begin(), edns.begin() + length));
    }
    for (const auto offset : {4, 6, 8, 10}) {
        Packet count = a;
        count[offset] = 1;
        reject(count);
    }
    Packet count = a;
    count[5] = 0;
    reject(count);
    count[5] = 2;
    reject(count);
    count = a;
    count[11] = 2;
    reject(count);
    for (const auto high : {0x81, 0x09, 0x05, 0x03}) {
        Packet flags = a;
        flags[2] = high;  // Response, opcode, AA, TC respectively.
        reject(flags);
    }
    for (const auto low : {0x80, 0x40, 0x01}) {
        Packet flags = a;
        flags[3] = low;  // RA, reserved bit, RCODE respectively.
        reject(flags);
    }
    Packet flags = a;
    flags[3] = 0x30;  // Query AD/CD accepted; response clears AD.
    assert(answer(flags, out) == a.size() + 16 && out[3] == 0x10);
    for (const auto label : {0x40, 0x80, 0xc0, 0xff, 63}) {
        Packet malformed = a;
        malformed[12] = label;
        reject(malformed);
    }
    Packet trailing = a;
    trailing.push_back(0);
    reject(trailing);
    reject(Packet(513, 0));

    // Maximum wire-format name length is 255 bytes including the root.
    Packet longest(a.begin(), a.begin() + 12);
    for (const unsigned labelLength : {63U, 63U, 63U, 61U}) {
        longest.push_back(static_cast<std::uint8_t>(labelLength));
        longest.insert(longest.end(), labelLength, 'a');
    }
    longest.insert(longest.end(), {0, 0, 1, 0, 1});
    assert(answer(longest, out) == longest.size() + 16);
    longest[204] = 62;
    longest.insert(longest.end() - 5, 'a');
    reject(longest);

    // Short output buffers and null arguments fail without modifying memory.
    out.fill(0xa5);
    assert(captive_dns::buildDnsResponse(a.data(), a.size(), out.data(), a.size() + 15) == 0);
    for (const auto value : out) {
        assert(value == 0xa5);
    }
    assert(captive_dns::buildDnsResponse(nullptr, a.size(), out.data(), out.size()) == 0);
    assert(captive_dns::buildDnsResponse(a.data(), a.size(), nullptr, 512) == 0);
    std::memcpy(out.data(), a.data(), a.size());
    assert(captive_dns::buildDnsResponse(out.data(), a.size(), out.data(), out.size()) == a.size() + 16);

    // Deterministic malformed-packet stress test; sanitizer catches bounds or
    // undefined-behavior errors, while sentinel tails catch excess writes.
    std::mt19937 random(0x19216841);
    for (unsigned iteration = 0; iteration < 100000; ++iteration) {
        Packet fuzz(random() % 520);
        for (auto& value : fuzz) {
            value = static_cast<std::uint8_t>(random());
        }
        if (iteration % 2 == 0 && fuzz.size() >= 12) {
            std::memcpy(fuzz.data(), a.data(), 12);
        }
        out.fill(0xa5);
        const std::size_t capacity = random() % 513;
        const std::size_t result = captive_dns::buildDnsResponse(
            fuzz.data(), fuzz.size(), out.data(), capacity);
        assert(result <= capacity);
        for (std::size_t index = result; index < out.size(); ++index) {
            assert(out[index] == 0xa5);
        }
    }
    std::puts("Captive DNS host tests passed (including 100,000 malformed packets).");
}
