/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"

#include <driver/gpio.h>
#include <freertos/task.h>

namespace {

constexpr gpio_num_t BUTTON_A_PIN              = GPIO_NUM_4;
constexpr gpio_num_t BUTTON_B_PIN              = GPIO_NUM_3;
constexpr std::uint8_t BUTTON_A_MASK           = 1U << 0;
constexpr std::uint8_t BUTTON_B_MASK           = 1U << 1;
constexpr std::uint32_t BUTTON_POLL_MS         = 2;
constexpr std::uint32_t BUTTON_DEBOUNCE_MS     = 10;
constexpr std::uint32_t BUTTON_ACTION_DELAY_MS = 800;
constexpr std::uint32_t BUTTON_A_LONG_PRESS_MS = 3000;
// A B triple is three short, B-only clicks. All times use debounced edges.
// The existing 800-ms single-action delay covers the entire ambiguity window.
constexpr std::uint32_t BUTTON_B_CLICK_MAX_MS  = 400;
constexpr std::uint32_t BUTTON_B_CLICK_GAP_MS  = 400;
constexpr std::uint32_t BUTTON_B_TRIPLE_MAX_MS = 800;
constexpr std::uint8_t BUTTON_ACTION_NONE      = 0;
constexpr std::uint8_t BUTTON_ACTION_A         = 1;
constexpr std::uint8_t BUTTON_ACTION_B         = 2;
constexpr std::uint8_t BUTTON_ACTION_A_LONG    = 3;
constexpr std::uint8_t BUTTON_ACTION_B_TRIPLE   = 4;

std::uint8_t readButtonBits()
{
    return (gpio_get_level(BUTTON_A_PIN) == 0 ? BUTTON_A_MASK : 0) |
           (gpio_get_level(BUTTON_B_PIN) == 0 ? BUTTON_B_MASK : 0);
}

}  // namespace

/* -------------------------------------------------------------------------- */
/*                                Button Input                                */
/* -------------------------------------------------------------------------- */
bool Hal::initButtonInput()
{
    _button_state_bits.store(readButtonBits(), std::memory_order_release);
    const BaseType_t created =
        xTaskCreatePinnedToCore(buttonInputTask, "button_input", 2048, this, tskIDLE_PRIORITY + 4, nullptr, 1);
    return created == pdPASS;
}

