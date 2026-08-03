/*
 * mayhem-b200 — Mic TX: live microphone transmitter (implementation).
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2024 Mark Thompson (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_mictx.hpp"

#include "../audio/audio_in.hpp"
#include "../core/settings.hpp"
#include "../dsp/modulate.hpp"  /* dsp::tones::ctcss */
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"

#include <algorithm>
#include <cmath>
#include <string>

namespace app {

/* --- Pure signal maths ----------------------------------------------------- */

double mictx_deviation_hz(uint32_t bw_khz) {
    return static_cast<double>(bw_khz) * 1000.0;
}

float mictx_tone_mix_weight(int percent) {
    /* persistent_memory tone_mix range is 10..99 (reset 20). */
    if (percent < 10) percent = 10;
    if (percent > 99) percent = 99;
    return static_cast<float>(percent) / 100.0f;
}

uint8_t mictx_meter_value(const float* samples, size_t count, float gain) {
    if (!samples || count == 0) return 0;

    double acc = 0.0;
    for (size_t i = 0; i < count; i++)
        acc += std::fabs(static_cast<double>(samples[i]) * static_cast<double>(gain));

    const double mean_abs = acc / static_cast<double>(count);

    /* Upstream FM VU: value = 4 * mean|sample_8bit|, and a full-scale float maps
     * to +/-128 in the 8-bit domain, so value = 512 * mean|sample_float|. */
    double scaled = mean_abs * 512.0;
    if (scaled < 0.0) scaled = 0.0;
    if (scaled > 255.0) scaled = 255.0;
    return static_cast<uint8_t>(std::lround(scaled));
}

namespace {

constexpr uint32_t kFrameMs = 1000 / 60;  /* on_frame_sync cadence, ~16 ms */

radio::TransmitterModel::Mode tx_mode_for(int32_t mod) {
    using Mode = radio::TransmitterModel::Mode;
    switch (mod) {
        case MicTXView::MOD_WFM: return Mode::WidebandFM;
        case MicTXView::MOD_AM: return Mode::AM;
        case MicTXView::MOD_USB: return Mode::USB;
        case MicTXView::MOD_LSB: return Mode::LSB;
        case MicTXView::MOD_DSB: return Mode::DSB;
        case MicTXView::MOD_NFM:
        default: return Mode::NarrowbandFM;
    }
}

}  // namespace

/* --- View ------------------------------------------------------------------ */

MicTXView::MicTXView() {
    add_children({&labels_,
                  &field_frequency_,
                  &options_mode_,
                  &field_bw_,
                  &options_gain_,
                  &field_tx_gain_,
                  &options_tone_,
                  &field_dcs_,
                  &field_tone_mix_,
                  &check_vox_,
                  &field_vox_level_,
                  &vumeter_,
                  &text_status_,
                  &text_warn_,
                  &tx_button_});

    text_warn_.set_style(ui::Theme::getInstance()->fg_yellow);

    /* Restore persisted settings (defaults match the member initialisers). */
    auto& s = core::settings();
    mod_ = static_cast<int32_t>(s.get_int("tx_mic", "mod", mod_));
    bw_khz_ = static_cast<uint32_t>(s.get_int("tx_mic", "bw_khz", bw_khz_));
    mic_gain_x10_ = static_cast<uint32_t>(s.get_int("tx_mic", "mic_gain_x10", mic_gain_x10_));
    tx_gain_db_ = static_cast<int32_t>(s.get_int("tx_mic", "tx_gain", tx_gain_db_));
    tone_value_ = static_cast<int32_t>(s.get_int("tx_mic", "tone", tone_value_));
    dcs_code_ = static_cast<int32_t>(s.get_int("tx_mic", "dcs", dcs_code_));
    tone_mix_pct_ = static_cast<int32_t>(s.get_int("tx_mic", "tone_mix", tone_mix_pct_));
    vox_enabled_ = s.get_int("tx_mic", "vox", vox_enabled_ ? 1 : 0) != 0;
    vox_level_ = static_cast<int32_t>(s.get_int("tx_mic", "vox_level", vox_level_));
    tx_frequency_ = static_cast<uint64_t>(
        s.get_int("tx_mic", "frequency", static_cast<int64_t>(tx_frequency_)));

    /* Tone-key selector: "None" plus the CTCSS table, keyed by table index. */
    ui::OptionsField::options_t tone_opts;
    tone_opts.emplace_back("None", -1);
    for (size_t i = 0; i < dsp::tones::ctcss.size(); i++)
        tone_opts.emplace_back(dsp::tones::ctcss[i].name, static_cast<int32_t>(i));
    options_tone_.set_options(tone_opts);

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_frequency_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                                   static_cast<uint64_t>(caps.tx_freq.max));
        field_tx_gain_.set_range(static_cast<int32_t>(caps.tx_gain.min),
                                 static_cast<int32_t>(caps.tx_gain.max));
    }

    field_frequency_.set_step_index(7);  /* 12.5 kHz, upstream's default step */
    field_frequency_.set_value(tx_frequency_, false);
    field_frequency_.on_change = [this](uint64_t f) {
        tx_frequency_ = f;
        if (auto* tx = globals().transmitter) tx->set_target_frequency(f);
    };

    options_mode_.set_by_value(mod_, false);
    options_mode_.on_change = [this](size_t, int32_t v) {
        mod_ = v;
        apply_mode();
    };

    field_bw_.set_value(static_cast<int32_t>(bw_khz_), false);
    field_bw_.on_change = [this](int32_t v) {
        bw_khz_ = static_cast<uint32_t>(v);
        if (is_fm_mode())
            if (auto* tx = globals().transmitter)
                tx->set_deviation(mictx_deviation_hz(bw_khz_));
        update_status();
    };

    options_gain_.set_by_value(static_cast<int32_t>(mic_gain_x10_), false);
    options_gain_.on_change = [this](size_t, int32_t v) {
        mic_gain_x10_ = static_cast<uint32_t>(v);
        apply_audio_gain();
    };

    field_tx_gain_.set_value(tx_gain_db_, false);
    field_tx_gain_.on_change = [this](int32_t v) {
        tx_gain_db_ = v;
        if (auto* tx = globals().transmitter) tx->set_gain(static_cast<double>(v));
    };

    options_tone_.set_by_value(tone_value_, false);
    options_tone_.on_change = [this](size_t, int32_t v) {
        tone_value_ = v;
        apply_sub_tone();
    };

    field_dcs_.set_value(dcs_code_, false);
    field_dcs_.on_change = [this](int32_t v) {
        dcs_code_ = v;
        apply_sub_tone();
    };

    field_tone_mix_.set_value(tone_mix_pct_, false);
    field_tone_mix_.on_change = [this](int32_t v) {
        tone_mix_pct_ = v;
        apply_sub_tone();
    };

    check_vox_.set_value(vox_enabled_);
    check_vox_.on_select = [this](ui::Checkbox&, bool v) {
        vox_enabled_ = v;
        attack_timer_ms_ = 0;
        decay_timer_ms_ = 0;
        update_status();
    };

    field_vox_level_.set_value(vox_level_, false);
    field_vox_level_.on_change = [this](int32_t v) {
        vox_level_ = v;
        vumeter_.set_mark(static_cast<uint8_t>(std::clamp(v, 0, 255)));
    };
    vumeter_.set_mark(static_cast<uint8_t>(std::clamp(vox_level_, 0, 255)));

    /* PTT: explicit toggle. First press keys TX, second releases it. Nothing
     * transmits until this (or VOX, which the user enables) fires. */
    tx_button_.on_select = [this](ui::Button&) {
        if (transmitting_) {
            set_tx(false);
        } else {
            ptt_latched_ = true;
            set_tx(true);
        }
    };
}

