/*
 * mayhem-b200 — Soundboard: transmit a .WAV file as audio-modulated RF.
 *
 * Ported from firmware/application/external/soundboard/soundboard_app.* .
 * See ui_soundboard.hpp for the pipeline description.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_soundboard.hpp"

#include "../audio/audio_out.hpp"  /* audio::sample_rate */
#include "../core/file_path.hpp"
#include "../core/fs_utils.hpp"
#include "../core/string_format.hpp"
#include "../dsp/modulate.hpp"  /* dsp::tones::ctcss */
#include "../radio/transmitter_model.hpp"
#include "app_context.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <cctype>
#include <memory>

namespace app {

namespace {

#ifndef MB200_SOUNDBOARD_NO_REGISTRAR
/* Fibonacci LFSR, length 31, taps (31, 18), shift amounts (12, 12, 8), shift
 * left — an exact port of lfsr_iterate_internal from firmware/common/
 * lfsr_random.cpp so the Random shuffle walks the same sequence upstream does. */
uint32_t lfsr_iterate(uint32_t v) {
    enum {
        tap_0 = 31,
        tap_1 = 18,
        s0 = 12,
        s1 = 12,
        s2 = 8,
    };
    const uint32_t zero = 0;
    v = (v << s0) |
        (((v >> (tap_0 - s0)) ^ (v >> (tap_1 - s0))) & (~(~zero << s0)));
    v = (v << s1) |
        (((v >> (tap_0 - s1)) ^ (v >> (tap_1 - s1))) & (~(~zero << s1)));
    v = (v << s2) |
        (((v >> (tap_0 - s2)) ^ (v >> (tap_1 - s2))) & (~(~zero << s2)));
    return v;
}
#endif  // MB200_SOUNDBOARD_NO_REGISTRAR

bool contains_ci(const std::string& haystack, const char* needle) {
    std::string h = haystack;
    std::transform(h.begin(), h.end(), h.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return h.find(needle) != std::string::npos;
}

}  // namespace

/* --- file enumeration ------------------------------------------------------ */

std::vector<std::string> soundboard_list_wavs(const std::string& dir) {
    std::vector<std::string> out;

    std::vector<core::DirEntry> entries;
    core::ListOptions opts{".WAV"};
    opts.include_directories = false;
    if (!core::list_directory(dir, entries, opts))
        return out;

    core::WavFileReader reader;
    for (const auto& e : entries) {
        if (e.is_directory) continue;
        /* Skip the shopping-cart LF waveform, as upstream's refresh_list does. */
        if (contains_ci(e.name, "shopping_cart")) continue;

        const std::string full = core::path_join(dir, e.name);
        if (!reader.open(full)) continue;

        const bool mono = (reader.channels() == 1);
        const uint16_t bits = reader.bits_per_sample();
        const bool ok_bits = (bits == 8) || (bits == 16);
        reader.close();

        if (mono && ok_bits)
            out.push_back(full);
    }

    return out;
}

/* --- WavAudioSource -------------------------------------------------------- */

bool WavAudioSource::open(const std::string& path) {
    close();

    if (!reader_.open(path))
        return false;

    /* Only mono 8/16-bit is playable; read_samples decodes exactly those. */
    if (reader_.channels() != 1 ||
        (reader_.bits_per_sample() != 8 && reader_.bits_per_sample() != 16)) {
        reader_.close();
        return false;
    }

    wav_rate_ = reader_.sample_rate();
    total_samples_ = reader_.sample_count();

    /* File rate up to the transmitter's audio rate (48 kHz). */
    resampler_.configure(static_cast<double>(wav_rate_),
                         static_cast<double>(audio::sample_rate));

    pending_.clear();
    pending_pos_ = 0;
    eof_ = false;
    samples_played_.store(0);
    finished_.store(false);
    return true;
}

void WavAudioSource::close() {
    reader_.close();
    resampler_.reset();
    pending_.clear();
    pending_pos_ = 0;
    eof_ = false;
    samples_played_.store(0);
    finished_.store(false);
}

size_t WavAudioSource::render(float* out, size_t count) {
    size_t produced = 0;

    while (produced < count) {
        /* Hand out whatever the resampler already produced. */
        if (pending_pos_ < pending_.size()) {
            const size_t n =
                std::min(count - produced, pending_.size() - pending_pos_);
            for (size_t i = 0; i < n; i++)
                out[produced + i] = pending_[pending_pos_ + i];
            produced += n;
            pending_pos_ += n;
            continue;
        }

        if (eof_) break;

        /* Refill: read a chunk, promote to float [-1, 1], resample to 48 kHz.
         * read_samples already applies the firmware's 8-bit promotion,
         * (v - 0x80) * 256, so the same scaling by 1/32768 covers both depths. */
        read_buf_.resize(read_chunk);
        const size_t got = reader_.read_samples(read_buf_.data(), read_chunk);
        if (got == 0) {
            eof_ = true;
            break;
        }
        samples_played_.fetch_add(got);

        in_buf_.resize(got);
        constexpr float scale = 1.0f / 32768.0f;
        for (size_t i = 0; i < got; i++)
            in_buf_[i] = static_cast<float>(read_buf_[i]) * scale;

        pending_.clear();
        pending_pos_ = 0;
        resampler_.process(in_buf_.data(), got, pending_);
    }

    if (eof_ && pending_pos_ >= pending_.size())
        finished_.store(true);

    return produced;
}

float WavAudioSource::progress() const {
    if (total_samples_ == 0) return 0.0f;
    const float p = static_cast<float>(samples_played_.load()) /
                    static_cast<float>(total_samples_);
    return std::clamp(p, 0.0f, 1.0f);
}

/* --- SoundboardView --------------------------------------------------------
 *
 * The view and its self-registration are excluded from the off-tree unit test
 * build (MB200_SOUNDBOARD_NO_REGISTRAR): they reference the whole UHD transmit
 * chain, which the test neither needs nor can link. WavAudioSource and
 * soundboard_list_wavs above are the tested units and stay in every build. */
#ifndef MB200_SOUNDBOARD_NO_REGISTRAR

SoundboardView::SoundboardView() {
    wav_directory_ = core::path_join(core::data_directory(), "WAV");
    core::ensure_directory(wav_directory_);

    add_children({&labels_, &field_freq_, &opt_mode_, &opt_tone_, &menu_,
                  &text_empty_, &text_status_, &progress_, &check_loop_,
                  &check_random_, &button_play_, &button_refresh_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_freq_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                              static_cast<uint64_t>(caps.tx_freq.max));
    }
    if (auto* tx = globals().transmitter)
        field_freq_.set_value(tx->target_frequency(), false);
    field_freq_.on_change = [this](uint64_t f) {
        if (auto* tx = globals().transmitter) tx->set_target_frequency(f);
    };

    opt_mode_.set_selected_index(0, false);

    /* CTCSS tone-key list: "None" then the 50 sub-audible tones. The value is
     * the index into that combined list; index 0 is None. */
    ui::OptionsField::options_t tone_options;
    tone_options.push_back({"None", 0});
    for (size_t i = 0; i < dsp::tones::ctcss.size(); i++)
        tone_options.push_back(
            {std::string{dsp::tones::ctcss[i].name}, static_cast<int32_t>(i + 1)});
    opt_tone_.set_options(std::move(tone_options));
    opt_tone_.set_selected_index(0, false);

    check_loop_.set_value(false);
    check_random_.set_value(false);

    button_play_.on_select = [this](ui::Button&) {
        if (playing_) {
            stop();
        } else if (!files_.empty()) {
            start_tx(menu_.highlighted_index());
        }
    };

    button_refresh_.on_select = [this](ui::Button&) {
        if (playing_) stop();
        refresh_list();
    };

    refresh_list();
    set_idle_status();
}

SoundboardView::~SoundboardView() {
    stop();
}

void SoundboardView::focus() {
    if (!files_.empty())
        menu_.focus();
    else
        button_refresh_.focus();
}

void SoundboardView::on_hide() {
    stop();
    View::on_hide();
}

void SoundboardView::on_frame_sync() {
    View::on_frame_sync();
    if (!playing_) return;

    progress_.set_value(static_cast<uint32_t>(source_.samples_played()));

    if (source_.finished())
        on_finished();
}

void SoundboardView::refresh_list() {
    files_ = soundboard_list_wavs(wav_directory_);

    menu_.clear();

    if (files_.empty()) {
        menu_.hidden(true);
        text_empty_.hidden(false);
    } else {
        menu_.hidden(false);
        text_empty_.hidden(true);
        for (size_t n = 0; n < files_.size(); n++) {
            const std::string name = core::filename(files_[n]);
            menu_.add_item({name.substr(0, 30), ui::Color::magenta(),
                            [this, n]() { start_tx(n); }});
        }
        menu_.set_highlighted(0);
    }

    set_dirty();
    set_idle_status();
}

void SoundboardView::start_tx(size_t index) {
    if (index >= files_.size()) {
        file_error("No file selected.");
        return;
    }

    auto* tx = globals().transmitter;
    if (!tx) {
        file_error("No transmitter wired.\nNeeds a USRP B200.");
        return;
    }

    /* Clean transition: stop() joins the DSP thread so it is safe to reopen. */
    stop();

    if (!source_.open(files_[index])) {
        file_error("File read error:\n" + core::filename(files_[index]));
        return;
    }

    playing_index_ = index;
    progress_.set_max(source_.total_samples() ? source_.total_samples() : 1);
    progress_.set_value(0);

    using Mode = radio::TransmitterModel::Mode;
    switch (static_cast<Mod>(opt_mode_.selected_index())) {
        case Mod::WFM:
            tx->set_mode(Mode::WidebandFM);
            break;
        case Mod::AM:
            tx->set_mode(Mode::AM);
            break;
        case Mod::NFM:
        default:
            tx->set_mode(Mode::NarrowbandFM);
            /* Upstream's default deviation is 5 kHz (the widest NFM slot). */
            tx->set_nfm_configuration(
                radio::TransmitterModel::NfmConfig::Wide16k);
            break;
    }

    tx->set_target_frequency(field_freq_.value());
    tx->set_audio_gain(1.0f);

    const size_t tone = opt_tone_.selected_index();
    if (tone > 0 && (tone - 1) < dsp::tones::ctcss.size())
        tx->set_ctcss(dsp::tones::ctcss[tone - 1].frequency_hz);
    else
        tx->set_sub_tone_none();

    tx->set_audio_source(
        [this](float* out, size_t n) { return source_.render(out, n); });

    if (!tx->start()) {
        file_error("TX start failed.\nNeeds a USRP B200.");
        stop();
        return;
    }

    playing_ = true;
    menu_.set_highlighted(index);
    button_play_.set_text("Stop");
    text_status_.set("TX: " + core::filename(files_[index]).substr(0, 26));
    set_dirty();
}

void SoundboardView::stop() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_audio_source(nullptr);
    }
    source_.close();

