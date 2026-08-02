/*
 * mayhem-b200 — frequency entry field.
 *
 * Shows a frequency in the firmware's 10-column "MMMM.KKKUUU" form and edits it
 * with the encoder. Select cycles the step size, which is how you get from
 * 1 Hz resolution to 1 MHz jumps without a keypad.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __HOST_UI_FREQ_FIELD_H__
#define __HOST_UI_FREQ_FIELD_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <functional>

namespace ui {

class FrequencyField : public Widget {
   public:
    /* Steps the Select key cycles through, in Hz. */
    static constexpr int64_t steps[] = {1, 10, 100, 1'000, 5'000, 6'250, 10'000,
                                        12'500, 25'000, 100'000, 1'000'000};
    static constexpr size_t step_count = sizeof(steps) / sizeof(steps[0]);

    explicit FrequencyField(Point parent_pos);

    uint64_t value() const { return value_; }
    void set_value(uint64_t hz, bool trigger_change = true);

    void set_range(uint64_t min_hz, uint64_t max_hz);

    int64_t step() const { return steps[step_index_]; }
    void set_step_index(size_t index);
    size_t step_index() const { return step_index_; }

    std::function<void(uint64_t)> on_change{};
    std::function<void()> on_edit{};

    void paint(Painter& painter) override;
    bool on_key(const KeyEvent key) override;
    bool on_encoder(const EncoderEvent delta) override;
    bool on_touch(const TouchEvent event) override;

   private:
    uint64_t value_{100'000'000};
    uint64_t min_{0};
    uint64_t max_{6'000'000'000ull};
    size_t step_index_{8};  /* 25 kHz, the firmware's default */
};

/* One-line readout of the current step size, e.g. "25.0k". */
class FrequencyStepView : public Widget {
   public:
    FrequencyStepView(Point parent_pos, FrequencyField& field);

    void paint(Painter& painter) override;

   private:
    FrequencyField& field_;
};

}  // namespace ui

#endif /*__HOST_UI_FREQ_FIELD_H__*/