MicTXView::~MicTXView() {
    set_tx(false);
    if (auto* ai = globals().audio_in) ai->stop();

    auto& s = core::settings();
    s.set_int("tx_mic", "mod", mod_);
    s.set_int("tx_mic", "bw_khz", static_cast<int64_t>(bw_khz_));
    s.set_int("tx_mic", "mic_gain_x10", static_cast<int64_t>(mic_gain_x10_));
    s.set_int("tx_mic", "tx_gain", tx_gain_db_);
    s.set_int("tx_mic", "tone", tone_value_);
    s.set_int("tx_mic", "dcs", dcs_code_);
    s.set_int("tx_mic", "tone_mix", tone_mix_pct_);
    s.set_int("tx_mic", "vox", vox_enabled_ ? 1 : 0);
    s.set_int("tx_mic", "vox_level", vox_level_);
    s.set_int("tx_mic", "frequency", static_cast<int64_t>(tx_frequency_));
    s.save();
}

void MicTXView::focus() {
    field_frequency_.focus();
}

void MicTXView::apply_audio_gain() {
    if (auto* tx = globals().transmitter)
        tx->set_audio_gain(static_cast<float>(mic_gain_x10_) / 10.0f);
}

void MicTXView::apply_mode() {
    if (auto* tx = globals().transmitter) {
        tx->set_mode(tx_mode_for(mod_));
        if (mod_ == MOD_NFM)
            tx->set_nfm_configuration(radio::TransmitterModel::NfmConfig::Wide16k);
        /* Deviation only bites in FM; zero restores the mode default elsewhere. */
        tx->set_deviation(is_fm_mode() ? mictx_deviation_hz(bw_khz_) : 0.0);
    }

    /* Deviation is an FM-only control; sub-tones only ride FM carriers. */
    field_bw_.hidden(!is_fm_mode());

    /* Sensible per-mode deviation default + range when switching into FM. */
    if (mod_ == MOD_NFM) {
        field_bw_.set_range(1, 60);
    } else if (mod_ == MOD_WFM) {
        field_bw_.set_range(1, 150);
    }

    apply_sub_tone();
    update_status();
    set_dirty();
}

