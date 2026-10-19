/* SPDX-License-Identifier: MIT */
#pragma once

#include "clock_fonts.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

// Display-independent production renderer. Host tests use this exact code and
// the same four-bit font pixels as the panel; no OS fonts are needed at runtime.
namespace clock_render {
constexpr int WIDTH = 960;
constexpr int HEIGHT = 540;
constexpr int MARGIN = 32;
constexpr int TIME_TOP = 108;
constexpr int TIME_HEIGHT = 324;
constexpr int DATE_HEIGHT = 44;

struct Content {
    bool valid = false;
    std::int64_t minute = 0;
    char date[64] = {};
    char time[6] = "--:--";
};
inline Content content(bool valid, std::int64_t epoch_seconds, const std::tm& local)
{
    Content result;
    result.valid = valid;
    if (!valid) return result;
    result.minute = epoch_seconds / 60;
    constexpr const char* weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
    const int weekday = local.tm_wday >= 0 && local.tm_wday < 7 ? local.tm_wday : 0;
    std::snprintf(result.date, sizeof(result.date), "%04d年%d月%d日 星期%s",
                  local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, weekdays[weekday]);
    // The time service guarantees normalized tm values. Bound the values here
    // as well so an invalid caller cannot overrun the fixed HH:MM buffer.
    std::snprintf(result.time, sizeof(result.time), "%02u:%02u",
                  static_cast<unsigned>(local.tm_hour) % 24U, static_cast<unsigned>(local.tm_min) % 60U);
    return result;
}
inline bool changed(const Content& previous, const Content& next)
{
    return previous.valid != next.valid || (next.valid && previous.minute != next.minute) ||
           std::strcmp(previous.date, next.date) != 0 || std::strcmp(previous.time, next.time) != 0;
}
struct Bounds { int x = 0, y = 0, width = 0, height = 0; };
inline std::uint32_t nextCodepoint(const char*& text)
{
    const auto first = static_cast<unsigned char>(*text++);
    if (first < 0x80) return first;
    unsigned count = first < 0xe0 ? 1 : (first < 0xf0 ? 2 : 3);
    std::uint32_t result = first & (0x7fU >> (count + 1));
    while (count-- && *text) result = (result << 6) | (static_cast<unsigned char>(*text++) & 0x3fU);
    return result;
}
inline const clock_fonts::Glyph* glyph(const clock_fonts::Font& font, std::uint32_t codepoint)
{
    for (std::size_t i = 0; i < font.count; ++i) {
        if (font.glyphs[i].codepoint == codepoint) return &font.glyphs[i];
    }
    return nullptr;
}
inline int pixelPosition(std::int32_t position_64) { return (position_64 + 32) / 64; }
inline Bounds bounds(const clock_fonts::Font& font, const char* text)
{
    Bounds result;
    bool have_ink = false;
    std::int32_t advance = 0;
    while (*text) {
        const auto* g = glyph(font, nextCodepoint(text));
        if (!g) continue;
        if (g->width && g->height) {
            const int x = pixelPosition(advance) + g->left;
            const int right = x + g->width;
            const int bottom = g->top + g->height;
            if (!have_ink) {
                result = {x, g->top, g->width, g->height};
                have_ink = true;
            } else {
                const int max_x = std::max(result.x + result.width, right);
                const int max_y = std::max(result.y + result.height, bottom);
                result.x = std::min(result.x, x);
                result.y = std::min(result.y, static_cast<int>(g->top));
                result.width = max_x - result.x;
                result.height = max_y - result.y;
            }
        }
        advance += g->advance_64;
    }
    return result;
}
inline std::uint8_t shade(const std::uint8_t* pixels, std::size_t index)
{
    const auto byte = pixels[index / 2];
    return (index & 1U) ? byte & 15U : byte >> 4;
}
template<class Sink>
void drawGlyph(Sink& sink, const clock_fonts::Glyph& g, const std::uint8_t* pixels, int x, int y)
{
    pixels += g.offset;
    for (int row = 0; row < g.height; ++row) {
        int col = 0;
        while (col < g.width) {
            const auto gray = shade(pixels, static_cast<std::size_t>(row) * g.width + col);
            int end = col + 1;
            while (end < g.width && shade(pixels, static_cast<std::size_t>(row) * g.width + end) == gray) ++end;
            if (gray != 15) sink.horizontal(x + col, y + row, end - col, gray);
            col = end;
        }
    }
}
template<class Sink>
void drawText(Sink& sink, const clock_fonts::Font& font, const char* text, int x, int baseline)
{
    std::int32_t advance = 0;
    while (*text) {
        const auto* g = glyph(font, nextCodepoint(text));
        if (!g) continue;
        drawGlyph(sink, *g, font.pixels, x + pixelPosition(advance) + g->left, baseline + g->top);
        advance += g->advance_64;
    }
}
template<class Sink>
void drawDate(Sink& sink, const Content& value)
{
    const auto box = bounds(clock_fonts::DATE, value.date);
    drawText(sink, clock_fonts::DATE, value.date, MARGIN - box.x, MARGIN - box.y);
}
template<class Sink>
void drawTime(Sink& sink, const Content& value)
{
    const auto box = bounds(clock_fonts::TIME, value.time);
    drawText(sink, clock_fonts::TIME, value.time,
             (WIDTH - box.width) / 2 - box.x, (HEIGHT - box.height) / 2 - box.y);
}
template<class Sink>
void drawFooter(Sink& sink)
{
    drawGlyph(sink, clock_fonts::FOOTER, clock_fonts::FOOTER_PIXELS,
              WIDTH - MARGIN - clock_fonts::FOOTER.width, HEIGHT - MARGIN - clock_fonts::FOOTER.height);
}
template<class Sink>
void drawFrame(Sink& sink, const Content& value)
{
    sink.clear(0, 0, WIDTH, HEIGHT);
    drawDate(sink, value);
    drawTime(sink, value);
    drawFooter(sink);
}
}  // namespace clock_render
