/*
 * mayhem-b200 — Analog TV receiver.
 *
 * Ported from firmware/application/external/analogtv/ (analog_tv_app.*, ui_tv.*),
 * originally by Shao (2020) on top of Jared Boone's / Furrtek's receiver shell.
 *
 * What upstream did, and what this port keeps
 * -------------------------------------------
 * The PortaPack version tunes an AM-video channel at a 2 MHz sample rate and
 * runs a dedicated M4 baseband image ("PAMT") that computes the video AM
 * envelope and streams it back in 256-sample chunks (two 128-pixel scan lines).
 * The app draws those chunks straight to the LCD with display.render_line(),
 * doubling each line vertically and offsetting horizontally by a manual
 * "x-correction" so the picture sits centred. It does no real horizontal-sync
 * locking — it renders a fixed number of samples per line and lets the operator
 * nudge the offset by hand. See the long comment in upstream ui_tv.cpp.
 *
 * On the host there is no M4 baseband. The envelope is computed here from the
 * raw wideband IQ the ReceiverModel already taps for its spectrum display: the
 * AM video envelope is simply |IQ| per sample. That envelope is then framed into
 * scan lines and rendered to host::display, exactly the shape of upstream's
 * pipeline. Two things are added over upstream because the task calls for them
 * and they are genuinely portable logic:
 *
 *   1. samples_per_line() — the line-timing maths, from the capture rate and the
 *      broadcast line rate (PAL 15625 Hz, NTSC 15734.264 Hz). Upstream hard-coded
 *      "128 samples == 2 lines" for its fixed 2 MHz rate; here it is derived so
 *      any capture rate works.
 *   2. detect_hsync() — real horizontal-sync-pulse detection on the envelope, so
 *      the frame can auto-align to the sync tips instead of only a manual offset.
 *
 * Honesty
 * -------
 * No hardware is attached during development, so nothing here has been proven
 * against a real off-air video signal — the maths and the framing are unit
 * tested, the RF end is not. The colour/luma decoding of a full PAL/NTSC frame
 * (colour burst, interlace, equalising pulses) is NOT implemented; like upstream
 * this shows a monochrome, roughly-framed envelope image, which is enough to see
 * a video signal is present and to line it up, and no more.
 *
 * Copyright (C) 2020 Shao (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_ANALOG_TV_H__
#define __MB200_UI_ANALOG_TV_H__

#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace app {
namespace analogtv {

/* --- Line-timing maths (pure, unit-tested) --------------------------------- */

/* Horizontal line rate (== horizontal sync frequency) of the two broadcast
 * families, in Hz. PAL/SECAM: 625 lines at 25 frames -> 15625 Hz exactly.
 * NTSC (M): 525 lines at 30/1.001 frames -> 15734.264 Hz. */
constexpr double kLineRatePAL = 15625.0;
constexpr double kLineRateNTSC = 15734.264;

/* Horizontal sync pulse width, seconds. Both families use ~4.7 us. */
constexpr double kHSyncWidthSec = 4.7e-6;

/* Samples per full line (active video + blanking) at a given capture rate.
 * This is the upstream "how many samples in a line" figure, generalised: at
 * 2 MHz / PAL it returns exactly 128, matching upstream's hard-coded value. */
inline double samples_per_line(double sample_rate_hz, double line_rate_hz) {
    if (line_rate_hz <= 0.0 || sample_rate_hz <= 0.0) return 0.0;
    return sample_rate_hz / line_rate_hz;
}

/* Minimum run length, in samples, to accept a dip as a horizontal-sync pulse at
 * a given capture rate. Uses 60% of the nominal 4.7 us so channel filtering and
 * a slightly-off capture rate don't cause misses. Always at least 1. */
inline size_t hsync_min_samples(double sample_rate_hz) {
    const double s = kHSyncWidthSec * 0.6 * sample_rate_hz;
    if (s <= 1.0) return 1;
    return static_cast<size_t>(s + 0.5);
}

/* Detects horizontal-sync pulses in a *display-polarity* envelope, i.e. one in
 * which the sync tip is the LOW extreme (video = 1 - normalised carrier
 * envelope, because broadcast TV is negatively modulated: sync sits at peak
 * carrier). A sync pulse is a run of at least `min_run` consecutive samples
 * whose value is <= `threshold`. Returns the index of the first sample (the
 * leading edge) of each such run. Adjacent samples inside one run collapse to a
 * single detection.
 *
 * This is deliberately simple and standard-agnostic — it locks to the line
 * cadence without needing to know PAL vs NTSC — which is what the framer uses to
 * auto-align. */
std::vector<size_t> detect_hsync(const float* env, size_t n,
                                 float threshold, size_t min_run);

/* --- Line framer (pure, unit-tested) --------------------------------------- */

