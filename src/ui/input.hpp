/*
 * mayhem-b200 — host input state.
 *
 * The PortaPack reads its five-way switch, rotary encoder and touch panel
 * through a CPLD and an IRQ handler. On the host those events arrive from the
 * Win32 message pump; this module holds the small amount of shared state the
 * ported UI code queries directly (notably long-press detection).
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version. See LICENSE.GPL-2.0-or-later.
 */

#ifndef __HOST_INPUT_H__
#define __HOST_INPUT_H__

#include "ui.hpp"

#include <cstdint>

namespace input {

/* Milliseconds a key must be held before it counts as a long press. Matches the
 * firmware's switch debounce/long-press threshold closely enough that ported
 * apps behave the same. */
constexpr uint32_t long_press_ms = 500;

/* Called by the window layer on key transitions. */
void note_key_down(ui::KeyEvent key, uint32_t now_ms);
void note_key_up(ui::KeyEvent key);

/* True while `key` is held and has been held for at least long_press_ms. */
bool key_is_long_pressed(ui::KeyEvent key);

/* True while `key` is physically down. */
bool key_is_down(ui::KeyEvent key);

/* Monotonic milliseconds since process start. */
uint32_t now_ms();

void reset();

}  // namespace input

#endif /*__HOST_INPUT_H__*/
