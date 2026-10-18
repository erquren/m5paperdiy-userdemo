#include "../wifi_request.h"
#include <cassert>
#include <string>
#include <iostream>
int main()
{
    unsigned checks = 0;
    const std::string token = "0123456789abcdef0123456789abcdef";
    auto check = [&](const std::string& json, bool expected, bool connect = true) {
        wifi_request::Request result;
        assert(wifi_request::parse(json.data(), json.size(), connect, result) == expected);
        if (expected) assert(std::string(result.token) == token);
        ++checks;
    };
    auto request = [&](const std::string& ssid, const std::string& password) {
        return "{\"ssid\":\"" + ssid + "\",\"password\":\"" + password + "\",\"token\":\"" + token + "\"}";
    };
    check(request("home", "12345678"), true);
    check(request("家庭网络", "12345678"), true);
    check(request("home", ""), true);
    check(request("home", "1234567"), false);
    check(request(std::string(32, 's'), std::string(63, 'p')), true);
    check(request(std::string(33, 's'), "12345678"), false);
    check(request("home", std::string(64, 'a')), true);
    check(request("home", std::string(64, 'x')), false);
    check(request("home", std::string(65, 'a')), false);
    check(request("", "12345678"), false);
    check(request("a\\u0000b", "12345678"), false);
    check(request("home", "1234\\u00005678"), false);
    check(request("home", "1234\\\\u00005678"), true);
    check(request("\xc0\xaf", "12345678"), false);
    check(request("\xed\xa0\x80", "12345678"), false);
    check(request("\xf4\x90\x80\x80", "12345678"), false);
    check(request("home", "12345678") + " {}", false);
    check(request("home", "12345678") + " \n\t", true);
    check("[]", false);
    check("{\"a\":{\"b\":\"c\"}}", false);
    check(std::string(250, '[') + std::string(250, ']'), false);
    check(request("home", "12345678\\\"x"), true);
    check("null", false);
    check("true", false);
    check("{\"ssid\":null,\"password\":\"12345678\",\"token\":\"" + token + "\"}", false);
    check("{\"ssid\":123,\"password\":\"12345678\",\"token\":\"" + token + "\"}", false);
    check("{\"ssid\":\"home\",\"ssid\":\"evil\",\"password\":\"12345678\",\"token\":\"" + token + "\"}", false);
    check("{\"ssid\":\"home\",\"password\":\"12345678\",\"token\":\"" + token + "\",\"extra\":\"x\"}", false);
    check("{\"ssid\":\"home\",\"password\":\"12345678\",\"token\":\"" + token + "\",\"token\":\"" + token + "\"}", false);
    check("{\"SSID\":\"home\",\"password\":\"12345678\",\"token\":\"" + token + "\"}", false);
    check("{\"ssid\":\"home\",\"password\":\"12345678\",\"token\":\"short\"}", false);
    check("{\"ssid\":\"home\",\"password\":\"12345678\",\"token\\u0000\":\"" + token + "\"}", false);
    check("{\"token\":\"" + token + "\"}", true, false);
    check(request("home", "12345678"), false, false);
    check("{\"token\":\"" + token + "\",\"token\":\"" + token + "\"}", false, false);
    check("{\"token\":null}", false, false);
    check(std::string(513, ' '), false);
    check(std::string("{\"ssid\":\"a\0b\"}", 14), false);
    check(request("a\nb", "12345678"), false);
    std::cout << "Wi-Fi request parser: " << checks << " checks passed\n";
}
