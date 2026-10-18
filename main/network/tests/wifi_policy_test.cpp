#include "../wifi_policy.h"
#include <cassert>
#include <iostream>
int main()
{
    using namespace wifi_policy;
    unsigned checks = 0;
    auto test = [&](bool value) { assert(value); ++checks; };
    test(retryDelay(1, 0, 0) == 1500);
    test(retryDelay(2, 0, 0) == 5000);
    test(retryDelay(3, 0, 0) == 60000);
    test(retryDelay(3, 1, 0) == 120000);
    test(retryDelay(3, 2, 0) == 240000);
    test(retryDelay(3, 3, 0) == 300000);
    test(retryDelay(3, 9999999, 9999999) == 300999);
    test(!expired(true, 299999, 300000, 0));
    test(expired(true, 300000, 300000, 0));
    test(expired(true, 300001, 300000, 0));
    test(!expired(false, 300001, 300000, 0));
    test(!expired(true, 19999, 300000, 20000));
    test(expired(true, 20000, 300000, 20000));
    test(expired(true, 300000, 300000, 310000));
    test(remaining(true, 0, 300000, 0) == 300);
    test(remaining(true, 299999, 300000, 0) == 1);
    test(remaining(true, 300000, 300000, 0) == 0);
    test(remaining(true, 300001, 300000, 0) == 0);
    test(remaining(false, 0, 300000, 0) == 0);
    test(remaining(true, 10000, 300000, 20000) == 10);
    test(remaining(true, 290000, 300000, 310000) == 10);
    test(currentAttempt(true, false, 2, 2));
    test(!currentAttempt(true, false, 1, 2));
    test(!currentAttempt(false, false, 2, 2));
    test(!currentAttempt(true, true, 2, 2));
    test(mayAcceptIp(true, true, false, 2, 2));
    test(!mayAcceptIp(true, false, false, 2, 2));
    test(!mayAcceptIp(true, true, true, 2, 2));
    test(!mayAcceptIp(true, true, false, 1, 2));
    test(!mayAcceptIp(false, true, false, 2, 2));
    // Repeated status reads have no side effect and cannot extend a session.
    for (unsigned i = 0; i < 1000; ++i) test(remaining(true, 299999, 300000, 0) == 1);
    test(expired(true, 300000, 300000, 0));
    std::cout << "Wi-Fi policy: " << checks << " checks passed\n";
}
