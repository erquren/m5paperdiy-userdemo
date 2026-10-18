#pragma once
struct M5GFX {};
struct M5Unified { M5GFX Display; };
extern M5Unified M5;
namespace m5gfx { enum class epd_mode_t { epd_text, epd_quality }; }
