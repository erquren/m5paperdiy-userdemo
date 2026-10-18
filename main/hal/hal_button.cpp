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
constexpr std::uint8_t BUTTON_ACTION_NONE      = 0;
constexpr std::uint8_t BUTTON_ACTION_A         = 1;
constexpr std::uint8_t BUTTON_ACTION_B         = 2;
constexpr std::uint8_t BUTTON_ACTION_A_LONG    = 3;

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
    auto* hal                   = static_cast<Hal*>(argument);
    std::uint8_t stable_bits    = readButtonBits();
    std::uint8_t candidate_bits = stable_bits;
    TickType_t candidate_since  = xTaskGetTickCount();
    bool chord_active           = (stable_bits & (BUTTON_A_MASK | BUTTON_B_MASK)) == (BUTTON_A_MASK | BUTTON_B_MASK);
    TickType_t a_pressed_since   = candidate_since;
    bool a_long_eligible         = stable_bits == BUTTON_A_MASK;
    bool a_long_triggered        = false;
    hal->_button_state_bits.store(stable_bits, std::memory_order_release);

    while (true) {
        const TickType_t now        = xTaskGetTickCount();
        const std::uint8_t raw_bits = readButtonBits();
        if (hal->_shutdown_in_progress.load(std::memory_order_acquire)) {
            stable_bits     = raw_bits;
            candidate_bits  = raw_bits;
            candidate_since = now;
            chord_active    = false;
            a_long_eligible = false;
            hal->_button_state_bits.store(stable_bits, std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
            continue;
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
            }
            if ((stable_bits & both_buttons) == both_buttons) {
                chord_active    = true;
                a_long_eligible = false;
                // A shutdown chord takes priority over an unconsumed long-A event.
                portENTER_CRITICAL(&hal->_button_action_mux);
                if (hal->_pending_button_action == BUTTON_ACTION_A_LONG) {
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
                            hal->scheduleButtonAction(ButtonId::B);
                        } else if (!a_long_triggered) {
                            hal->scheduleButtonAction(ButtonId::A);
                        }
                        chord_active = false;
                    }
                } else {
                    if ((released_bits & BUTTON_A_MASK) != 0 && !a_long_triggered) {
                        hal->scheduleButtonAction(ButtonId::A);
                    }
                    if ((released_bits & BUTTON_B_MASK) != 0) {
                        hal->scheduleButtonAction(ButtonId::B);
                    }
                }
            }
        }

        if (hal->_suppress_button_actions_until_release.load(std::memory_order_acquire)) {
            a_long_eligible = false;
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
void Hal::scheduleButtonAction(ButtonId button)
{
    if (_shutdown_in_progress.load(std::memory_order_acquire)) {
        return;
    }

    portENTER_CRITICAL(&_button_action_mux);
    if (!_shutdown_in_progress.load(std::memory_order_acquire) &&
        !_suppress_button_actions_until_release.load(std::memory_order_acquire)) {
        _pending_button_action  = button == ButtonId::A ? BUTTON_ACTION_A : BUTTON_ACTION_B;
        _button_action_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(BUTTON_ACTION_DELAY_MS);
    }
    portEXIT_CRITICAL(&_button_action_mux);
}

bool Hal::consumeButtonAction(ButtonId button)
{
    if (_shutdown_in_progress.load(std::memory_order_acquire)) {
        return false;
    }

    const std::uint8_t expected_action = button == ButtonId::A ? BUTTON_ACTION_A : BUTTON_ACTION_B;
    bool clicked                       = false;

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
    return consumeButtonAction(ButtonId::A);
}

bool Hal::wasButtonBClicked()
{
    return consumeButtonAction(ButtonId::B);
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
