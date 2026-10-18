#include "../wifi_validation.h"
#include <cassert>
#include <string>
int main()
{
    using namespace wifi_validation;
    assert(credentials("home", 4, "", 0));
    assert(credentials("home", 4, "12345678", 8));
    assert(!credentials("", 0, "12345678", 8));
    assert(!credentials("home", 4, "1234567", 7));
    assert(credentials(std::string(32, 'x').data(), 32, std::string(63, 'x').data(), 63));
    assert(!credentials(std::string(33, 'x').data(), 33, "", 0));
    assert(credentials("home", 4, std::string(64, 'a').data(), 64));
    assert(!credentials("home", 4, std::string(64, 'x').data(), 64));
    assert(credentials(u8"家庭网络", 12, "12345678", 8));
    assert(!credentials("x\0y", 3, "", 0));
    assert(!utf8("\xc0\xaf", 2));
    assert(!utf8("\xed\xa0\x80", 3));
    assert(!utf8("\xf4\x90\x80\x80", 4));
    assert(!utf8("\xe2\x82", 2));
    assert(utf8("\xf0\x9f\x98\x80", 4));
    const std::string invalid = R"({"ssid":"a\u0000b"})";
    assert(!jsonWithoutNul(invalid.data(), invalid.size()));
    const std::string literal = R"({"ssid":"a\\u0000b"})";
    assert(jsonWithoutNul(literal.data(), literal.size()));
    const std::string newline = "{\"ssid\":\"a\nb\"}";
    assert(!jsonWithoutNul(newline.data(), newline.size()));
    const std::string good = R"({"ssid":"家","password":"12\"345678"})";
    assert(jsonWithoutNul(good.data(), good.size()));
    assert(equalToken("0123456789abcdef0123456789abcdef", "0123456789abcdef0123456789abcdef"));
    assert(!equalToken("0123456789abcdef0123456789abcdef", "1123456789abcdef0123456789abcdef"));
    assert(!equalToken("", ""));
}
