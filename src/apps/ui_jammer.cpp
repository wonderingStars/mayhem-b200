/*
 * mayhem-b200 — Jammer app view.
 *
 * See ui_jammer.hpp for the encoder/port notes and the legality warning. This
 * file is the on-screen app: it collects the ranges, builds the channel plan,
 * shows it, and — only on an explicit, confirmed Start — streams the jamming IQ
 * to the transmitter.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2025 RocketGod (Flipper-derived extra modes)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_jammer.hpp"

#include "../core/string_format.hpp"
#include "../dsp/modulate.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <cmath>

namespace app {

using jammer::Channel;
using jammer::Plan;
using jammer::PlanStatus;
using jammer::Range;
using jammer::Type;

/* --- widget accessors ------------------------------------------------------ */

ui::Checkbox* JammerView::check_en(size_t i) {
    switch (i) {
        case 0: return &check_en_0_;
        case 1: return &check_en_1_;
        default: return &check_en_2_;
    }
}

ui::FrequencyField* JammerView::field_center(size_t i) {
    switch (i) {
        case 0: return &field_center_0_;
        case 1: return &field_center_1_;
        default: return &field_center_2_;
    }
}

ui::OptionsField* JammerView::options_width(size_t i) {
    switch (i) {
        case 0: return &options_width_0_;
        case 1: return &options_width_1_;
        default: return &options_width_2_;
    }
}

/* --- lifecycle ------------------------------------------------------------- */

JammerView::JammerView() {
    add_children({&labels_,
                  &text_warning_,
                  &check_en_0_, &check_en_1_, &check_en_2_,
                  &field_center_0_, &field_center_1_, &field_center_2_,
                  &options_width_0_, &options_width_1_, &options_width_2_,
                  &options_type_,
                  &options_speed_,
                  &options_hop_,
                  &field_timetx_,
                  &field_timepause_,
                  &field_jitter_,
                  &field_gain_,
                  &text_plan_,
                  &text_status_,
                  &button_tx_});

    text_warning_.set(STR_COLOR_RED "ILLEGAL to radiate - test only");
    text_status_.set(STR_COLOR_LIGHT_GREY "Idle - needs a B200 for RF");

    /* Frequency range from the device, falling back to the published B200
     * window when no radio is attached. */
    uint64_t f_min = 70'000'000, f_max = 6'000'000'000ull;
    int32_t g_max = 89;
    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        if (caps.tx_freq.max > caps.tx_freq.min) {
            f_min = static_cast<uint64_t>(caps.tx_freq.min);
            f_max = static_cast<uint64_t>(caps.tx_freq.max);
        }
        if (caps.tx_gain.max > caps.tx_gain.min)
            g_max = static_cast<int32_t>(caps.tx_gain.max);
    }
    field_gain_.set_range(0, g_max);
    field_gain_.set_value(std::min<int32_t>(30, g_max), false);

    /* Upstream defaults: range A at 315 MHz / 1 MHz, type Noise, speed 10 kHz. */
    const uint64_t defaults[kNumRanges] = {315'000'000, 433'920'000, 868'000'000};
    for (size_t i = 0; i < kNumRanges; ++i) {
        field_center(i)->set_range(f_min, f_max);
        field_center(i)->set_value(defaults[i], false);
        options_width(i)->set_by_value(1'000'000, false);  /* 1 MHz */
    }
    check_en_0_.set_value(true);

    options_type_.set_by_value(static_cast<int32_t>(Type::Random), false);  /* Noise */
    options_speed_.set_by_value(10000, false);
    options_hop_.set_by_value(0, false);

    field_timetx_.set_value(30, false);
    field_timepause_.set_value(0, false);
    field_jitter_.set_value(0, false);

    field_gain_.on_change = [this](int32_t v) {
        if (transmitting_.load())
            if (auto* tx = globals().transmitter) tx->set_gain(v);
    };

    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_.load())
            stop_tx();
        else
            start_tx();
    };

    update_plan_readout();
}

JammerView::~JammerView() {
    stop_tx();
}

void JammerView::focus() {
    button_tx_.focus();
}

