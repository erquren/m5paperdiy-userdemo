#include "hal.h"
#include "../../main/apps/app_clock/app_clock.h"
#include "../../main/apps/app_wifi_scan/app_wifi_scan.h"
#include "app_shutdown/app_shutdown.h"
#include "../../main/clock/time_service.h"
#include "../../main/clock/time_policy.h"
#include <clock/clock_radio.h>
#include <cstdlib>
#include <iostream>

namespace {
Hal hal;
TimeService::Snapshot time_snapshot;
WifiManager::Snapshot wifi_snapshot;
bool fail_allocation = false, defer_portal = false;
int allocations = 0, wifi_ticks = 0, time_ticks = 0, scans = 0;
void frame(AppClock& clock, AppWifiScan& wifi) {clock.onRunning();wifi.onRunning();}
void toggle(AppClock& clock, AppWifiScan& wifi) {hal.triple=true;frame(clock,wifi);}
void setTime(std::int64_t value) {
    time_snapshot.valid = true; time_snapshot.epoch_seconds=value;
    time_snapshot.local_time=clock_policy::chinaTime(value);
}
}
Hal& GetHAL() {return hal;}
void* heap_caps_malloc(std::size_t size,unsigned caps) {
    assert(caps==3);
    if(fail_allocation)return nullptr;
    void* p=std::malloc(size);if(p)++allocations;return p;
}
void heap_caps_free(void* p) {if(p)--allocations;std::free(p);}
TimeService::TimeService():_impl(nullptr) {}
TimeService& TimeService::instance() {static TimeService value;return value;}
bool TimeService::begin() {return true;}
void TimeService::tick() {++time_ticks;}
TimeService::Snapshot TimeService::snapshot() const {return time_snapshot;}
WifiManager::WifiManager():_impl(nullptr) {}
WifiManager& WifiManager::instance() {static WifiManager value;return value;}
bool WifiManager::begin() {return true;}
void WifiManager::tick() {++wifi_ticks;}
bool WifiManager::startProvisioning() {wifi_snapshot.provisioning=true;wifi_snapshot.ap_ssid=defer_portal?"":"test";wifi_snapshot.ap_password=defer_portal?"":"12345678";wifi_snapshot.seconds_remaining=300;++wifi_snapshot.revision;return true;}
void WifiManager::stopProvisioning() {wifi_snapshot.provisioning=false;++wifi_snapshot.revision;}
bool WifiManager::provisioning() const {return wifi_snapshot.provisioning;}
WifiManager::Snapshot WifiManager::snapshot() const {return wifi_snapshot;}
void WifiManager::requestScan() {++scans;}

