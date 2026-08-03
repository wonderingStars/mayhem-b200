/*
 * mayhem-b200 — VOR TX.
 *
 * See ui_vor_tx.hpp for what was ported, from where, and what changed.
 *
 * Copyright (C) 2026 PortaPack Mayhem (original app and baseband)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_vor_tx.hpp"

#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "string_format.hpp"
#include "ui_alphanum.hpp"
#include "ui_modal.hpp"
#include "ui_morse_tx.hpp" /* app::morse_tx::morse_encode() — same ITU table as Morse TX */
#include "ui_navigation.hpp"

#include <algorithm>
#include <cmath>

namespace app {
namespace vor_tx {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

/* Wraps `phase` back into [0, 2*pi) without the unbounded growth a bare
 * running accumulator would suffer over a transmission that may run for
 * hours. */
inline void wrap_phase(double& phase) {
    if (phase >= kTwoPi)
        phase = std::fmod(phase, kTwoPi);
    else if (phase < 0.0)
        phase = kTwoPi - std::fmod(-phase, kTwoPi);
}
}  // namespace

double vor_radial_to_offset_rad(int32_t radial_deg) {
    int32_t d = radial_deg % 360;
    if (d < 0) d += 360;
    return static_cast<double>(d) * kPi / 180.0;
}

void VorTxGenerator::configure(double sample_rate_hz) {
    std::lock_guard<std::mutex> lock(mutex_);
    sample_rate_ = (sample_rate_hz > 0.0) ? sample_rate_hz : 48000.0;
    ref_phase_ = sub_phase_ = id_phase_ = 0.0;
    ident_index_ = 0;
    ident_remaining_ = 0;
    rebuild_ident_schedule_locked();
}

void VorTxGenerator::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    ref_phase_ = sub_phase_ = id_phase_ = 0.0;
    ident_index_ = 0;
    ident_remaining_ = 0;
}

void VorTxGenerator::set_radial(int32_t radial_deg) {
    std::lock_guard<std::mutex> lock(mutex_);
    radial_deg_ = radial_deg;
    radial_offset_rad_ = vor_radial_to_offset_rad(radial_deg);
}

int32_t VorTxGenerator::radial() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return radial_deg_;
}

void VorTxGenerator::set_ident_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    ident_enabled_ = enabled;
}

bool VorTxGenerator::ident_enabled() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ident_enabled_;
}

void VorTxGenerator::set_ident_text(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    ident_text_ = text.substr(0, 7);
    ident_index_ = 0;
    ident_remaining_ = 0;
    rebuild_ident_schedule_locked();
}

const std::string& VorTxGenerator::ident_text() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ident_text_;
}

/* Ported from proc_vor_tx.cpp's build_ident_schedule(), but the dot/dash/gap
 * timeline itself comes from app::morse_tx::morse_encode() (ui_morse_tx.hpp)
 * rather than a second copy of the ITU table: morse_encode() already appends
 * a LETTER_SPACE after each character and a WORD_SPACE between words, so
 * expanding its symbol stream one-for-one against morse_symbols[] reproduces
 * upstream's schedule exactly. */
void VorTxGenerator::rebuild_ident_schedule_locked() {
    ident_segments_.clear();

    std::vector<uint8_t> symbols;
    if (morse_tx::morse_encode(ident_text_, symbols) == 0) return;

    const double dot_seconds = 1200.0 / (static_cast<double>(kIdentWpm) * 1000.0);
    uint64_t dot_samples = static_cast<uint64_t>(std::llround(dot_seconds * sample_rate_));
    if (dot_samples < 1) dot_samples = 1;

    uint64_t total_samples = 0;
    for (uint8_t s : symbols) {
        if (s >= 5) continue;
        const bool on = (s < 2); /* DOT or DASH */
        const uint64_t len = static_cast<uint64_t>(morse_tx::morse_symbols[s]) * dot_samples;
        ident_segments_.push_back({on, len});
        total_samples += len;
    }

    if (ident_segments_.empty()) return;

    /* Pad the trailing silence so ident-start to ident-start is a full
     * kIdentPeriodSeconds; fall back to one word-space gap if the keyed
     * sequence somehow runs longer than that. */
    const uint64_t period_samples =
        static_cast<uint64_t>(std::llround(kIdentPeriodSeconds * sample_rate_));
    const uint64_t min_gap =
        static_cast<uint64_t>(morse_tx::morse_symbols[morse_tx::WORD_SPACE]) * dot_samples;
    const uint64_t gap = (period_samples > total_samples + min_gap)
                              ? (period_samples - total_samples)
                              : min_gap;
    ident_segments_.push_back({false, gap});
}

bool VorTxGenerator::ident_tone_active_locked() {
    if (ident_segments_.empty()) return false;

    if (ident_remaining_ == 0) ident_remaining_ = ident_segments_[ident_index_].length_samples;

    const bool on = ident_segments_[ident_index_].on;

    if (ident_remaining_ == 0 || --ident_remaining_ == 0) {
        ident_index_ = (ident_index_ + 1) % ident_segments_.size();
    }

    return on;
}

