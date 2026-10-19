/* SPDX-License-Identifier: MIT */
#include "app_clock.h"

#include <app_shutdown/app_shutdown.h>
#include <clock/time_service.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <hal.h>
#include <limits>

namespace {
constexpr char TAG[] = "AppClock";
struct DisplaySink {
    M5GFX& display;
    void clear(int x, int y, int width, int height) { display.fillRect(x, y, width, height, TFT_WHITE); }
    void horizontal(int x, int y, int width, std::uint8_t gray)
    {
        const auto value = static_cast<std::uint8_t>(gray * 17U);
        display.drawFastHLine(x, y, width, display.color565(value, value, value));
    }
};
}

void AppClock::onCreate()
{
    setAppInfo().name = "AppClock";
    _instance = this;
    open();
}
void AppClock::onRunning()
{
    auto& hal = GetHAL();
    if (AppShutdown::isDialogOpen() || hal.isShutdownInProgress()) return;
    if (hal.isButtonPressed(ButtonId::A) && hal.isButtonPressed(ButtonId::B)) {
        if (isActive()) leave();
        return;
    }
    if (hal.wasButtonBTripleClicked()) {
        if (isActive()) leave();
        else enter();
        return;
    }
    if (!isActive()) return;
    // Short actions are intentionally inert in the full-screen clock. Long A
    // remains owned by WifiScan, which restores this screen before setup.
    hal.wasButtonAClicked();
    hal.wasButtonBClicked();
    render(false);
}
void AppClock::leaveForSystemAction()
{
    if (_instance && isActive()) _instance->leave();
}
bool AppClock::enter()
{
    auto& hal = GetHAL();
    auto& display = hal.display;
    _saved_width = display.width();
    _saved_height = display.height();
    _saved_rotation = display.getRotation();
    if (_saved_width <= 0 || _saved_height <= 0 ||
        static_cast<std::size_t>(_saved_width) > std::numeric_limits<std::size_t>::max() /
            sizeof(std::uint16_t) / static_cast<std::size_t>(_saved_height)) return false;
    const auto size = static_cast<std::size_t>(_saved_width) * _saved_height * sizeof(std::uint16_t);
    _saved_pixels = static_cast<std::uint16_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!_saved_pixels) {
        ESP_LOGW(TAG, "Clock needs free PSRAM; preserving current mode");
        hal.suppressButtonActionsUntilRelease();
        return false;
    }
    {
        DisplayGuard guard(hal);
        // The feedback task rechecks this atomic under the same display lock.
        _active.store(true, std::memory_order_release);
        display.waitDisplay();
        display.clearClipRect();
        display.readRect(0, 0, _saved_width, _saved_height, _saved_pixels);
        display.setRotation(1);
    }
    if (display.width() != clock_render::WIDTH || display.height() != clock_render::HEIGHT) {
        ESP_LOGE(TAG, "Clock requires a 960x540 landscape display");
        leave();
        return false;
    }
    hal.suppressButtonActionsUntilRelease();
    _has_rendered = false;
    render(true);
    return true;
}
void AppClock::leave()
{
    auto& hal = GetHAL();
    {
        DisplayGuard guard(hal);
        hal.display.waitDisplay();
        hal.display.clearClipRect();
        hal.display.setRotation(_saved_rotation);
        if (_saved_pixels) {
            hal.display.pushImage(0, 0, _saved_width, _saved_height, _saved_pixels);
            heap_caps_free(_saved_pixels);
            _saved_pixels = nullptr;
        }
        hal.requestFullRedraw(m5gfx::epd_mode_t::epd_quality);
        _active.store(false, std::memory_order_release);
    }
    hal.suppressButtonActionsUntilRelease();
    _has_rendered = false;
}
void AppClock::render(bool full)
{
    const auto snapshot = TimeService::instance().snapshot();
    const auto next = clock_render::content(snapshot.valid, snapshot.epoch_seconds, snapshot.local_time);
    if (!full && _has_rendered && !clock_render::changed(_last_content, next)) return;
    auto& hal = GetHAL();
    DisplayGuard guard(hal);
    auto& display = hal.display;
    display.clearClipRect();
    DisplaySink sink{display};
    if (full || !_has_rendered) {
        clock_render::drawFrame(sink, next);
        hal.requestFullRedraw(m5gfx::epd_mode_t::epd_quality);
    } else {
        sink.clear(clock_render::MARGIN, clock_render::TIME_TOP,
                   clock_render::WIDTH - 2 * clock_render::MARGIN, clock_render::TIME_HEIGHT);
        clock_render::drawTime(sink, next);
        hal.requestRefresh(clock_render::MARGIN, clock_render::TIME_TOP,
                           clock_render::WIDTH - 2 * clock_render::MARGIN, clock_render::TIME_HEIGHT,
                           m5gfx::epd_mode_t::epd_text);
        if (std::strcmp(_last_content.date, next.date) != 0) {
            sink.clear(clock_render::MARGIN, clock_render::MARGIN,
                       clock_render::WIDTH - 2 * clock_render::MARGIN, clock_render::DATE_HEIGHT);
            clock_render::drawDate(sink, next);
            hal.requestRefresh(clock_render::MARGIN, clock_render::MARGIN,
                               clock_render::WIDTH - 2 * clock_render::MARGIN, clock_render::DATE_HEIGHT,
                               m5gfx::epd_mode_t::epd_text);
        }
    }
    _last_content = next;
    _has_rendered = true;
}
