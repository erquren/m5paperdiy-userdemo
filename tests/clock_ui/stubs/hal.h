#pragma once
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

constexpr std::uint16_t TFT_WHITE = 0xffff, TFT_BLACK = 0;
enum Datum {middle_center, top_left};
enum class ButtonId {A, B};
namespace m5gfx { enum class epd_mode_t {epd_quality, epd_text, epd_fastest}; }
class M5GFX {
public:
    std::vector<std::uint16_t> pixels = std::vector<std::uint16_t>(540 * 960, TFT_WHITE);
    int rotation = 0, draws = 0, lock_depth = 0, qr_count = 0;
    int width() const { return rotation & 1 ? 960 : 540; }
    int height() const { return rotation & 1 ? 540 : 960; }
    int getRotation() const { return rotation; }
    void setRotation(int value) { assert(lock_depth == 1); rotation = value; }
    void waitDisplay() {}
    void clearClipRect() {}
    void setClipRect(int, int, int, int) {}
    void setTextColor(int, int) {}
    void setTextDatum(Datum) {}
    void setTextSize(float) {}
    void setFont(const int*) {}
    int textWidth(const char* value) const { return std::strlen(value) * 8; }
    void drawString(const char*, int x, int y) { fillRect(x, y, 1, 1, TFT_BLACK); }
    void qrcode(const char*, int x, int y, int size, int, bool) { ++qr_count; fillRect(x,y,size,size,TFT_BLACK); }
    void readRect(int x, int y, int w, int h, std::uint16_t* target)
    {
        assert(lock_depth == 1 && x == 0 && y == 0 && w == width() && h == height());
        std::copy(pixels.begin(), pixels.end(), target);
    }
    void pushImage(int x, int y, int w, int h, const std::uint16_t* source)
    {
        assert(lock_depth == 1 && x == 0 && y == 0 && w == width() && h == height());
        std::copy(source, source + w * h, pixels.begin()); ++draws;
    }
    void fillRect(int x, int y, int w, int h, std::uint16_t color)
    {
        assert(lock_depth == 1 && x >= 0 && y >= 0 && x+w<=width() && y+h<=height());
        for(int row=y;row<y+h;++row) std::fill(pixels.begin()+row*width()+x,pixels.begin()+row*width()+x+w,color);
        ++draws;
    }
    void fillScreen(std::uint16_t color) { fillRect(0,0,width(),height(),color); }
    void drawFastHLine(int x,int y,int w,std::uint16_t color) {fillRect(x,y,w,1,color);}
    std::uint16_t color565(std::uint8_t r,std::uint8_t g,std::uint8_t b) {return (r>>3)<<11|(g>>2)<<5|(b>>3);}
};
class Hal {
public:
    M5GFX display;
    bool a=false,b=false,short_a=false,short_b=false,triple=false,long_a=false,shutdown=false;
    int suppressions=0,full_refreshes=0,partial_refreshes=0;
    bool isButtonPressed(ButtonId id) const {return id==ButtonId::A?a:b;}
    bool isShutdownInProgress() const {return shutdown;}
    bool wasButtonAClicked() {const bool v=short_a;short_a=false;return v;}
    bool wasButtonBClicked() {const bool v=short_b;short_b=false;return v;}
    bool wasButtonBTripleClicked() {const bool v=triple;triple=false;return v;}
    bool wasButtonALongPressed() {const bool v=long_a;long_a=false;return v;}
    void suppressButtonActionsUntilRelease() {++suppressions;short_a=short_b=triple=long_a=false;}
    void requestFullRedraw(m5gfx::epd_mode_t) {++full_refreshes;}
    void requestRefresh(int,int,int,int,m5gfx::epd_mode_t) {++partial_refreshes;}
};
Hal& GetHAL();
class DisplayGuard {
    Hal& hal;
public:
    explicit DisplayGuard(Hal& h):hal(h) {assert(hal.display.lock_depth++==0);}
    ~DisplayGuard() {assert(--hal.display.lock_depth==0);}
};
