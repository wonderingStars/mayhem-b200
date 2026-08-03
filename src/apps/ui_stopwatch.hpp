/*
 * mayhem-b200 — Stopwatch utility.
 *
 * Host port of PortaPack Mayhem's external/stopwatch app (Copyright 2025 Mark
 * Thompson, copyleft 2025 zxkmm). The firmware read ChibiOS ticks (chTimeNow(),
 * CH_FREQUENCY per second) and rendered each digit by hand to dodge the slow SPI
 * LCD's flicker. On the host the elapsed time is measured with a monotonic
 * std::chrono::steady_clock and the readout is a normal full-view repaint. The
 * time-formatting logic lives in stopwatch_fmt so it can be unit tested against
 * injected durations rather than by waiting on a real clock.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_STOPWATCH_H__
#define __MB200_UI_STOPWATCH_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <string>

namespace app {

/* ---- pure elapsed-time formatting (tested in test_stopwatch.cpp) ---------- */
namespace stopwatch_fmt {

struct HMSC {
    uint32_t hours;
    uint32_t minutes;   /* 0..59 */
    uint32_t seconds;   /* 0..59 */
    uint32_t centis;    /* 0..99, hundredths of a second (truncated) */
};

/* Split a duration in milliseconds into hours/minutes/seconds/centiseconds. */
HMSC decompose_ms(uint64_t ms);

/* "MM:SS.CC" under an hour; "H:MM:SS.CC" once the hour rolls over. */
std::string format_elapsed(uint64_t ms);

/* A lap delta, e.g. "+00:13.34" (always non-negative). */
std::string format_lap_delta(uint64_t delta_ms);

}  // namespace stopwatch_fmt

class StopwatchView : public ui::View {
   public:
    StopwatchView();

    std::string title() const override { return "Stopwatch"; }

    void focus() override;
    void on_show() override;
    void on_frame_sync() override;
    void paint(ui::Painter& painter) override;

   private:
    void toggle_run();
    void run();
    void stop();
    void lap();
    void reset();
    uint64_t elapsed_now() const;

    bool running_{false};
    uint64_t accumulated_ms_{0};   /* elapsed carried across stop/start */
    uint64_t start_ms_{0};         /* monotonic time the current run began */
    uint64_t last_lap_total_ms_{0};
    uint32_t lap_count_{0};
    std::string displayed_{};      /* current big-readout text */

    ui::Labels labels_{
        {{8, 8}, "TOTAL", ui::Color::light_grey()},
        {{8, 74}, "LAPS", ui::Color::light_grey()},
    };

    ui::Console laps_{{0, 94, 240, 108}};

    ui::Button button_run_stop_{{16, 212, 96, 28}, "START"};
    ui::Button button_reset_lap_{{128, 212, 96, 28}, "RESET"};
    ui::Button button_exit_{{72, 250, 96, 28}, "EXIT"};
};

}  // namespace app

#endif /*__MB200_UI_STOPWATCH_H__*/
