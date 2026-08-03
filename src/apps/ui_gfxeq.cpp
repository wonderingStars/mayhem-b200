/*
 * mayhem-b200 — gfxEQ, the audio spectrum bar visualiser.
 *
 * Copyright (C) 2025 RocketGod, HTotoo (original app and widget)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_gfxeq.hpp"

#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cmath>

namespace app {

namespace {

/* Upstream's twenty (base, peak) pairs, in order — external/gfxeq/ui_gfxeq.hpp. */
const std::array<GfxEqTheme, 20> kThemes{{
    {ui::Color(255, 0, 255), ui::Color(255, 255, 255)},
    {ui::Color(0, 255, 0), ui::Color(255, 0, 0)},
    {ui::Color(0, 0, 255), ui::Color(255, 255, 0)},
    {ui::Color(255, 128, 0), ui::Color(255, 0, 128)},
    {ui::Color(128, 0, 255), ui::Color(0, 255, 255)},
    {ui::Color(255, 255, 0), ui::Color(0, 255, 128)},
    {ui::Color(255, 0, 0), ui::Color(0, 128, 255)},
    {ui::Color(0, 255, 128), ui::Color(255, 128, 255)},
    {ui::Color(128, 128, 128), ui::Color(255, 255, 255)},
    {ui::Color(255, 64, 0), ui::Color(0, 255, 64)},
    {ui::Color(0, 128, 128), ui::Color(255, 192, 0)},
    {ui::Color(0, 255, 0), ui::Color(0, 128, 0)},
    {ui::Color(32, 64, 32), ui::Color(0, 255, 0)},
    {ui::Color(64, 0, 128), ui::Color(255, 0, 255)},
    {ui::Color(0, 64, 0), ui::Color(0, 255, 128)},
    {ui::Color(255, 255, 255), ui::Color(0, 0, 255)},
    {ui::Color(128, 0, 0), ui::Color(255, 128, 0)},
    {ui::Color(0, 128, 255), ui::Color(255, 255, 128)},
    {ui::Color(64, 64, 64), ui::Color(255, 0, 0)},
    {ui::Color(255, 192, 0), ui::Color(0, 64, 128)},
}};

/* Frames without a fresh block before the app admits it is not receiving.
 * ~1 s at the 60 Hz UI loop. */
constexpr uint32_t kStarvedFramesLimit = 60;

}  // namespace

size_t gfxeq_theme_count() {
    return kThemes.size();
}

const GfxEqTheme& gfxeq_theme(size_t index) {
    return kThemes[index % kThemes.size()];
}

/* --- AudioSpectrumAnalyzer ------------------------------------------------- */

AudioSpectrumAnalyzer::AudioSpectrumAnalyzer() {
    work_.resize(kFftSize);
}

void AudioSpectrumAnalyzer::reset() {
    pending_count_ = 0;
    buffer_.fill(0.0f);
    spectrum_ = ui::AudioSpectrum{};
}

uint8_t AudioSpectrumAnalyzer::db_to_byte(float db) {
    const float v = db * kMagScale + 255.0f;
    /* Written as a negated comparison so a NaN lands on 0 rather than falling
     * through to the cast. */
    if (!(v > 0.0f)) return 0;
    if (v >= 255.0f) return 255;
    return static_cast<uint8_t>(v);  /* truncation, as upstream */
}

size_t AudioSpectrumAnalyzer::bin_for_hz(float hz) {
    if (hz <= 0.0f) return 0;
    return static_cast<size_t>(hz / kBinHz);
}

bool AudioSpectrumAnalyzer::feed(const float* samples, size_t count) {
    if (samples == nullptr || count == 0) return false;

    bool updated = false;
    size_t consumed = 0;

    while (consumed < count) {
        const size_t room = kFftSize - pending_count_;
        const size_t n = std::min(count - consumed, room);
        std::copy(samples + consumed, samples + consumed + n,
                  buffer_.data() + pending_count_);
        pending_count_ += n;
        consumed += n;

        if (pending_count_ == kFftSize) {
            analyze(buffer_.data());
            pending_count_ = 0;
            updated = true;
        }
    }

    return updated;
}