void MicTXView::apply_sub_tone() {
    auto* tx = globals().transmitter;
    if (!tx) return;

    if (!is_fm_mode()) {
        tx->set_sub_tone_none();
        return;
    }

    const float w = mictx_tone_mix_weight(tone_mix_pct_);

    if (dcs_code_ > 0) {
        tx->set_dcs(static_cast<uint16_t>(dcs_code_), w);
    } else if (tone_value_ >= 0 &&
               static_cast<size_t>(tone_value_) < dsp::tones::ctcss.size()) {
        tx->set_ctcss(dsp::tones::ctcss[static_cast<size_t>(tone_value_)].frequency_hz, w);
    } else {
        tx->set_sub_tone_none();
    }
}

void MicTXView::set_tx(bool enable) {
    auto* tx = globals().transmitter;
    auto* ai = globals().audio_in;

    if (enable) {
        if (transmitting_) return;
        if (!tx || !ai) {
            text_status_.set(STR_COLOR_RED "No radio / mic wired");
            ptt_latched_ = false;
            return;
        }

        if (!ai->running()) ai->start();

        tx_ring_.clear();
        apply_mode();
        apply_audio_gain();
        tx->set_target_frequency(tx_frequency_);
        tx->set_gain(static_cast<double>(tx_gain_db_));
        tx->set_audio_source([this](float* out, size_t n) {
            return tx_ring_.read(out, n);
        });

        if (!tx->start()) {
            tx->set_audio_source({});
            transmitting_ = false;
            ptt_latched_ = false;
            text_status_.set(STR_COLOR_RED "TX start failed (needs B200)");
            return;
        }

        transmitting_ = true;
        tx_button_.set_text("STOP TX");
    } else {
        transmitting_ = false;
        ptt_latched_ = false;
        attack_timer_ms_ = 0;
        decay_timer_ms_ = 0;
        if (tx) {
            tx->stop();
            tx->set_audio_source({});
        }
        tx_ring_.clear();
        tx_button_.set_text("PTT / TX");
    }
    update_status();
}

