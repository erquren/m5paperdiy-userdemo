#pragma once
#include <string>
namespace mooncake {
class AppAbility {
public:
    struct Info { std::string name; };
    virtual ~AppAbility() = default;
    virtual void onCreate() {}
    virtual void onOpen() {}
    virtual void onRunning() {}
    Info& setAppInfo() { return info; }
    void open() { onOpen(); }
private:
    Info info;
};
}