void AudioSpectrumAnalyzer::analyze(const float* block) {
    /* Upstream converts the real audio to "complex just so the FFT can be done".
     * Same here: no window, which is also upstream's choice (its Hamming call is
     * commented out). */
    for (size_t i = 0; i < kFftSize; i++)
        work_[i] = dsp::cfloat{block[i], 0.0f};

    fft_.transform(work_);

    /* Only the first 128 bins are published — 0 to 47.6 kHz — exactly the range
     * upstream's AudioSpectrum::db covers. */
    for (size_t i = 0; i < spectrum_.db.size(); i++) {
        const float mag = std::abs(work_[i]) * kFftMagScale;
        const float db = (mag > 0.0f) ? (20.0f * std::log10(mag)) : -1000.0f;
        spectrum_.db[i] = db_to_byte(db);
    }
}

/* --- WfmAudioTap ----------------------------------------------------------- */

void WfmAudioTap::configure(double input_rate_hz) {
    configured_ = false;
    input_rate_ = input_rate_hz;
    if (!(input_rate_ > 0.0)) return;

    /* Decimate towards upstream's 384 kHz demodulator input. */
    long long cdec = std::llround(input_rate_ / kDemodRate);
    if (cdec < 1) cdec = 1;
    channel_decimation_ = static_cast<size_t>(cdec);
    channel_rate_ = input_rate_ / static_cast<double>(channel_decimation_);

    /* Channel filter: half the 180 kHz WFM bandwidth each side of DC, with the
     * transition band filling most of what is left before the decimated
     * Nyquist, which keeps the tap count sane at 1.5 Msps. */
    const double cutoff = kChannelBandwidth / 2.0;
    const double nyquist = channel_rate_ / 2.0;
    const double transition = std::max((nyquist - cutoff) * 0.9, 1000.0);
    channel_.configure(dsp::design_lowpass(cutoff, transition, input_rate_, 60.0, 1023),
                       channel_decimation_);

    fm_.configure(static_cast<float>(channel_rate_), static_cast<float>(kDeviation));

    /* Audio side down to 96 kHz. Note the corner sits just under the 48 kHz
     * output Nyquist rather than at 15 kHz: upstream taps the spectrum *before*
     * its audio filter, so the 19 kHz pilot and the 38 kHz stereo subcarrier are
     * meant to be visible. */
    long long adec = std::llround(channel_rate_ / AudioSpectrumAnalyzer::kAudioRate);
    if (adec < 1) adec = 1;
    audio_decimation_ = static_cast<size_t>(adec);
    audio_rate_ = channel_rate_ / static_cast<double>(audio_decimation_);

    const double audio_cutoff = audio_rate_ * 0.45;
    const double audio_transition = std::max(audio_rate_ * 0.05, 1000.0);
    audio_.configure(
        dsp::design_lowpass(audio_cutoff, audio_transition, channel_rate_, 60.0, 511),
        audio_decimation_);

    /* The integer decimations only land on exactly 96 kHz when the device gave
     * us the rate we asked for. When it did not, trim the rest fractionally —
     * the bins must be 375 Hz wide or GraphEq's band table means nothing. */
    needs_resample_ =
        std::fabs(audio_rate_ - AudioSpectrumAnalyzer::kAudioRate) > 0.5;
    if (needs_resample_)
        resampler_.configure(audio_rate_, AudioSpectrumAnalyzer::kAudioRate);

    nco_.set_frequency(-offset_hz_, input_rate_);

    configured_ = true;
}

void WfmAudioTap::reset() {
    nco_.reset();
    channel_.reset();
    fm_.reset();
    audio_.reset();
    resampler_.reset();
}

void WfmAudioTap::set_offset(double offset_hz) {
    offset_hz_ = offset_hz;
    /* Mixing by -offset brings a signal sitting at +offset down to baseband. */
    nco_.set_frequency(-offset_hz_, input_rate_);
}

size_t WfmAudioTap::process(const dsp::cfloat* in, size_t count, std::vector<float>& out) {
    out.clear();
    if (!configured_ || in == nullptr || count == 0) return 0;

    mixed_.resize(count);
    nco_.mix(in, mixed_.data(), count);

    channel_out_.clear();
    channel_.process(mixed_.data(), count, channel_out_);
    if (channel_out_.empty()) return 0;

    demodulated_.clear();
    fm_.process(channel_out_.data(), channel_out_.size(), demodulated_);
    if (demodulated_.empty()) return 0;

    decimated_.clear();
    audio_.process(demodulated_.data(), demodulated_.size(), decimated_);
    if (decimated_.empty()) return 0;

    if (needs_resample_)
        resampler_.process(decimated_.data(), decimated_.size(), out);
    else
        out.assign(decimated_.begin(), decimated_.end());

    return out.size();
}

/* --- GfxEqView ------------------------------------------------------------- */

