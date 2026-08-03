/*
 * mayhem-b200 — Analog TV receiver (host port).
 *
 * See ui_analog_tv.hpp for the porting notes and the honesty statement.
 *
 * Copyright (C) 2020 Shao (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_analog_tv.hpp"

#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "display.hpp"
#include "ui_painter.hpp"

#include <algorithm>
#include <cmath>

namespace app {
namespace analogtv {

/* --- pure logic ------------------------------------------------------------ */

std::vector<size_t> detect_hsync(const float* env, size_t n,
                                 float threshold, size_t min_run) {
    std::vector<size_t> edges;
    if (env == nullptr || n == 0) return edges;
    if (min_run == 0) min_run = 1;

    size_t run_start = 0;
    size_t run_len = 0;
    bool in_run = false;

    for (size_t i = 0; i < n; ++i) {
        if (env[i] <= threshold) {
            if (!in_run) {
                in_run = true;
                run_start = i;
                run_len = 1;
            } else {
                ++run_len;
            }
        } else {
            if (in_run && run_len >= min_run) edges.push_back(run_start);
            in_run = false;
            run_len = 0;
        }
    }
    /* A run that reaches the end of the buffer still counts. */
    if (in_run && run_len >= min_run) edges.push_back(run_start);

    return edges;
}

void LineFramer::configure(double spl, size_t x_correction) {
    spl_ = spl > 1.0 ? spl : 1.0;
    x_correction_ = x_correction;
    reset();
}

void LineFramer::reset() {
    frac_ = 0.0;
    primed_ = false;
    buf_.clear();
}

size_t LineFramer::nominal_line_length() const {
    return static_cast<size_t>(spl_ + 0.5);
}

size_t LineFramer::peek_next_length() const {
    const size_t base = static_cast<size_t>(spl_);
    const double f = frac_ + (spl_ - static_cast<double>(base));
    size_t len = (f >= 1.0) ? base + 1 : base;
    return len == 0 ? 1 : len;
}

void LineFramer::advance_frac() {
    const size_t base = static_cast<size_t>(spl_);
    frac_ += spl_ - static_cast<double>(base);
    if (frac_ >= 1.0) frac_ -= 1.0;
}

void LineFramer::feed(const float* env, size_t n,
                      const std::function<void(const float*, size_t)>& emit) {
    if (env == nullptr || n == 0) return;
    buf_.insert(buf_.end(), env, env + n);

    if (!primed_) {
        if (buf_.size() <= x_correction_) return;
        buf_.erase(buf_.begin(),
                   buf_.begin() + static_cast<ptrdiff_t>(x_correction_));
        primed_ = true;
    }

    for (;;) {
        const size_t len = peek_next_length();
        if (buf_.size() < len) break;
        if (emit) emit(buf_.data(), len);
        advance_frac();
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<ptrdiff_t>(len));
    }
}

void line_to_pixels(const float* env, size_t len, float lo, float hi,
                    ui::Color* out, size_t width) {
    if (out == nullptr || width == 0) return;
    const float span = (hi - lo) > 1e-9f ? (hi - lo) : 1e-9f;

    for (size_t x = 0; x < width; ++x) {
        size_t src = (len == 0) ? 0 : (x * len) / width;
        if (src >= len && len > 0) src = len - 1;
        float v = (len == 0) ? 0.0f : (env[src] - lo) / span;
        v = std::clamp(v, 0.0f, 1.0f);
        const uint8_t g = static_cast<uint8_t>(v * 255.0f + 0.5f);
        out[x] = ui::Color(g, g, g);
    }
}

/* --- TvScreen -------------------------------------------------------------- */

TvScreen::TvScreen(ui::Rect parent_rect) {
    set_parent_rect(parent_rect);
}

void TvScreen::on_show() {
    row_ = 0;
    clear();
}

void TvScreen::on_hide() {
    /* nothing to tear down; the view above repaints its background */
}

void TvScreen::clear() {
    host::display.fill_rectangle(screen_rect(), ui::Color::black());
}

void TvScreen::push_line(const ui::Color* pixels, size_t width) {
    const auto r = screen_rect();
    if (r.height() <= 0) return;

    const ui::Coord y = static_cast<ui::Coord>(r.top() + row_);
    host::display.render_line({r.left(), y},
                              static_cast<uint16_t>(std::min<size_t>(width, r.width())),
                              pixels);

    row_ = static_cast<ui::Coord>(row_ + 1);
    if (row_ >= r.height()) row_ = 0;  /* frame wrap */
}

/* --- AnalogTvView ---------------------------------------------------------- */

namespace {
constexpr size_t kImageWidth = 240;
constexpr size_t kSamplesPerFrame = 4096;  /* == receiver's spectrum buffer */
/* Sync tip threshold on the [0,1] display-polarity envelope. Sync is the LOW
 * extreme, so anything below this is treated as blanking/sync. */
constexpr float kSyncThreshold = 0.18f;
}  // namespace