void JammerView::on_hide() {
    stop_tx();
    View::on_hide();
}

/* --- planning -------------------------------------------------------------- */

std::array<Range, JammerView::kNumRanges> JammerView::collect_ranges() const {
    std::array<Range, kNumRanges> ranges{};
    for (size_t i = 0; i < kNumRanges; ++i) {
        /* const_cast: the accessors are non-const helpers but only read here. */
        auto* self = const_cast<JammerView*>(this);
        const bool en = self->check_en(i)->value();
        const int64_t center =
            static_cast<int64_t>(self->field_center(i)->value());
        const int64_t width = self->options_width(i)->selected_index_value();
        Range r;
        r.enabled = en;
        r.min = center - width / 2;
        r.max = center + width / 2;
        ranges[i] = r;
    }
    return ranges;
}

void JammerView::update_plan_readout() {
    const auto ranges = collect_ranges();
    const uint32_t hop = static_cast<uint32_t>(options_hop_.selected_index_value());
    const Plan plan = jammer::plan_channels(ranges, hop);

    switch (plan.status) {
        case PlanStatus::Ok:
            text_plan_.set("Channels: " +
                           to_string_dec_uint(plan.channels.size()));
            break;
        case PlanStatus::TooManyChannels:
            text_plan_.set(STR_COLOR_RED "Too wide (>80MHz)");
            break;
        case PlanStatus::NoRangeEnabled:
            text_plan_.set(STR_COLOR_YELLOW "No range enabled");
            break;
    }
}

/* --- transmit -------------------------------------------------------------- */

void JammerView::start_tx() {
    update_plan_readout();

    const auto ranges = collect_ranges();
    const uint32_t hop = static_cast<uint32_t>(options_hop_.selected_index_value());
    Plan plan = jammer::plan_channels(ranges, hop);

    if (plan.status == PlanStatus::NoRangeEnabled) {
        ui::display_modal("Jammer", "No range enabled.");
        return;
    }
    if (plan.status == PlanStatus::TooManyChannels) {
        ui::display_modal("Jammer",
                          "Jamming bandwidth too large.\nMust be 80 MHz or less.");
        return;
    }

    /* Explicit, per-session legality confirmation before the first transmit. */
    if (!confirmed_legal_) {
        ui::display_modal(
            "Legality",
            "Radiating interference is\nillegal almost everywhere.\n"
            "Transmit anyway?",
            ui::YESNO, [this](bool ok) {
                if (ok) {
                    confirmed_legal_ = true;
                    start_tx();
                }
            });
        return;
    }

    auto* tx = globals().transmitter;
    if (!tx) {
        text_status_.set(STR_COLOR_YELLOW "No transmitter (needs B200).");
        return;
    }

    /* Streamed band: cover the enabled span, centred between its edges. Clamp to
     * the device's streamable rate; a wider span cannot all radiate at once. */
    int64_t lo_edge = INT64_MAX, hi_edge = INT64_MIN;
    for (const auto& ch : plan.channels) {
        const int64_t half = static_cast<int64_t>(ch.width_hz) / 2;
        lo_edge = std::min<int64_t>(lo_edge, static_cast<int64_t>(ch.center) - half);
        hi_edge = std::max<int64_t>(hi_edge, static_cast<int64_t>(ch.center) + half);
    }
    tx_center_ = static_cast<uint64_t>((lo_edge + hi_edge) / 2);
    const double span = static_cast<double>(hi_edge - lo_edge);

    double max_rate = 56'000'000.0;
    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        if (caps.tx_rate.max > 0.0) max_rate = caps.tx_rate.max;
    }
    stream_rate_ = std::max(jammer::kBasebandRate, span * 1.10);
    bool truncated = false;
    if (stream_rate_ > max_rate) {
        stream_rate_ = max_rate;
        truncated = true;
    }

    engine_.configure(plan.channels,
                      static_cast<Type>(options_type_.selected_index_value()),
                      static_cast<uint32_t>(options_speed_.selected_index_value()),
                      stream_rate_, tx_center_);
    engine_.set_paused(false);

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(stream_rate_);
    tx->set_target_frequency(tx_center_);
    tx->set_gain(field_gain_.value());
    tx->set_iq_source([this](dsp::cfloat* out, size_t n) -> size_t {
        return engine_.generate(out, n);
    });

    if (!tx->start()) {
        tx->set_iq_source(nullptr);
        text_status_.set(STR_COLOR_YELLOW "TX start failed (needs B200).");
        return;
    }

    /* Reset the duty-cycle scheduler. */
    cooling_ = false;
    sched_seconds_ = 0;
    sched_mscounter_ = 0;

    transmitting_.store(true);
    button_tx_.set_text("Stop");

    std::string msg = STR_COLOR_GREEN "TX ";
    msg += to_string_dec_uint(plan.channels.size()) + "ch @ ";
    msg += to_string_decimal(static_cast<float>(stream_rate_ / 1e6), 1) + "M";
    text_status_.set(msg);
    if (truncated)
        text_plan_.set(STR_COLOR_YELLOW "Span > dev BW; edges cut");
}