void VorTxGenerator::process(dsp::cfloat* out, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);

    const double ref_step = kTwoPi * kToneHz / sample_rate_;
    const double sub_step_nominal = kTwoPi * kSubcarrierHz / sample_rate_;
    const double sub_dev_step = kTwoPi * kSubcarrierDeviationHz / sample_rate_;
    const double id_step = kTwoPi * kIdentToneHz / sample_rate_;

    for (size_t i = 0; i < count; i++) {
        const double ref_sine = std::sin(ref_phase_);
        const double var_sine = std::sin(ref_phase_ - radial_offset_rad_);

        sub_phase_ += sub_step_nominal + (ref_sine * sub_dev_step);
        wrap_phase(sub_phase_);
        const double sub_sine = std::sin(sub_phase_);

        double id_sine = 0.0;
        if (ident_enabled_ && ident_tone_active_locked()) {
            id_sine = std::sin(id_phase_);
            id_phase_ += id_step;
            wrap_phase(id_phase_);
        }

        double env = kCarrierLevel + (var_sine * kVarDepth) + (sub_sine * kSubDepth) +
                     (id_sine * kIdDepth);
        env = std::clamp(env, 0.0, 1.0);

        out[i] = dsp::cfloat{static_cast<float>(env), 0.0f};

        ref_phase_ += ref_step;
        wrap_phase(ref_phase_);
    }
}

}  // namespace vor_tx

/* --- View -------------------------------------------------------------- */

VorTxView::VorTxView() {
    add_children({&labels_, &field_radial_, &text_radial_unit_, &field_freq_, &step_view_,
                  &check_ident_, &button_ident_text_, &text_status_, &warning_, &button_tx_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_freq_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                              static_cast<uint64_t>(caps.tx_freq.max));
    }
    field_freq_.set_value(113'500'000, false);
    field_freq_.on_change = [](uint64_t hz) {
        if (auto* tx = globals().transmitter) tx->set_target_frequency(hz);
    };

    field_radial_.set_value(0, false);
    generator_.set_radial(0);
    field_radial_.on_change = [this](int32_t v) { generator_.set_radial(v); };

    check_ident_.set_value(true);
    generator_.set_ident_enabled(true);
    check_ident_.on_select = [this](ui::Checkbox&, bool v) { generator_.set_ident_enabled(v); };

    generator_.set_ident_text("VOR");
    button_ident_text_.set_text(generator_.ident_text());
    button_ident_text_.on_select = [this](ui::Button&) {
        auto* nav = globals().nav;
        if (!nav) return;
        ident_edit_buffer_ = generator_.ident_text();
        ui::text_prompt(*nav, ident_edit_buffer_, 7, ENTER_KEYBOARD_MODE_ALPHA,
                        [this](std::string& v) {
                            generator_.set_ident_text(v);
                            button_ident_text_.set_text(generator_.ident_text());
                        });
    };

    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            request_start();
    };
}

VorTxView::~VorTxView() {
    if (transmitting_) stop_tx();
}

void VorTxView::focus() {
    field_radial_.focus();
}

void VorTxView::request_start() {
    if (tx_acknowledged_) {
        start_tx();
        return;
    }
    ui::display_modal(
        "TX WARNING",
        "VOR is an aviation nav band.\nTX only into a dummy load or\na shielded test setup.",
        ui::YESNO, [this](bool choice) {
            if (choice) {
                tx_acknowledged_ = true;
                start_tx();
            }
        });
}

void VorTxView::start_tx() {
    auto* tx = globals().transmitter;
    if (!tx) {
        text_status_.set(STR_COLOR_RED "No transmitter wired.");
        return;
    }

    /* A few kHz is ample for a ~11 kHz-wide composite (carrier + 30 Hz +
     * 9960 Hz subcarrier +/-480 Hz); clamp to whatever the device offers. */
    double fs = 192'000.0;
    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        if (caps.tx_rate.min > fs) fs = caps.tx_rate.min;
    }

    generator_.configure(fs);
    generator_.set_radial(field_radial_.value());

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(fs);
    tx->set_target_frequency(field_freq_.value());
    tx->set_iq_source([this](dsp::cfloat* out, size_t n) -> size_t {
        generator_.process(out, n);
        return n;
    });

    if (!tx->start()) {
        text_status_.set(STR_COLOR_YELLOW "TX start failed (needs B200).");
        tx->set_iq_source(nullptr);
        return;
    }

    transmitting_ = true;
    button_tx_.set_text("Stop");
    text_status_.set(STR_COLOR_GREEN "TX radial " + to_string_dec_uint(
                                                        static_cast<uint32_t>(field_radial_.value())));
}

void VorTxView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    transmitting_ = false;
    button_tx_.set_text("Start");
    text_status_.set("Idle");
}

void VorTxView::on_frame_sync() {
    View::on_frame_sync();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream: menu_location TX, icon_color green (external/vor_tx/main.cpp);
 * its bitmap is a plain diamond that nothing in bitmaps.hpp matches, so this
 * takes the generic tile rather than borrowing a misleading icon. */
const app::Registrar reg_vor_tx{{"vor_tx", "VOR TX", app::Category::Transmit,
                                 ui::Color::green(), nullptr,
                                 [] { return std::make_unique<app::VorTxView>(); }}};
}  // namespace
