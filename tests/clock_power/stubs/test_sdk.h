#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>
#include <cassert>
#include <cstdio>

#ifdef TEST_PM_DISABLED
#define CONFIG_PM_ENABLE 0
#else
#define CONFIG_PM_ENABLE 1
#endif
#define CONFIG_FREERTOS_USE_TICKLESS_IDLE 1
#define CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED 1
#define CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ 240
#define configTICK_RATE_HZ 1000
using TickType_t = std::uint32_t;
using TaskHandle_t = void*;
using SemaphoreHandle_t = std::mutex*;
constexpr auto portMAX_DELAY = UINT32_MAX;
constexpr int pdTRUE = 1;
using esp_err_t = int;
constexpr auto ESP_OK = 0;
constexpr auto ESP_FAIL = -1;
constexpr auto ESP_ERR_NO_MEM = 0x101;
enum gpio_num_t { GPIO_NUM_3 = 3, GPIO_NUM_4 = 4 };
enum gpio_int_type_t { GPIO_INTR_LOW_LEVEL };
enum esp_pm_lock_type_t { ESP_PM_CPU_FREQ_MAX, ESP_PM_NO_LIGHT_SLEEP };
struct esp_pm_lock { esp_pm_lock_type_t type; int depth = 0; };
using esp_pm_lock_handle_t = esp_pm_lock*;
struct esp_pm_config_t { int max_freq_mhz; int min_freq_mhz; bool light_sleep_enable; };