    playing_ = false;
    progress_.set_value(0);
    button_play_.set_text("Play");
    set_idle_status();
    set_dirty();
}

void SoundboardView::on_finished() {
    if (check_random_.value() && files_.size() > 1) {
        lfsr_ = lfsr_iterate(lfsr_);
        const size_t next = lfsr_ % files_.size();
        start_tx(next);
    } else if (check_loop_.value()) {
        start_tx(playing_index_);
    } else {
        stop();
    }
}

void SoundboardView::set_idle_status() {
    if (files_.empty()) {
        text_status_.set("No sounds. RF needs USRP B200.");
    } else {
        text_status_.set(to_string_dec_uint(files_.size()) +
                         " sounds. RF needs USRP B200.");
    }
}

void SoundboardView::file_error(const std::string& message) {
    if (auto* nav = globals().nav)
        ui::display_modal(*nav, "Error", message);
}

#endif  // MB200_SOUNDBOARD_NO_REGISTRAR (SoundboardView)

}  // namespace app

#ifndef MB200_SOUNDBOARD_NO_REGISTRAR
#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_soundboard{{
    "soundboard",
    "Soundboard",
    app::Category::Transmit,
    ui::Color::yellow(),  /* upstream icon_color = ui::Color::yellow() */
    &ui::bitmap_icon_speaker,
    [] { return std::make_unique<app::SoundboardView>(); },
    false}};
}  // namespace
#endif  // MB200_SOUNDBOARD_NO_REGISTRAR