/* Slices a continuous envelope stream into scan lines of length ~spl, honouring
 * a fractional samples-per-line so a non-integer rate (e.g. NTSC at 2 MHz =
 * 127.11) does not drift over a frame. An initial x-correction drops that many
 * samples once, the host equivalent of upstream's field_xcorr offset. Each
 * complete line is handed to a callback; partial data is retained for the next
 * feed(). */
class LineFramer {
   public:
    /* spl is samples-per-line (may be fractional); x_correction shifts the very
     * first line boundary, in samples. Resets any buffered data. */
    void configure(double spl, size_t x_correction);
    void reset();

    /* Nominal integer pixels per emitted line (rounds spl). */
    size_t nominal_line_length() const;

    /* Appends `n` samples and emits every complete line via `emit(ptr,len)`. */
    void feed(const float* env, size_t n,
              const std::function<void(const float*, size_t)>& emit);

    /* The exact length the *next* emitted line will take, given the fractional
     * accumulator. Exposed for testing the anti-drift behaviour. */
    size_t peek_next_length() const;

   private:
    void advance_frac();

    double spl_{128.0};
    size_t x_correction_{0};
    double frac_{0.0};
    bool primed_{false};
    std::vector<float> buf_{};
};

/* Maps one line of envelope samples (display polarity, arbitrary length) to a
 * row of `width` grayscale pixels by nearest-neighbour resampling, scaling the
 * [lo, hi] envelope range to full black..white. Writes `width` colours. */
void line_to_pixels(const float* env, size_t len, float lo, float hi,
                    ui::Color* out, size_t width);

/* --- The on-screen video area --------------------------------------------- *
 *
 * A widget that owns a rectangle of the screen and paints scan lines into it by
 * drawing straight to host::display, the same trick WaterfallWidget uses: its
 * paint() is a no-op so the widget tree never erases the picture, and rows are
 * pushed in via render_line(). */
class TvScreen : public ui::Widget {
   public:
    explicit TvScreen(ui::Rect parent_rect);

    void paint(ui::Painter&) override {}  /* drawn directly, not via the tree */
    void on_show() override;
    void on_hide() override;

    void clear();

    /* Draws one grayscale line at the current row and advances; wraps to the top
     * of the area at the bottom, which is the frame reset. */
    void push_line(const ui::Color* pixels, size_t width);

    /* Restart at the top of the image (vertical retrace). */
    void new_frame() { row_ = 0; }

   private:
    ui::Coord row_{0};
};

/* --- The app view ---------------------------------------------------------- */

class AnalogTvView : public ui::View {
   public:
    AnalogTvView();
    ~AnalogTvView() override;

    std::string title() const override { return "Analog TV"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

    bool on_key(const ui::KeyEvent key) override;
    bool on_encoder(const ui::EncoderEvent delta) override;

   private:
    void reconfigure_timing();
    void render_available_samples();

    radio::ReceiverModel& receiver_;
    radio::ReceiverModel::Mode previous_mode_{};

    static constexpr ui::Coord kHeaderHeight = 3 * 16;

    /* --- controls --- */
    ui::Labels labels_{
        {{0, 4}, "Freq", ui::Color::light_grey()},
        {{0, 22}, "Gain", ui::Color::light_grey()},
        {{120, 22}, "Std", ui::Color::light_grey()},
        {{0, 40}, "Xcorr", ui::Color::light_grey()},
        {{120, 40}, "Sync", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 4}};
    ui::NumberField field_gain_{{40, 22}, 3, {0, 76}, 1, ' '};

    ui::OptionsField options_std_{
        {152, 22},
        4,
        {{"PAL ", 0}, {"NTSC", 1}}};

    ui::NumberField field_xcorr_{{56, 40}, 3, {0, 255}, 1, ' '};
    ui::Checkbox check_autosync_{{152, 36}, 4, "Auto"};

    TvScreen tv_screen_{{0, kHeaderHeight, 240, static_cast<ui::Dim>(304 - kHeaderHeight)}};

    /* Honest overlay shown until real samples arrive. */
    ui::Text text_status_{{0, kHeaderHeight + 4, 240, 16}, ""};

    /* --- framing state --- */
    LineFramer framer_{};
    double line_rate_{kLineRatePAL};
    double sample_rate_{2'000'000.0};
    double sample_rate_saved_{2'400'000.0};

    std::vector<std::complex<float>> samples_{};
    std::vector<float> envelope_{};
    std::vector<ui::Color> line_pixels_{};

    /* Running envelope range for auto-contrast, tracked with slow decay so a
     * bright flash doesn't wash out the following frames. */
    float env_lo_{0.0f};
    float env_hi_{1.0f};

    uint32_t frames_without_data_{0};
    bool ever_had_data_{false};
};

}  // namespace analogtv
}  // namespace app

#endif /*__MB200_UI_ANALOG_TV_H__*/