namespace mock {
inline std::uint32_t sync_generation = 0;
inline std::int64_t now = 1000000;
inline std::int64_t minute_deadline = 12234567;
inline std::int64_t radio_deadline = 3600000000;
inline bool clock_active = true, dialog = false, usb = false, debugger = false, radio_safe = true;
inline bool fail_mutex = false;
inline int create_fail_at = 0, acquire_fail_at = 0, release_fail_at = 0, gpio_fail_at = 0;
inline int retention_fail_at = 0, retention_calls = 0, retention_successes = 0;
inline int creates = 0, acquires = 0, releases = 0, gpio_calls = 0, configure_calls = 0;
inline bool fail_sleep_wake = false, fail_configure = false;
inline int feed_count = 0, take_count = 0, pending_notifications = 0, feedback_notifications = 0;
inline TaskHandle_t task = reinterpret_cast<void*>(1);
inline TaskHandle_t notified_task = nullptr;
inline TickType_t waited_ticks = 0;
inline std::function<void()> on_wait, on_display_wait;
inline std::vector<esp_pm_lock_handle_t> locks;
inline std::vector<int> wake_pins, retained_pins;
inline esp_pm_config_t config = {240, 240, false};
inline int held(esp_pm_lock_type_t type) {
    int count = 0;
    for (auto* lock : locks) if (lock->type == type) count += lock->depth;
    return count;
}
}
inline const char* esp_err_to_name(int error) { return error == 0 ? "OK" : "ERROR"; }
#define ESP_LOGW(...) do {} while (0)
#define ESP_LOGI(...) do {} while (0)
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return mock::fail_mutex ? nullptr : new std::mutex; }
inline int xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t) { mutex->lock(); return 1; }
inline int xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); return 1; }
inline TaskHandle_t xTaskGetCurrentTaskHandle() { return mock::task; }
inline int ulTaskNotifyTake(int clear, TickType_t ticks) {
    assert(clear == pdTRUE);
    ++mock::take_count;
    mock::waited_ticks = ticks;
    if (mock::on_wait) mock::on_wait();
    const auto pending = mock::pending_notifications;
    mock::pending_notifications = 0;
    return pending;
}
inline void xTaskNotifyGive(TaskHandle_t task) {
    ++mock::feedback_notifications;
    mock::notified_task = task;
}
inline bool usb_serial_jtag_is_connected() { return mock::usb; }
inline bool esp_cpu_dbgr_is_attached() { return mock::debugger; }
inline std::int64_t esp_timer_get_time() { return mock::now; }
inline esp_err_t esp_pm_lock_create(esp_pm_lock_type_t type, int, const char*, esp_pm_lock_handle_t* out) {
    if (++mock::creates == mock::create_fail_at) return ESP_ERR_NO_MEM;
    *out = new esp_pm_lock{type};
    mock::locks.push_back(*out);
    return ESP_OK;
}
inline esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t lock) {
    assert(lock);
    if (++mock::acquires == mock::acquire_fail_at) return ESP_FAIL;
    ++lock->depth;
    return ESP_OK;
}
inline esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t lock) {
    assert(lock && lock->depth > 0);
    if (++mock::releases == mock::release_fail_at) return ESP_FAIL;
    --lock->depth;
    return ESP_OK;
}
inline esp_err_t esp_pm_configure(const void* data) {
    ++mock::configure_calls;
    const auto config = *static_cast<const esp_pm_config_t*>(data);
    if (config.light_sleep_enable) {
        assert(mock::retention_successes == 20);
        assert(mock::wake_pins == (std::vector<int>{4, 3}));
        assert(!mock::fail_sleep_wake);
    }
    if (mock::fail_configure && config.light_sleep_enable) return ESP_FAIL;
    mock::config = config;
    return ESP_OK;
}
inline esp_err_t gpio_sleep_sel_dis(gpio_num_t pin) {
    mock::retained_pins.push_back(pin);
    if (++mock::retention_calls == mock::retention_fail_at) return ESP_FAIL;
    ++mock::retention_successes;
    return ESP_OK;
}
inline esp_err_t gpio_wakeup_enable(gpio_num_t pin, gpio_int_type_t type) {
    assert(type == GPIO_INTR_LOW_LEVEL);
    mock::wake_pins.push_back(pin);
    return ++mock::gpio_calls == mock::gpio_fail_at ? ESP_FAIL : ESP_OK;
}
inline esp_err_t esp_sleep_enable_gpio_wakeup() { return mock::fail_sleep_wake ? ESP_FAIL : ESP_OK; }
class AppClock { public: static bool isActive() { return mock::clock_active; } };
class AppShutdown { public: static bool isDialogOpen() { return mock::dialog; } };
namespace m5gfx { enum class epd_mode_t { epd_quality, epd_text, epd_fastest }; }
struct FakeDisplay {
    int submissions = 0, completions = 0;
    bool busy = false;
    m5gfx::epd_mode_t mode = m5gfx::epd_mode_t::epd_quality;
    void setEpdMode(m5gfx::epd_mode_t value) { mode = value; }
    auto getEpdMode() const { return mode; }
    int width() const { return 540; }
    int height() const { return 960; }
    void display(int, int, int, int) {
        assert(!busy);
        assert(mock::held(ESP_PM_CPU_FREQ_MAX) > 0);
        assert(mock::held(ESP_PM_NO_LIGHT_SLEEP) > 0);
        busy = true;
        ++submissions;
    }
    void waitDisplay() {
        assert(busy);
        assert(mock::held(ESP_PM_CPU_FREQ_MAX) > 0);
        assert(mock::held(ESP_PM_NO_LIGHT_SLEEP) > 0);
        if (mock::on_display_wait) mock::on_display_wait();
        busy = false;
        ++completions;
    }
};
class Hal {
public:
    bool wake_ready = true, input_idle = true, shutdown = false, idle_mode = false;
    TaskHandle_t wake_task = nullptr;
    FakeDisplay display;
    bool buttonWakeReady() const { return wake_ready; }
    bool isButtonIdle() { return input_idle; }
    void setButtonWakeTask(TaskHandle_t task) { wake_task = task; }
    void setButtonIdleMode(bool mode) { idle_mode = mode; }
    bool isShutdownInProgress() const { return shutdown; }
    void feedTheDog() { ++mock::feed_count; }
    void flushDisplay();
    bool flushDisplayRegion(std::int32_t, std::int32_t, std::int32_t, std::int32_t, m5gfx::epd_mode_t);
    void flushFullDisplay(m5gfx::epd_mode_t);
    void requestRefresh();
    void requestRefresh(std::int32_t, std::int32_t, std::int32_t, std::int32_t, m5gfx::epd_mode_t);
    void requestFullRedraw(m5gfx::epd_mode_t);
    bool isRefreshRequested() const;
    bool isFullRedrawRequested() const;
    m5gfx::epd_mode_t partialRefreshMode() const;
    m5gfx::epd_mode_t fullRedrawMode() const;
    void clearRefreshRequest();
    bool _refresh_request = false, _full_redraw_request = false, _partial_refresh_request = false;
    std::int32_t _refresh_x = 0, _refresh_y = 0, _refresh_width = 0, _refresh_height = 0;
    m5gfx::epd_mode_t _partial_refresh_mode = m5gfx::epd_mode_t::epd_text;
    m5gfx::epd_mode_t _full_redraw_mode = m5gfx::epd_mode_t::epd_quality;
    std::uint32_t _partial_refresh_count = 0;
};
inline Hal& GetHAL() { static Hal hal; return hal; }
