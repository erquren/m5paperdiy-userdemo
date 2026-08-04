/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <hal.h>

class ButtonFeedback {
public:
    bool begin();

private:
    bool update(bool force_draw = false);
    void refreshUpdate();
    static void feedbackTask(void* argument);

    bool _initialized             = false;
    bool _button_a_pressed        = false;
    bool _button_b_pressed        = false;
    unsigned int _updated_buttons = 0;
};