void Hal::buttonInputTask(void* argument)
{
    auto* hal                     = static_cast<Hal*>(argument);
    std::uint8_t stable_bits      = readButtonBits();
    std::uint8_t candidate_bits   = stable_bits;
    TickType_t candidate_since    = xTaskGetTickCount();
    bool chord_active             = (stable_bits & (BUTTON_A_MASK | BUTTON_B_MASK)) == (BUTTON_A_MASK | BUTTON_B_MASK);
    TickType_t a_pressed_since    = candidate_since;
    bool a_long_eligible          = stable_bits == BUTTON_A_MASK;
    bool a_long_triggered         = false;
    TickType_t b_pressed_since    = candidate_since;
    TickType_t b_first_clicked_at = candidate_since;
    TickType_t b_last_released_at = candidate_since;
    TickType_t b_idle_since       = candidate_since;
    std::uint8_t b_click_count    = 0;
    // A button already held at startup is not a completed short-click gesture.
    bool b_short_eligible         = false;
    bool b_train_suppressed       = false;
    hal->_button_state_bits.store(stable_bits, std::memory_order_release);

    while (true) {
        const TickType_t now        = xTaskGetTickCount();
        const std::uint8_t raw_bits = readButtonBits();
        if (hal->_shutdown_in_progress.load(std::memory_order_acquire)) {
            stable_bits        = raw_bits;
            candidate_bits     = raw_bits;
            candidate_since    = now;
            chord_active       = false;
            a_long_eligible    = false;
            b_short_eligible   = false;
            b_click_count      = 0;
            b_train_suppressed = false;
            hal->_button_state_bits.store(stable_bits, std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
            continue;
        }
        // Once a triple has fired, absorb a fourth (or later) rapid click.
        // Re-arm only after 400 ms with both buttons released. Keep this guard
        // across application suppression so a mode change cannot leak its tail.
        if (b_train_suppressed) {
            if (stable_bits == 0 && now - b_idle_since >= pdMS_TO_TICKS(BUTTON_B_CLICK_GAP_MS)) {
                b_train_suppressed = false;
            } else if (stable_bits != 0 || raw_bits != 0) {
                b_idle_since = now;
            }
        }
        if (raw_bits != candidate_bits) {
            candidate_bits  = raw_bits;
            candidate_since = now;
        } else if (candidate_bits != stable_bits && now - candidate_since >= pdMS_TO_TICKS(BUTTON_DEBOUNCE_MS)) {
            const std::uint8_t previous_bits = stable_bits;
            const std::uint8_t changed_bits  = stable_bits ^ candidate_bits;
            stable_bits                      = candidate_bits;
            hal->_button_state_bits.store(stable_bits, std::memory_order_release);

            const std::uint8_t both_buttons = BUTTON_A_MASK | BUTTON_B_MASK;
            if ((changed_bits & stable_bits & BUTTON_A_MASK) != 0) {
                a_pressed_since  = now;
                a_long_triggered = false;
                a_long_eligible  = stable_bits == BUTTON_A_MASK && !chord_active;
                b_click_count    = 0;
                b_short_eligible = false;
            }
            if ((changed_bits & stable_bits & BUTTON_B_MASK) != 0) {
                b_pressed_since  = now;
                b_short_eligible = stable_bits == BUTTON_B_MASK && !chord_active;
            }
            if ((stable_bits & both_buttons) == both_buttons) {
                chord_active     = true;
                a_long_eligible  = false;
                b_click_count    = 0;
                b_short_eligible = false;
                // A shutdown chord takes priority over unconsumed mode gestures.
                portENTER_CRITICAL(&hal->_button_action_mux);
                if (hal->_pending_button_action == BUTTON_ACTION_A_LONG ||
                    hal->_pending_button_action == BUTTON_ACTION_B_TRIPLE) {
                    hal->_pending_button_action = BUTTON_ACTION_NONE;
                }
                portEXIT_CRITICAL(&hal->_button_action_mux);
            }

            const std::uint8_t released_bits = changed_bits & previous_bits;
            if ((released_bits & BUTTON_A_MASK) != 0) {
                a_long_eligible = false;
            }
            if (released_bits != 0) {
                if (chord_active) {
                    if ((stable_bits & both_buttons) == 0) {
                        if ((released_bits & BUTTON_B_MASK) != 0) {
                            hal->scheduleButtonAction(BUTTON_ACTION_B);
                        } else if (!a_long_triggered) {
                            hal->scheduleButtonAction(BUTTON_ACTION_A);
                        }
                        chord_active = false;
                    }
                } else {
                    if ((released_bits & BUTTON_A_MASK) != 0 && !a_long_triggered) {
                        hal->scheduleButtonAction(BUTTON_ACTION_A);
                    }
                    if ((released_bits & BUTTON_B_MASK) != 0) {
                        const bool short_click = b_short_eligible && stable_bits == 0 &&
                                                 now - b_pressed_since <= pdMS_TO_TICKS(BUTTON_B_CLICK_MAX_MS);
                        const bool suppressed =
                            hal->_suppress_button_actions_until_release.load(std::memory_order_acquire);
                        if (suppressed || b_train_suppressed) {
                            b_click_count = 0;
                        } else if (short_click) {
                            if (b_click_count == 0 ||
                                now - b_last_released_at > pdMS_TO_TICKS(BUTTON_B_CLICK_GAP_MS) ||
                                now - b_first_clicked_at > pdMS_TO_TICKS(BUTTON_B_TRIPLE_MAX_MS)) {
                                b_click_count      = 0;
                                b_first_clicked_at = now;
                            }
                            if (++b_click_count == 3) {
                                hal->scheduleButtonAction(BUTTON_ACTION_B_TRIPLE);
                                b_click_count      = 0;
                                b_train_suppressed = true;
                                b_idle_since       = now;
                            } else {
                                // One or two clicks retain the original coalesced
                                // single-B action after 800 ms from the last release.
                                hal->scheduleButtonAction(BUTTON_ACTION_B);
                            }
                        } else {
                            b_click_count = 0;
                            hal->scheduleButtonAction(BUTTON_ACTION_B);
                        }
                    }
                }
            }
            if ((released_bits & BUTTON_B_MASK) != 0) {
                b_last_released_at = now;
                b_short_eligible   = false;
            }
        }

        if (hal->_suppress_button_actions_until_release.load(std::memory_order_acquire)) {
            a_long_eligible  = false;
            b_short_eligible = false;
            b_click_count    = 0;
            if (stable_bits == 0 && raw_bits == 0) {
                hal->_suppress_button_actions_until_release.store(false, std::memory_order_release);
            }
        } else if (a_long_eligible && stable_bits == BUTTON_A_MASK && raw_bits == BUTTON_A_MASK &&
                   now - a_pressed_since >= pdMS_TO_TICKS(BUTTON_A_LONG_PRESS_MS)) {
            a_long_eligible  = false;
            a_long_triggered = true;
            portENTER_CRITICAL(&hal->_button_action_mux);
            if (!hal->_shutdown_in_progress.load(std::memory_order_acquire) &&
                !hal->_suppress_button_actions_until_release.load(std::memory_order_acquire)) {
                hal->_pending_button_action = BUTTON_ACTION_A_LONG;
            }
            portEXIT_CRITICAL(&hal->_button_action_mux);
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }
}

/* -------------------------------------------------------------------------- */
/*                                Button State                                */
/* -------------------------------------------------------------------------- */
bool Hal::isButtonPressed(ButtonId button) const
{
    const std::uint8_t mask = button == ButtonId::A ? BUTTON_A_MASK : BUTTON_B_MASK;
    return (_button_state_bits.load(std::memory_order_acquire) & mask) != 0;
}

bool Hal::isButtonInteractionActive()
{
    if (_button_state_bits.load(std::memory_order_acquire) != 0) {
        return true;
    }

    portENTER_CRITICAL(&_button_action_mux);
    const bool active = _pending_button_action != BUTTON_ACTION_NONE;
    portEXIT_CRITICAL(&_button_action_mux);
    return active;
}

/* -------------------------------------------------------------------------- */
/*                               Button Actions                               */
/* -------------------------------------------------------------------------- */
void Hal::scheduleButtonAction(std::uint8_t action)
{
    if (_shutdown_in_progress.load(std::memory_order_acquire)) {
        return;
    }

    portENTER_CRITICAL(&_button_action_mux);
    if (!_shutdown_in_progress.load(std::memory_order_acquire) &&
        !_suppress_button_actions_until_release.load(std::memory_order_acquire)) {
        // A delayed consumer must still receive an already recognized triple,
        // even if another B train starts before it gets CPU time.
        if (_pending_button_action != BUTTON_ACTION_B_TRIPLE) {
            _pending_button_action  = action;
            _button_action_deadline =
                xTaskGetTickCount() + (action == BUTTON_ACTION_B_TRIPLE ? 0 : pdMS_TO_TICKS(BUTTON_ACTION_DELAY_MS));
        }
    }
    portEXIT_CRITICAL(&_button_action_mux);
}

bool Hal::consumeButtonAction(std::uint8_t expected_action)
{
    if (_shutdown_in_progress.load(std::memory_order_acquire)) {
        return false;
    }

    bool clicked = false;

    portENTER_CRITICAL(&_button_action_mux);
    if (!_shutdown_in_progress.load(std::memory_order_acquire) &&
        !_suppress_button_actions_until_release.load(std::memory_order_acquire) &&
        _pending_button_action == expected_action && _button_state_bits.load(std::memory_order_relaxed) == 0 &&
        static_cast<std::int32_t>(xTaskGetTickCount() - _button_action_deadline) >= 0) {
        _pending_button_action = BUTTON_ACTION_NONE;
        clicked                = true;
    }
    portEXIT_CRITICAL(&_button_action_mux);
    return clicked;
}

bool Hal::wasButtonAClicked()
{
    return consumeButtonAction(BUTTON_ACTION_A);
}

bool Hal::wasButtonBClicked()
{
    return consumeButtonAction(BUTTON_ACTION_B);
}

bool Hal::wasButtonBTripleClicked()
{
    return consumeButtonAction(BUTTON_ACTION_B_TRIPLE);
}

bool Hal::wasButtonALongPressed()
{
    bool pressed = false;
    portENTER_CRITICAL(&_button_action_mux);
    if (!_shutdown_in_progress.load(std::memory_order_acquire) &&
        !_suppress_button_actions_until_release.load(std::memory_order_acquire) &&
        (_button_state_bits.load(std::memory_order_relaxed) & BUTTON_B_MASK) == 0 &&
        _pending_button_action == BUTTON_ACTION_A_LONG) {
        _pending_button_action = BUTTON_ACTION_NONE;
        pressed                = true;
    }
    portEXIT_CRITICAL(&_button_action_mux);
    return pressed;
}

void Hal::suppressButtonActionsUntilRelease()
{
    portENTER_CRITICAL(&_button_action_mux);
    _suppress_button_actions_until_release.store(true, std::memory_order_release);
    _pending_button_action = BUTTON_ACTION_NONE;
    portEXIT_CRITICAL(&_button_action_mux);
}

/* -------------------------------------------------------------------------- */
/*                                  Shutdown                                  */
/* -------------------------------------------------------------------------- */
void Hal::beginShutdown()
{
    portENTER_CRITICAL(&_button_action_mux);
    _shutdown_in_progress.store(true, std::memory_order_release);
    _suppress_button_actions_until_release.store(true, std::memory_order_release);
    _pending_button_action = BUTTON_ACTION_NONE;
    portEXIT_CRITICAL(&_button_action_mux);
}

bool Hal::isShutdownInProgress() const
{
    return _shutdown_in_progress.load(std::memory_order_acquire);
}