void JammerView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    engine_.set_paused(false);
    if (transmitting_.exchange(false)) {
        button_tx_.set_text("Start");
        text_status_.set(STR_COLOR_LIGHT_GREY "Idle");
    }
}

/* --- duty-cycle scheduler (port of JammerView::on_timer) ------------------- */

uint32_t JammerView::lfsr_iterate31(uint32_t v) {
    /* common/lfsr_random.cpp: Fibonacci, length 31, taps (31,18),
     * shift amounts (12,12,8). */
    const uint32_t zero = 0;
    v = (v << 12) |
        (((v >> (31 - 12)) ^ (v >> (18 - 12))) & (~(~zero << 12)));
    v = (v << 12) |
        (((v >> (31 - 12)) ^ (v >> (18 - 12))) & (~(~zero << 12)));
    v = (v << 8) |
        (((v >> (31 - 8)) ^ (v >> (18 - 8))) & (~(~zero << 8)));
    return v;
}

void JammerView::tick_scheduler() {
    /* on_frame_sync runs at ~60 Hz; 60 ticks = one second, matching upstream's
     * mscounter against the 60 Hz DisplayFrameSync. */
    if (++sched_mscounter_ < 60) return;
    sched_mscounter_ = 0;

    const int32_t timepause = field_timepause_.value();

    if (cooling_) {
        if (timepause == 0 || ++sched_seconds_ >= timepause) {
            engine_.set_paused(false);
            button_tx_.set_text("Stop");
            const int32_t jitter = field_jitter_.value();
            if (jitter) {
                jitter_lfsr_ = lfsr_iterate31(jitter_lfsr_);
                sched_mscounter_ = static_cast<int16_t>(
                    sched_mscounter_ + (jitter / 2) -
                    static_cast<int32_t>(jitter_lfsr_ & jitter));
            }
            cooling_ = false;
            sched_seconds_ = 0;
        }
    } else {
        if (timepause && ++sched_seconds_ >= field_timetx_.value()) {
            engine_.set_paused(true);
            button_tx_.set_text("Paused");
            const int32_t jitter = field_jitter_.value();
            if (jitter) {
                jitter_lfsr_ = lfsr_iterate31(jitter_lfsr_);
                sched_mscounter_ = static_cast<int16_t>(
                    sched_mscounter_ + (jitter / 2) -
                    static_cast<int32_t>(jitter_lfsr_ & jitter));
            }
            cooling_ = true;
            sched_seconds_ = 0;
        }
    }
}

void JammerView::on_frame_sync() {
    View::on_frame_sync();
    if (!transmitting_.load()) return;
    tick_scheduler();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream is an external app in the TX menu. The channel planner, hop
 * sequencer and noise generator are the tested, faithful port; RF only radiates
 * with a USRP B200 attached, so hardware_limited stays false. */
const app::Registrar reg_jammer{{
    "jammer", "Jammer", app::Category::Transmit,
    ui::Color::red(), &ui::bitmap_icon_transmit,
    [] { return std::make_unique<app::JammerView>(); }}};
}  // namespace
