/*
 * mayhem-b200 — frequency entry field.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_freq_field.hpp"

#include "../core/string_format.hpp"
#include "theme.hpp"

#include <algorithm>

namespace ui {

constexpr int64_t FrequencyField::steps[];

FrequencyField::FrequencyField(Point parent_pos)
    : Widget{{parent_pos, {10 * 8, 16}}} {
    set_focusable(true);
}

void FrequencyField::set_value(uint64_t hz, bool trigger_change) {
    const uint64_t clamped = std::clamp(hz, min_, max_);
    if (clamped == value_) return;

    value_ = clamped;
    set_dirty();
    if (trigger_change && on_change) on_change(value_);
}

void FrequencyField::set_range(uint64_t min_hz, uint64_t max_hz) {
    min_ = min_hz;
    max_ = max_hz;
    set_value(value_, false);
}

void FrequencyField::set_step_index(size_t index) {
    if (index >= step_count) index = 0;
    if (index == step_index_) return;
    step_index_ = index;
    set_dirty();
}

void FrequencyField::paint(Painter& painter) {
    const auto r = screen_rect();
    const Style s = (has_focus() || highlighted()) ? style().invert() : style();

    painter.fill_rectangle(r, s.background);
    painter.draw_string(r.location(), s, to_string_freq(value_));
}

bool FrequencyField::on_key(const KeyEvent key) {
    if (key == KeyEvent::Select) {
        set_step_index((step_index_ + 1) % step_count);
        if (on_edit) on_edit();
        return true;
    }
    return false;
}

bool FrequencyField::on_encoder(const EncoderEvent delta) {
    const int64_t change = static_cast<int64_t>(delta) * step();
    const int64_t current = static_cast<int64_t>(value_);

    /* Compute in signed space so a downward step past zero saturates at min_
     * rather than wrapping to 18 exahertz. */
    int64_t next = current + change;
    if (next < static_cast<int64_t>(min_)) next = static_cast<int64_t>(min_);
    if (next > static_cast<int64_t>(max_)) next = static_cast<int64_t>(max_);

    set_value(static_cast<uint64_t>(next));
    return true;
}

bool FrequencyField::on_touch(const TouchEvent event) {
    if (event.type == TouchEvent::Type::Start) {
        focus();
        return true;
    }
    return false;
}

/* --- FrequencyStepView ----------------------------------------------------- */

FrequencyStepView::FrequencyStepView(Point parent_pos, FrequencyField& field)
    : Widget{{parent_pos, {6 * 8, 16}}},
      field_{field} {
}

void FrequencyStepView::paint(Painter& painter) {
    const auto r = screen_rect();
    const auto& s = style();

    painter.fill_rectangle(r, s.background);

    const auto text = unit_auto_scale(static_cast<double>(field_.step()), 4, 1);
    painter.draw_string(r.location(), s.font,
                        Theme::getInstance()->fg_cyan->foreground, s.background, text);
}

}  // namespace ui
