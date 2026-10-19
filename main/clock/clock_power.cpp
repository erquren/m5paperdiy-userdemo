/* SPDX-License-Identifier: MIT */
#include "clock_power.h"
#include "clock_radio.h"
#include "time_service.h"

#include <algorithm>
#include <atomic>
#include <app_clock/app_clock.h>
#include <app_shutdown/app_shutdown.h>
#include <driver/gpio.h>
#include <driver/usb_serial_jtag.h>
#include <esp_cpu.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <hal.h>
#include <sdkconfig.h>

namespace {
constexpr char TAG[] = "ClockPower";
constexpr int MAX_CPU_MHZ = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
constexpr int IDLE_CPU_MHZ = 40;
constexpr std::int64_t SECOND_US = 1000000;
// PaperDIY mappings from the pinned M5GFX/M5Unified board configuration:
// EPD outputs (including boost GPIO46 and OE GPIO45), TF MOSI/CLK/CS, and
// externally pulled-up PMIC I2C SDA/SCL. Retain their driver's idle levels.
// Do not exempt unused/input-only pins from the SDK's ESD reset workaround.
constexpr int RETAINED_BOARD_PINS[] = {
    6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 45, 46,
    38, 39, 47, 41, 42,
};

bool debugConnected()
{
    // Keep USB Serial/JTAG and an attached debugger usable. The SDK's USB
    // connection monitor additionally holds its own NO_LIGHT_SLEEP lock.
    return esp_cpu_dbgr_is_attached()
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED
        || usb_serial_jtag_is_connected()
#endif
        ;
}
}

struct ClockPower::Impl {
    bool attempted = false;
    std::int64_t frame_deadline_us = 0;
    std::uint32_t frame_sync_generation = 0;
    std::atomic<bool> ready{false};
    std::atomic<unsigned> work_count{0};
    std::atomic<TaskHandle_t> feedback_task{nullptr};
    SemaphoreHandle_t work_mutex = nullptr;
    esp_pm_lock_handle_t main_cpu = nullptr;
    esp_pm_lock_handle_t main_awake = nullptr;
    esp_pm_lock_handle_t work_cpu = nullptr;
    esp_pm_lock_handle_t work_awake = nullptr;
    bool main_cpu_held = false;
    bool main_awake_held = false;

    void failClosed(const char* operation, esp_err_t error)
    {
        ready.store(false, std::memory_order_release);
        // No lock failure may leave automatic sleep or DFS enabled. Existing
        // locks remain owned until their normal release; fixed-frequency PM is
        // also safe if setup failed before any of them could be acquired.
        const esp_pm_config_t fallback = {MAX_CPU_MHZ, MAX_CPU_MHZ, false};
        const auto fallback_error = esp_pm_configure(&fallback);
        ESP_LOGW(TAG, "%s failed (%s); using awake fallback (%s)", operation,
                 esp_err_to_name(error), esp_err_to_name(fallback_error));
    }
    bool acquireMain()
    {
        auto error = ESP_OK;
        if (!main_awake_held) {
            error = esp_pm_lock_acquire(main_awake);
            main_awake_held = error == ESP_OK;
        }
        if (error == ESP_OK && !main_cpu_held) {
            error = esp_pm_lock_acquire(main_cpu);
            main_cpu_held = error == ESP_OK;
        }
        if (error != ESP_OK) failClosed("Main PM lock", error);
        return error == ESP_OK;
    }
    bool releaseMain()
    {
        auto error = esp_pm_lock_release(main_cpu);
        if (error == ESP_OK) {
            main_cpu_held = false;
            error = esp_pm_lock_release(main_awake);
            if (error == ESP_OK) main_awake_held = false;
        }
        if (error != ESP_OK) failClosed("Main PM unlock", error);
        return error == ESP_OK;
    }
};

ClockPower::ClockPower()
{
    static Impl implementation;
    _impl = &implementation;
}
ClockPower& ClockPower::instance()
{
    static ClockPower power;
    return power;
}

void ClockPower::begin()
{
    auto& self = *_impl;
    if (self.attempted) return;
    self.attempted = true;
    auto& hal = GetHAL();
    hal.setButtonWakeTask(xTaskGetCurrentTaskHandle());
#if !CONFIG_PM_ENABLE || !CONFIG_FREERTOS_USE_TICKLESS_IDLE
    ESP_LOGW(TAG, "PM/tickless support disabled; retaining awake polling");
    return;
#endif
    if (!hal.buttonWakeReady()) {
        ESP_LOGW(TAG, "Button interrupts unavailable; retaining awake polling");
        return;
    }
    self.work_mutex = xSemaphoreCreateMutex();
    if (!self.work_mutex) {
        ESP_LOGW(TAG, "PM mutex unavailable; retaining awake polling");
        return;
    }
    auto error = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "clock_main", &self.main_cpu);
    if (error == ESP_OK) error = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "clock_main", &self.main_awake);
    if (error == ESP_OK) error = esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "clock_work", &self.work_cpu);
    if (error == ESP_OK) error = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "clock_work", &self.work_awake);
    if (error != ESP_OK) {
        self.failClosed("PM lock creation", error);
        return;
    }
    if (!self.acquireMain()) return;
    // ESP32-S3's GPIO-reset workaround selects sleep isolation even if
    // PM_SLP_DISABLE_GPIO was requested off. Keep that workaround, but never
    // float EPD boost/OE or the powered TF card's CS during clock sleep.
    for (const int pin : RETAINED_BOARD_PINS) {
        error = gpio_sleep_sel_dis(static_cast<gpio_num_t>(pin));
        if (error != ESP_OK) {
            self.failClosed("Board GPIO sleep retention", error);
            return;
        }
    }
    // Low-level wake remains asserted across a sleep-entry race. Hal masks
    // the level IRQ on the first press, polls through the gesture, and rearms
    // only after release; gpio_wakeup_enable uses the same trigger type.
    // This IDF API also disables sleep isolation for the two wake pins.
    error = gpio_wakeup_enable(GPIO_NUM_4, GPIO_INTR_LOW_LEVEL);
    if (error == ESP_OK) error = gpio_wakeup_enable(GPIO_NUM_3, GPIO_INTR_LOW_LEVEL);
    if (error == ESP_OK) error = esp_sleep_enable_gpio_wakeup();
    if (error != ESP_OK) {
        self.failClosed("Button light-sleep wake", error);
        return;
    }
    const esp_pm_config_t config = {MAX_CPU_MHZ, IDLE_CPU_MHZ, true};
    error = esp_pm_configure(&config);
    if (error != ESP_OK) {
        self.failClosed("PM configuration", error);
        return;
    }
    self.ready.store(true, std::memory_order_release);
    ESP_LOGI(TAG, "Clock idle: automatic light sleep, GPIO wake, CPU %d-%d MHz", IDLE_CPU_MHZ, MAX_CPU_MHZ);
}

