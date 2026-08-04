/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"

#include <algorithm>

namespace {

constexpr std::uint32_t PARTIAL_REFRESH_LIMIT = 40;

}  // namespace

/* -------------------------------------------------------------------------- */
/*                               Display Flush                                */
/* -------------------------------------------------------------------------- */
void Hal::flushDisplay()
{
    if (_partial_refresh_request && !_full_redraw_request) {
        flushDisplayRegion(_refresh_x, _refresh_y, _refresh_width, _refresh_height, _partial_refresh_mode);
    } else {
        flushFullDisplay(_full_redraw_request ? _full_redraw_mode : display.getEpdMode());
    }
}

bool Hal::flushDisplayRegion(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                             m5gfx::epd_mode_t mode)
{
    ++_partial_refresh_count;
    if (_partial_refresh_count >= PARTIAL_REFRESH_LIMIT) {
        flushFullDisplay(m5gfx::epd_mode_t::epd_quality);
        return true;
    }
    display.setEpdMode(mode);
    display.display(x, y, width, height);
    return false;
}

void Hal::flushFullDisplay(m5gfx::epd_mode_t mode)
{
    display.setEpdMode(mode);
    display.display(0, 0, display.width(), display.height());
    // Callers hold DisplayGuard; keep it locked until the full waveform completes.
    display.waitDisplay();
    _partial_refresh_count = 0;
}

/* -------------------------------------------------------------------------- */
/*                              Refresh Requests                              */
/* -------------------------------------------------------------------------- */
void Hal::requestRefresh()
{
    _refresh_request = true;
}

void Hal::requestRefresh(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                         m5gfx::epd_mode_t mode)
{
    if (width <= 0 || height <= 0) {
        return;
    }

    if (_partial_refresh_request) {
        const std::int32_t left   = std::min(_refresh_x, x);
        const std::int32_t top    = std::min(_refresh_y, y);
        const std::int32_t right  = std::max(_refresh_x + _refresh_width, x + width);
        const std::int32_t bottom = std::max(_refresh_y + _refresh_height, y + height);
        _refresh_x                = left;
        _refresh_y                = top;
        _refresh_width            = right - left;
        _refresh_height           = bottom - top;
        if (static_cast<int>(mode) < static_cast<int>(_partial_refresh_mode)) {
            _partial_refresh_mode = mode;
        }
    } else {
        _refresh_x               = x;
        _refresh_y               = y;
        _refresh_width           = width;
        _refresh_height          = height;
        _partial_refresh_mode    = mode;
        _partial_refresh_request = true;
    }
    requestRefresh();
}

void Hal::requestFullRedraw(m5gfx::epd_mode_t mode)
{
    _full_redraw_request = true;
    _full_redraw_mode    = mode;
    requestRefresh();
}

bool Hal::isRefreshRequested() const
{
    return _refresh_request;
}

bool Hal::isFullRedrawRequested() const
{
    return _full_redraw_request;
}

m5gfx::epd_mode_t Hal::partialRefreshMode() const
{
    return _partial_refresh_mode;
}

m5gfx::epd_mode_t Hal::fullRedrawMode() const
{
    return _full_redraw_mode;
}

void Hal::clearRefreshRequest()
{
    _refresh_request         = false;
    _full_redraw_request     = false;
    _partial_refresh_request = false;
    _partial_refresh_mode    = m5gfx::epd_mode_t::epd_text;
}