void MicTXView::pump_audio() {
    auto* ai = globals().audio_in;
    if (!ai || !ai->running()) return;

    if (capture_.size() != 2048) capture_.assign(2048, 0.0f);

    const float gain = static_cast<float>(mic_gain_x10_) / 10.0f;
    uint8_t peak_meter = 0;

    /* Drain what the capture device has, bounded so a backlog cannot stall the
     * UI frame. Meter every block; feed the modulator only while keyed. */
    for (int iter = 0; iter < 8; iter++) {
        const size_t got = ai->read(capture_.data(), capture_.size());
        if (got == 0) break;

        const uint8_t m = mictx_meter_value(capture_.data(), got, gain);
        if (m > peak_meter) peak_meter = m;

        if (transmitting_) tx_ring_.write(capture_.data(), got);

        if (got < capture_.size()) break;
    }

    meter_value_ = peak_meter;
    vumeter_.set_value(meter_value_);
}

void MicTXView::on_show() {
    ui::View::on_show();

    if (auto* ai = globals().audio_in)
        if (!ai->running()) ai->start();

    apply_mode();
    apply_audio_gain();
    if (auto* tx = globals().transmitter) {
        tx->set_target_frequency(tx_frequency_);
        tx->set_gain(static_cast<double>(tx_gain_db_));
    }

    update_status();
    field_frequency_.focus();
}

void MicTXView::on_hide() {
    set_tx(false);
    ui::View::on_hide();
}

void MicTXView::on_frame_sync() {
    ui::View::on_frame_sync();

    pump_audio();

    if (vox_enabled_) {
        if (!transmitting_) {
            if (meter_value_ >= vox_level_) {
                attack_timer_ms_ += kFrameMs;
                if (attack_timer_ms_ >= attack_ms_) {
                    attack_timer_ms_ = 0;
                    decay_timer_ms_ = 0;
                    set_tx(true);
                }
            } else {
                attack_timer_ms_ = 0;
            }
        } else {
            if (meter_value_ < vox_level_) {
                decay_timer_ms_ += kFrameMs;
                if (decay_timer_ms_ >= decay_ms_) {
                    decay_timer_ms_ = 0;
                    attack_timer_ms_ = 0;
                    if (!ptt_latched_) set_tx(false);
                }
            } else {
                decay_timer_ms_ = 0;
            }
        }
    }
}

void MicTXView::update_status() {
    std::string line;
    if (transmitting_) {
        line = STR_COLOR_RED "TX ";
    } else if (vox_enabled_) {
        line = STR_COLOR_YELLOW "VOX ";
    } else {
        line = STR_COLOR_GREEN "RDY ";
    }

    line += options_mode_.selected_index_name();

    if (is_fm_mode())
        line += " " + std::to_string(bw_khz_) + "k";

    if (is_fm_mode()) {
        if (dcs_code_ > 0)
            line += " D" + std::to_string(dcs_code_);
        else if (tone_value_ >= 0 &&
                 static_cast<size_t>(tone_value_) < dsp::tones::ctcss.size())
            line += " CT";
    }

    text_status_.set(line);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream ui_navigation.cpp:
 *   {"microphone", "Mic", TRX, Color::green(), &bitmap_icon_microphone, ...} */
const app::Registrar reg_mictx{{
    "microphone", "Mic TX", app::Category::Transceiver,
    ui::Color::green(), &ui::bitmap_icon_microphone,
    [] { return std::make_unique<app::MicTXView>(); }}};
}  // namespace
