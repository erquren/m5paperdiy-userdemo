#include "../../main/clock/clock_render.h"
#include "../../main/clock/time_policy.h"
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

struct Canvas {
    std::vector<unsigned char> pixels = std::vector<unsigned char>(960 * 540, 255);
    void clear(int x, int y, int width, int height)
    {
        assert(x >= 0 && y >= 0 && x + width <= 960 && y + height <= 540);
        for (int row = y; row < y + height; ++row)
            for (int col = x; col < x + width; ++col) pixels[row * 960 + col] = 255;
    }
    void horizontal(int x, int y, int width, std::uint8_t gray)
    {
        assert(x >= 0 && y >= 0 && x + width <= 960 && y < 540 && gray <= 15);
        for (int col = x; col < x + width; ++col) pixels[y * 960 + col] = gray * 17;
    }
    void save(const std::string& path)
    {
        std::ofstream file(path, std::ios::binary);
        assert(file.is_open());
        file << "P5\n960 540\n255\n";
        file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
        file.flush();
        assert(file.good());
    }
};
int main(int argc, char** argv)
{
    using namespace clock_render;
    const auto invalid = content(false, 0, {});
    assert(!invalid.valid && invalid.date[0] == '\0' && std::strcmp(invalid.time, "--:--") == 0);
    const std::int64_t sample_epoch = 1792282080; // 2026-10-18 00:08 UTC, 08:08 China.
    const auto sample = content(true, sample_epoch, clock_policy::chinaTime(sample_epoch));
    assert(std::strcmp(sample.date, "2026年10月18日 星期日") == 0);
    assert(std::strcmp(sample.time, "08:08") == 0);
    assert(changed(invalid, sample));
    auto previous = content(true, sample_epoch + 58, clock_policy::chinaTime(sample_epoch + 58));
    auto next = content(true, sample_epoch + 59, clock_policy::chinaTime(sample_epoch + 59));
    assert(!changed(previous, next));
    next = content(true, sample_epoch + 60, clock_policy::chinaTime(sample_epoch + 60));
    assert(changed(previous, next)); // Wall-minute boundary, not 60 s since entry.
    assert(changed(next, previous)); // Backward synchronization redraws immediately.
    assert(changed(sample, invalid));
    assert(!changed(invalid, invalid));
    const auto before_midnight = content(true, 1792339199, clock_policy::chinaTime(1792339199));
    const auto after_midnight = content(true, 1792339200, clock_policy::chinaTime(1792339200));
    assert(std::strcmp(before_midnight.date, after_midnight.date) != 0);
    assert(changed(before_midnight, after_midnight));
    for (int minute = 0; minute < 1440; ++minute) {
        std::tm tm = {};
        tm.tm_year = 126; tm.tm_mon = 9; tm.tm_mday = 18; tm.tm_wday = 0;
        tm.tm_hour = minute / 60; tm.tm_min = minute % 60;
        const auto value = content(true, minute * 60, tm);
        const auto box = bounds(clock_fonts::TIME, value.time);
        assert(box.width <= 738 && box.height <= TIME_HEIGHT);
        Canvas canvas;
        drawFrame(canvas, value); // Every time exercises production asset decoding/bounds.
        for (const auto p : canvas.pixels) assert(p % 17 == 0);
    }
    for (const char* text : {"2026年1月1日 星期一", "2103年12月31日 星期六"}) {
        const auto box = bounds(clock_fonts::DATE, text);
        assert(box.width < WIDTH - 2 * MARGIN && box.height <= DATE_HEIGHT);
        const char* cursor = text;
        while (*cursor) assert(glyph(clock_fonts::DATE, nextCodepoint(cursor)) != nullptr);
    }
    Canvas sample_canvas;
    drawFrame(sample_canvas, sample);
    Canvas invalid_canvas;
    drawFrame(invalid_canvas, invalid);
    for (int y = 0; y < 100; ++y) for (int x = 0; x < 960; ++x)
        assert(invalid_canvas.pixels[y * 960 + x] == 255); // No fabricated date.
    // All ink belongs to exactly the date, center time, or footer rectangles.
    for (int y = 0; y < 540; ++y) for (int x = 0; x < 960; ++x) {
        if (sample_canvas.pixels[y * 960 + x] == 255) continue;
        assert(x >= 32 && x < 928 && y >= 32 && y < 508);
        assert((y >= 32 && y < 76) || (y >= TIME_TOP && y < TIME_TOP + TIME_HEIGHT) ||
               (y >= 470 && x > 700));
    }
    if (argc > 1) {
        sample_canvas.save(std::string(argv[1]) + "/clock-sample.pgm");
        invalid_canvas.save(std::string(argv[1]) + "/clock-unsynchronized.pgm");
    }
    std::cout << "PASS: production 4-bit renderer, all1440 times, Chinese/date/footer bounds, invalid state and minute/backward/day transitions\n";
}