AnalogTvView::AnalogTvView()
    : receiver_{*globals().receiver} {
    add_children({&labels_, &field_frequency_, &field_gain_, &options_std_,
                  &field_xcorr_, &check_autosync_, &tv_screen_, &text_status_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.set_step_index(9);  /* 100 kHz — TV channels are wide */
    field_frequency_.on_change = [this](uint64_t hz) {
        receiver_.set_target_frequency(hz);
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    options_std_.set_selected_index(0, false);
    options_std_.on_change = [this](size_t, int32_t v) {
        line_rate_ = (v == 1) ? kLineRateNTSC : kLineRatePAL;
        reconfigure_timing();
    };

    field_xcorr_.set_value(10, false);  /* upstream default */
    field_xcorr_.on_change = [this](int32_t) { reconfigure_timing(); };

    check_autosync_.set_value(true);

    line_pixels_.resize(kImageWidth);
    text_status_.set(STR_COLOR_LIGHT_GREY "Tune a TV channel. Needs a USRP B200.");

    reconfigure_timing();
}

AnalogTvView::~AnalogTvView() {
    receiver_.set_mode(previous_mode_);
    receiver_.set_sampling_rate(sample_rate_saved_);
}

void AnalogTvView::reconfigure_timing() {
    sample_rate_ = receiver_.sampling_rate();
    if (sample_rate_ <= 0.0) sample_rate_ = 2'000'000.0;

    const double spl = samples_per_line(sample_rate_, line_rate_);
    framer_.configure(spl > 1.0 ? spl : 128.0,
                      static_cast<size_t>(field_xcorr_.value()));
}

void AnalogTvView::on_show() {
    View::on_show();
    field_frequency_.focus();

    previous_mode_ = receiver_.mode();
    sample_rate_saved_ = receiver_.sampling_rate();

    /* Raw wideband IQ, no audio demod — the envelope is computed here. 2 MHz is
     * upstream's rate and gives an integer 128 samples/line for PAL. */
    receiver_.set_sampling_rate(2'000'000.0);
    receiver_.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    reconfigure_timing();

    if (!receiver_.running()) receiver_.start();

    tv_screen_.on_show();
}

void AnalogTvView::on_hide() {
    tv_screen_.on_hide();
    View::on_hide();
}

void AnalogTvView::render_available_samples() {
    if (!receiver_.take_spectrum_samples(samples_, kSamplesPerFrame)) {
        if (++frames_without_data_ > 120 && !ever_had_data_) {
            text_status_.hidden(false);
            text_status_.set(STR_COLOR_YELLOW
                             "No IQ. Attach a USRP B200 and tune a TV channel.");
        }
        return;
    }
    if (samples_.empty()) return;

    frames_without_data_ = 0;
    if (!ever_had_data_) {
        ever_had_data_ = true;
        text_status_.hidden(true);
    }

    const size_t n = samples_.size();
    envelope_.resize(n);

    /* AM video envelope = |IQ|, then normalise to display polarity in [0,1]
     * (sync tip -> 0, because broadcast video is negatively modulated). */
    float lo = 1e30f, hi = -1e30f;
    for (size_t i = 0; i < n; ++i) {
        const float m = std::abs(samples_[i]);
        envelope_[i] = m;
        lo = std::min(lo, m);
        hi = std::max(hi, m);
    }
    /* Slow AGC on the contrast window so a transient doesn't wash the picture. */
    env_lo_ = env_lo_ * 0.7f + lo * 0.3f;
    env_hi_ = env_hi_ * 0.7f + hi * 0.3f;
    const float span = (env_hi_ - env_lo_) > 1e-6f ? (env_hi_ - env_lo_) : 1e-6f;

    for (size_t i = 0; i < n; ++i) {
        float v = (envelope_[i] - env_lo_) / span;
        v = std::clamp(v, 0.0f, 1.0f);
        envelope_[i] = 1.0f - v;  /* display polarity: sync low, scene bright */
    }

    auto emit_line = [this](const float* line, size_t len) {
        line_to_pixels(line, len, 0.0f, 1.0f, line_pixels_.data(), kImageWidth);
        tv_screen_.push_line(line_pixels_.data(), kImageWidth);
    };

    if (check_autosync_.value()) {
        /* Frame directly off detected sync tips: each span between consecutive
         * leading edges is one line. */
        const double spl = samples_per_line(sample_rate_, line_rate_);
        const size_t min_run = hsync_min_samples(sample_rate_);
        const auto edges =
            detect_hsync(envelope_.data(), n, kSyncThreshold, min_run);

        if (edges.size() >= 2) {
            for (size_t e = 0; e + 1 < edges.size(); ++e) {
                const size_t start = edges[e];
                const size_t len = edges[e + 1] - start;
                /* Reject spurious edges: a plausible line is within +-50%. */
                if (len < static_cast<size_t>(spl * 0.5) ||
                    len > static_cast<size_t>(spl * 1.5))
                    continue;
                emit_line(envelope_.data() + start, len);
            }
            return;
        }
        /* No lock this block — fall through to fixed-rate framing. */
    }

    framer_.feed(envelope_.data(), n, emit_line);
}

void AnalogTvView::on_frame_sync() {
    View::on_frame_sync();
    render_available_samples();
}

bool AnalogTvView::on_key(const ui::KeyEvent key) {
    if (key == ui::KeyEvent::Right) {
        field_frequency_.on_encoder(+1);
        return true;
    }
    if (key == ui::KeyEvent::Left) {
        field_frequency_.on_encoder(-1);
        return true;
    }
    return false;
}

bool AnalogTvView::on_encoder(const ui::EncoderEvent delta) {
    field_frequency_.on_encoder(delta);
    return true;
}

}  // namespace analogtv
}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_analogtv{{
    "analogtv", "Analog TV", app::Category::Receive,
    ui::Color::yellow(), &ui::bitmap_icon_receivers,
    [] { return std::make_unique<app::analogtv::AnalogTvView>(); },
    false /* hardware_limited: works with any USRP B200 RF front end */}};
}  // namespace
