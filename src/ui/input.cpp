/*
 * mayhem-b200 — host input state.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version. See LICENSE.GPL-2.0-or-later.
 */

#include "input.hpp"

#include <array>
#include <atomic>
#include <chrono>

namespace input {

namespace {

/* KeyEvent ordinals run 0..6 (Right, Left, Down, Up, Select, Dfu, Back). */
constexpr size_t key_count = 7;

struct KeyState {
    std::atomic<bool> down{false};
    std::atomic<uint32_t> down_at_ms{0};
};

std::array<KeyState, key_count> keys{};

const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

bool valid(ui::KeyEvent key) {
    return static_cast<size_t>(key) < key_count;
}

}  // namespace

uint32_t now_ms() {
    const auto d = std::chrono::steady_clock::now() - start;
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(d).count());
}

void note_key_down(ui::KeyEvent key, uint32_t at_ms) {
    if (!valid(key)) return;
    auto& k = keys[static_cast<size_t>(key)];
    /* Auto-repeat from the OS re-sends WM_KEYDOWN; keep the original timestamp
     * so a held key does not keep resetting its long-press clock. */
    if (!k.down.exchange(true)) {
        k.down_at_ms.store(at_ms);
    }
}

void note_key_up(ui::KeyEvent key) {
    if (!valid(key)) return;
    keys[static_cast<size_t>(key)].down.store(false);
}

bool key_is_down(ui::KeyEvent key) {
    if (!valid(key)) return false;
    return keys[static_cast<size_t>(key)].down.load();
}

bool key_is_long_pressed(ui::KeyEvent key) {
    if (!valid(key)) return false;
    const auto& k = keys[static_cast<size_t>(key)];
    if (!k.down.load()) return false;
    return (now_ms() - k.down_at_ms.load()) >= long_press_ms;
}

void reset() {
    for (auto& k : keys) {
        k.down.store(false);
        k.down_at_ms.store(0);
    }
}

}  // namespace input
