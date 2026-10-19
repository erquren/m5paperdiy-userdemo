/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <M5Unified.hpp>
#include <atomic>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdmmc_cmd.h>
#include <string>

enum class ButtonId : std::uint8_t {
    A,
    B,
};

enum class TfCardStatus : std::uint8_t {
    NoCard,
    Mounted,
    MountFailed,
};

struct TfCardInfo {
    TfCardStatus status = TfCardStatus::NoCard;
    std::string type;
    std::string brand;
    std::string message;
    std::uint64_t capacity_bytes = 0;
};

struct TfBenchmarkResult {
    bool success     = false;
    float write_mb_s = 0.0f;
    float read_mb_s  = 0.0f;
    std::string message;
};

class Hal {
public:
    bool init();

    M5GFX& display = M5.Display;

    /* --------------------------------- System --------------------------------- */
    void delay(std::uint32_t ms);
    std::uint32_t millis();
    std::int16_t getBatteryVoltageMv();
    void feedTheDog();

    /* --------------------------------- Button --------------------------------- */
    bool isButtonPressed(ButtonId button) const;
    bool isButtonInteractionActive();
    bool wasButtonAClicked();
    bool wasButtonBClicked();
    // Three B-only clicks: holds <=400 ms, releases <=400 ms apart and <=800 ms
    // total. Suppresses the single-B action and extra clicks until 400 ms idle.
    bool wasButtonBTripleClicked();
    // Consume the one-shot event from holding A alone for three seconds.
    bool wasButtonALongPressed();
    // Clear queued actions and ignore the current press until all buttons are released.
    void suppressButtonActionsUntilRelease();

    /* --------------------------------- TF Card -------------------------------- */
    bool initTfCard();
    bool isTfCardInserted() const;
    const TfCardInfo& updateTfCard(bool retry_mount = false);
    const TfCardInfo& getTfCardInfo() const;
    TfBenchmarkResult benchmarkTfCard();

    /* --------------------------------- Display -------------------------------- */
    void lockDisplay();
    void unlockDisplay();
    void flushDisplay();
    bool flushDisplayRegion(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                            m5gfx::epd_mode_t mode);
    void flushFullDisplay(m5gfx::epd_mode_t mode);
    void requestRefresh();
    void requestRefresh(std::int32_t x, std::int32_t y, std::int32_t width, std::int32_t height,
                        m5gfx::epd_mode_t mode = m5gfx::epd_mode_t::epd_text);
    void requestFullRedraw(m5gfx::epd_mode_t mode = m5gfx::epd_mode_t::epd_quality);
    bool isRefreshRequested() const;
    bool isFullRedrawRequested() const;
    m5gfx::epd_mode_t partialRefreshMode() const;
    m5gfx::epd_mode_t fullRedrawMode() const;
    void clearRefreshRequest();

    /* -------------------------------- Shutdown -------------------------------- */
    void beginShutdown();
    bool isShutdownInProgress() const;

private:
    /* --------------------------------- Button --------------------------------- */
    bool initButtonInput();
    void scheduleButtonAction(std::uint8_t action);
    bool consumeButtonAction(std::uint8_t expected_action);
    static void buttonInputTask(void* argument);

    std::atomic<std::uint8_t> _button_state_bits{0};
    std::atomic<bool> _suppress_button_actions_until_release{false};
    portMUX_TYPE _button_action_mux     = portMUX_INITIALIZER_UNLOCKED;
    std::uint8_t _pending_button_action = 0;
    TickType_t _button_action_deadline  = 0;

    /* --------------------------------- TF Card -------------------------------- */
    bool mountTfCard();
    void unmountTfCard();

    bool _tf_spi_initialized = false;
    sdmmc_card_t* _tf_card   = nullptr;
    TfCardInfo _tf_card_info;

    /* --------------------------------- Display -------------------------------- */
    bool _refresh_request                   = false;
    bool _full_redraw_request               = false;
    bool _partial_refresh_request           = false;
    std::int32_t _refresh_x                 = 0;
    std::int32_t _refresh_y                 = 0;
    std::int32_t _refresh_width             = 0;
    std::int32_t _refresh_height            = 0;
    m5gfx::epd_mode_t _partial_refresh_mode = m5gfx::epd_mode_t::epd_text;
    m5gfx::epd_mode_t _full_redraw_mode     = m5gfx::epd_mode_t::epd_quality;
    std::uint32_t _partial_refresh_count    = 0;
    SemaphoreHandle_t _display_mutex        = nullptr;

    /* -------------------------------- Shutdown -------------------------------- */
    std::atomic<bool> _shutdown_in_progress{false};
};

/* ------------------------------- Display Guard ------------------------------ */
class DisplayGuard {
public:
    explicit DisplayGuard(Hal& hal) : _hal(hal)
    {
        _hal.lockDisplay();
    }

    ~DisplayGuard()
    {
        _hal.unlockDisplay();
    }

    DisplayGuard(const DisplayGuard&)            = delete;
    DisplayGuard& operator=(const DisplayGuard&) = delete;

private:
    Hal& _hal;
};

Hal& GetHAL();
