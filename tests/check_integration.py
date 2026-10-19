"""Static integration regressions; device behavior still requires hardware."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
def read(path): return (root / path).read_text()
main = read('main/main.cpp')
assert main.index('std::make_unique<AppClock>') < main.index('std::make_unique<AppWifiScan>')
assert main.index('std::make_unique<AppWifiScan>') < main.index('std::make_unique<AppBattery>')
for app in ('app_battery', 'app_tf_card'):
    assert 'if (AppWifiScan::isModal() || AppClock::isActive()) return;' in read(f'main/apps/{app}/{app}.cpp')
feedback = read('main/apps/app_button_feedback/app_button_feedback.cpp')
assert '!hal.isShutdownInProgress() && !AppWifiScan::isModal() && !AppClock::isActive()' in feedback
assert 'hal.isShutdownInProgress() || AppWifiScan::isModal() || AppClock::isActive()' in feedback
ui = read('main/apps/app_wifi_scan/app_wifi_scan.cpp')
assert 'MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT' in ui
assert 'd.readRect(0, 0, _saved_width, _saved_height, _saved_pixels)' in ui
assert 'hal.display.pushImage(0, 0, _saved_width, _saved_height, _saved_pixels)' in ui
assert 'hal.wasButtonALongPressed()' in ui
assert 'ClockRadio::instance().tick()' in ui
assert 'AppShutdown::isDialogOpen() || hal.isShutdownInProgress()' in ui
assert ui.index('ClockRadio::instance().tick()') < ui.index('AppShutdown::isDialogOpen()')
assert 'wifi.stopProvisioning()' in ui
assert 'http://192.168.4.1' in ui
assert 'hal.isButtonPressed(ButtonId::A) && hal.isButtonPressed(ButtonId::B)' in ui
cmake=read('main/CMakeLists.txt')
assert 'assets/wifi_setup.html' in cmake
assert 'esp_http_server' in cmake
manager=read('main/network/wifi_manager.cpp')
assert 'global_user_ctx_free_fn' in manager
assert 'nvs_flash_erase' not in read('main/apps/app_shutdown/app_shutdown.cpp')
print('PASS: app ordering, modal isolation, framebuffer lifecycle, gesture integration, portal asset')

clock = read("main/apps/app_clock/app_clock.cpp")
assert "display.setRotation(1)" in clock
assert "hal.display.setRotation(_saved_rotation)" in clock
assert clock.index("_active.store(true") < clock.index("display.setRotation(1)")
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in clock
assert "hal.wasButtonBTripleClicked()" in clock
assert "hal.wasButtonAClicked();" in clock and "hal.wasButtonBClicked();" in clock
assert ui.index("ClockRadio::instance().tick()") < ui.index("if (AppClock::isActive())")
assert "AppClock::leaveForSystemAction();" in ui
assert "./clock/clock_fonts.cpp" in cmake
print("PASS: clock ordering, landscape ownership, short-click isolation, time lifecycle")

assert "ClockRadio::instance().setActive(true)" in clock
assert "ClockRadio::instance().setActive(false)" in clock
assert "./clock/clock_radio.cpp" in cmake
