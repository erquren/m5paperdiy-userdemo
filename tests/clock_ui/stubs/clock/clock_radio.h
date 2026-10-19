#pragma once
#include <network/wifi_manager.h>
#include <clock/time_service.h>
class ClockRadio {
public:
 static ClockRadio& instance() { static ClockRadio value; return value; }
 void setActive(bool value) { active = value; ++transitions; if(value) WifiManager::instance().stopProvisioning(); }
 void tick() { WifiManager::instance().tick(); TimeService::instance().tick(); }
 bool active = false;
 int transitions = 0;
};