bool ClockPower::lockWork()
{
    auto& self = *_impl;
    if (!self.ready.load(std::memory_order_acquire)) return false;
    xSemaphoreTake(self.work_mutex, portMAX_DELAY);
    if (!self.ready.load(std::memory_order_acquire)) {
        xSemaphoreGive(self.work_mutex);
        return false;
    }
    auto error = esp_pm_lock_acquire(self.work_awake);
    if (error == ESP_OK) error = esp_pm_lock_acquire(self.work_cpu);
    if (error == ESP_OK) self.work_count.fetch_add(1, std::memory_order_release);
    else self.failClosed("Work PM lock", error);
    xSemaphoreGive(self.work_mutex);
    return error == ESP_OK;
}
void ClockPower::unlockWork()
{
    auto& self = *_impl;
    // SDK PM handles are recursive but not safe for concurrent calls on the
    // same handle. Serialize EPD/main and TF/background acquisitions/releases.
    xSemaphoreTake(self.work_mutex, portMAX_DELAY);
    auto error = esp_pm_lock_release(self.work_cpu);
    if (error == ESP_OK) error = esp_pm_lock_release(self.work_awake);
    self.work_count.fetch_sub(1, std::memory_order_release);
    if (error != ESP_OK) self.failClosed("Work PM unlock", error);
    xSemaphoreGive(self.work_mutex);
}

void ClockPower::waitForClockExit()
{
    auto& self = *_impl;
    self.feedback_task.store(xTaskGetCurrentTaskHandle(), std::memory_order_release);
    // Publish before rechecking the mode: an exit before/during this check
    // either skips the wait or leaves a notification ready to consume.
    if (AppClock::isActive()) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

void ClockPower::beginFrame()
{
    auto& self = *_impl;
    self.frame_sync_generation = TimeService::instance().snapshot().sync_generation;
    self.frame_deadline_us = TimeService::instance().nextMinuteDeadlineUs();
}

void ClockPower::idle()
{
    auto& self = *_impl;
    auto& hal = GetHAL();
    const bool clock_active = AppClock::isActive();
    if (!clock_active) {
        if (auto task = self.feedback_task.exchange(nullptr, std::memory_order_acq_rel)) xTaskNotifyGive(task);
    }
    // A prior setup framebuffer can remain hidden under the clock. The radio
    // sleepSafe gate, not that hidden UI flag, proves AP/HTTP teardown finished.
    const bool may_idle = self.ready.load(std::memory_order_acquire) && clock_active &&
        !hal.isShutdownInProgress() && !AppShutdown::isDialogOpen() &&
        !debugConnected() && hal.buttonWakeReady();
    hal.setButtonIdleMode(may_idle);
    if (!may_idle || !hal.isButtonIdle() || hal.isRefreshRequested() ||
        self.work_count.load(std::memory_order_acquire) != 0 || !ClockRadio::instance().sleepSafe()) {
        hal.feedTheDog();
        return;
    }
    const auto now = esp_timer_get_time();
    // AppClock renders before the radio/time tick. A minute crossed during
    // this frame, or a new accepted SNTP sample, requires another frame now.
    if (TimeService::instance().snapshot().sync_generation != self.frame_sync_generation) {
        hal.feedTheDog();
        return;
    }
    const auto deadline = std::min(self.frame_deadline_us, ClockRadio::instance().nextWakeDeadlineUs());
    const auto remaining = deadline - now;
    // Round DOWN: waking a fraction of an RTOS tick early is safe; on that
    // short final iteration normal polling renders after the real boundary.
    const auto ticks = remaining > 0 ? remaining * configTICK_RATE_HZ / SECOND_US : 0;
    if (ticks < 2) {
        hal.feedTheDog();
        return;
    }
    // Do not clear notifications before waiting. An edge racing any of the
    // preceding checks must make this take return immediately, even if its
    // GPIO already returned high. The input task also rechecks raw pins.
    if (!self.releaseMain()) {
        hal.setButtonIdleMode(false);
        hal.feedTheDog();
        return;
    }
    ulTaskNotifyTake(pdTRUE, static_cast<TickType_t>(std::min<std::int64_t>(ticks, portMAX_DELAY - 1)));
    self.acquireMain();
}