int main()
{
    AppClock clock;
    AppWifiScan wifi;
    clock.onCreate();wifi.onCreate();frame(clock,wifi);
    const auto normal=hal.display.pixels;
    fail_allocation=true;
    toggle(clock,wifi);
    assert(!AppClock::isActive() && hal.display.getRotation()==0 && hal.display.pixels==normal && allocations==0);
    fail_allocation=false;
    toggle(clock,wifi);
    assert(AppClock::isActive() && hal.display.getRotation()==1 && allocations==1);
    const int initial_draws=hal.display.draws;
    const int start_wifi_ticks=wifi_ticks, start_time_ticks=time_ticks;
    wifi_snapshot.message="hidden update";++wifi_snapshot.revision;
    hal.short_a=hal.short_b=true;
    frame(clock,wifi);
    assert(hal.display.draws==initial_draws && wifi_ticks==start_wifi_ticks+1 && time_ticks==start_time_ticks+1);
    assert(!hal.short_a && !hal.short_b && scans==1);
    setTime(1792282080);
    frame(clock,wifi);
    const int synchronized_draws=hal.display.draws;
    assert(synchronized_draws>initial_draws);
    ++time_snapshot.revision;time_snapshot.network_available=true;
    frame(clock,wifi);
    assert(hal.display.draws==synchronized_draws); // No redraw on network/status-only changes.
    setTime(1792282139);frame(clock,wifi);
    assert(hal.display.draws==synchronized_draws);
    setTime(1792282140);frame(clock,wifi);
    assert(hal.display.draws>synchronized_draws);
    const int minute_draws=hal.display.draws;
    setTime(1792282080);frame(clock,wifi);
    assert(hal.display.draws>minute_draws);
    toggle(clock,wifi);
    assert(!AppClock::isActive() && hal.display.getRotation()==0 && allocations==0);
    for(int i=0;i<25;++i) {toggle(clock,wifi);toggle(clock,wifi);}
    assert(allocations==0 && !AppClock::isActive() && hal.display.getRotation()==0);

    // Entering clock closes a prior AP; the hidden framebuffer is reconciled on exit.
    hal.long_a=true;frame(clock,wifi);
    assert(AppWifiScan::isModal() && wifi_snapshot.provisioning && allocations==1);
    const auto portal=hal.display.pixels;
    fail_allocation=true;toggle(clock,wifi);fail_allocation=false;
    assert(!AppClock::isActive() && AppWifiScan::isModal() && hal.display.pixels==portal && allocations==1);
    assert(wifi_snapshot.provisioning && !ClockRadio::instance().active);
    toggle(clock,wifi);
    assert(AppClock::isActive() && ClockRadio::instance().active && AppWifiScan::isModal() && allocations==2);
    assert(!wifi_snapshot.provisioning);
    const auto clock_pixels=hal.display.pixels;
    frame(clock,wifi);
    assert(hal.display.pixels==clock_pixels);
    toggle(clock,wifi);
    assert(!AppClock::isActive() && !ClockRadio::instance().active && !AppWifiScan::isModal());
    assert(hal.display.getRotation()==0 && allocations==0);

    // Long A exits the clock and deliberately opens fresh provisioning, even
    // when its previous page was a portal that was closed on clock entry.
    toggle(clock,wifi);hal.long_a=true;frame(clock,wifi);
    assert(!AppClock::isActive() && AppWifiScan::isModal() && wifi_snapshot.provisioning && hal.display.getRotation()==0);
    toggle(clock,wifi);hal.long_a=true;frame(clock,wifi);
    assert(!AppClock::isActive() && AppWifiScan::isModal() && wifi_snapshot.provisioning && allocations==1);
    hal.long_a=true;frame(clock,wifi);
    assert(!AppWifiScan::isModal() && !wifi_snapshot.provisioning && allocations==0);
    // A pending driver stop delays the portal credentials: never draw an empty
    // QR, then repaint the whole setup screen when the fresh AP identity arrives.
    defer_portal=true;hal.long_a=true;frame(clock,wifi);
    assert(AppWifiScan::isModal() && wifi_snapshot.ap_ssid.empty());
    const int pending_qr=hal.display.qr_count, pending_full=hal.full_refreshes;
    frame(clock,wifi);assert(hal.display.qr_count==pending_qr);
    wifi_snapshot.ap_ssid="fresh-ap";wifi_snapshot.ap_password="fresh-password";++wifi_snapshot.revision;
    frame(clock,wifi);assert(hal.display.qr_count==pending_qr+2 && hal.full_refreshes==pending_full+1);
    defer_portal=false;hal.long_a=true;frame(clock,wifi);
    assert(!AppWifiScan::isModal() && allocations==0);
    // A+B restores portrait before the shutdown controller (not compiled here).
    toggle(clock,wifi);hal.a=hal.b=true;frame(clock,wifi);
    assert(!AppClock::isActive() && hal.display.getRotation()==0 && allocations==0);
    hal.a=hal.b=false;
    hal.long_a=true;frame(clock,wifi);toggle(clock,wifi);hal.a=hal.b=true;frame(clock,wifi);
    assert(!AppClock::isActive() && !AppWifiScan::isModal() && !wifi_snapshot.provisioning && hal.display.getRotation()==0 && allocations==0);
    hal.a=hal.b=false;
    AppShutdown::dialog=true;hal.triple=true;frame(clock,wifi);
    assert(!AppClock::isActive() && allocations==0);
    hal.triple=false;AppShutdown::dialog=false;
    assert(hal.display.lock_depth==0);
    std::cout << "PASS: production clock/Wi-Fi lifecycle, PSRAM failure/repeated restore, clock AP shutdown, explicit provisioning override, network policy tick, short-click isolation, long A and A+B priorities\n";
}
