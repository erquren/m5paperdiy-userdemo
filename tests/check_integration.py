"""Static integration regressions; device behavior still requires hardware."""
from pathlib import Path
root = Path(__file__).resolve().parents[1]
def read(path): return (root / path).read_text()
main = read('main/main.cpp')
assert main.index('std::make_unique<AppWifiScan>') < main.index('std::make_unique<AppBattery>')
for app in ('app_battery', 'app_tf_card'):
    assert 'if (AppWifiScan::isModal()) return;' in read(f'main/apps/{app}/{app}.cpp')
feedback = read('main/apps/app_button_feedback/app_button_feedback.cpp')
assert '!hal.isShutdownInProgress() && !AppWifiScan::isModal()' in feedback
ui = read('main/apps/app_wifi_scan/app_wifi_scan.cpp')
assert 'MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT' in ui
assert 'd.readRect(0, 0, _saved_width, _saved_height, _saved_pixels)' in ui
assert 'hal.display.pushImage(0, 0, _saved_width, _saved_height, _saved_pixels)' in ui
assert 'hal.wasButtonALongPressed()' in ui
assert 'wifi.tick()' in ui
assert 'AppShutdown::isDialogOpen() || hal.isShutdownInProgress()' in ui
assert ui.index('wifi.tick()') < ui.index('AppShutdown::isDialogOpen()')
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