GfxEqView::GfxEqView()
    : receiver_{*globals().receiver} {
    add_children({&field_frequency_,
                  &labels_,
                  &field_gain_,
                  &field_volume_,
                  &button_mood_,
                  &graph_,
                  &text_status_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    field_frequency_.set_value(frequency_value_, false);
    field_frequency_.on_change = [this](uint64_t hz) {
        frequency_value_ = hz;
        receiver_.set_target_frequency(hz);
    };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    field_volume_.set_value(receiver_.volume(), false);
    field_volume_.on_change = [this](int32_t v) {
        receiver_.set_volume(static_cast<uint8_t>(v));
    };

    button_mood_.on_select = [this](ui::Button&) { cycle_theme(); };

    apply_theme();
    text_status_.set("starting");
}

GfxEqView::~GfxEqView() {
    /* The radio is left streaming, as the other receive apps do — the sampling
     * rate this app selected stays until the next app sets its own. */
}

void GfxEqView::on_show() {
    View::on_show();
    field_frequency_.focus();

    /* Upstream's constructor: WFM, configuration 1 (the 180 kHz filter), and a
     * capture rate wide enough to hold it. */
    receiver_.set_mode(radio::ReceiverModel::Mode::WidebandFMAudio);
    receiver_.set_wfm_configuration(radio::ReceiverModel::WfmConfig::Narrow180k);
    receiver_.set_sampling_rate(kSampleRate);
    receiver_.set_target_frequency(frequency_value_);

    if (!receiver_.running()) receiver_.start();

    /* The device may not have granted the rate we asked for. */
    tap_.configure(receiver_.sampling_rate());
    analyzer_.reset();
    starved_frames_ = 0;
    offset_valid_ = false;

    update_status();
}

void GfxEqView::cycle_theme() {
    current_theme_ = static_cast<uint32_t>((current_theme_ + 1) % gfxeq_theme_count());
    apply_theme();
}

void GfxEqView::apply_theme() {
    const GfxEqTheme& t = gfxeq_theme(current_theme_);
    graph_.set_theme(t.base, t.peak);
}

void GfxEqView::update_status() {
    std::string line;

    if (!receiver_.running()) {
        line = STR_COLOR_RED "radio not streaming";
    } else if (!tap_.configured()) {
        line = STR_COLOR_RED "no sample rate from device";
    } else if (starved_frames_ >= kStarvedFramesLimit) {
        line = STR_COLOR_RED "no samples from receiver";
    } else {
        line = "WFM " + to_string_dec_uint(static_cast<uint32_t>(tap_.audio_rate() / 1000)) +
               "k tap  mood " +
               to_string_dec_uint(static_cast<uint32_t>(current_theme_ + 1)) + "/" +
               to_string_dec_uint(static_cast<uint32_t>(gfxeq_theme_count()));
    }

    text_status_.set(line);
}

void GfxEqView::on_frame_sync() {
    View::on_frame_sync();
    frame_counter_++;

    /* take_spectrum_samples() hands back the raw band as captured, before the
     * receiver's own NCO, so the app has to apply the same shift itself. */
    if (auto* r = globals().radio) {
        const double offset =
            static_cast<double>(receiver_.target_frequency()) - r->rx_frequency();
        if (!offset_valid_ || offset != last_offset_) {
            tap_.set_offset(offset);
            last_offset_ = offset;
            offset_valid_ = true;
        }
    }

    if (receiver_.take_spectrum_samples(samples_, kTapSamples) && !samples_.empty()) {
        if (tap_.process(samples_.data(), samples_.size(), audio_) > 0) {
            if (analyzer_.feed(audio_.data(), audio_.size())) {
                graph_.update_audio_spectrum(analyzer_.spectrum());
                starved_frames_ = 0;
            }
        }
    } else if (starved_frames_ < kStarvedFramesLimit) {
        starved_frames_++;
    }

    if ((frame_counter_ % 15) == 0) update_status();
}

bool GfxEqView::on_key(const ui::KeyEvent key) {
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

bool GfxEqView::on_encoder(const ui::EncoderEvent delta) {
    field_frequency_.on_encoder(delta);
    return true;
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* app_location_t::RX and Color::green() in upstream's application_information. */
const app::Registrar reg_gfxeq{{"gfxeq", "gfxEQ", app::Category::Receive,
                                ui::Color::green(), &ui::bitmap_icon_speaker,
                                [] { return std::make_unique<app::GfxEqView>(); }}};
}  // namespace
