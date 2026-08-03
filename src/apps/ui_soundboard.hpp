/*
 * mayhem-b200 — Soundboard: transmit a .WAV file as audio-modulated RF.
 *
 * Ported from firmware/application/external/soundboard/soundboard_app.* . The
 * firmware streams the WAV to the M4's proc_audiotx image, which does a
 * zero-order-hold resample from the file's sample rate up to the 1.536 MHz
 * baseband, an optional CTCSS sub-tone mix, and an FM modulation
 * (proc_audiotx.cpp: fm_delta = deviation * 0xFFFFFF / baseband_fs). Here the
 * host radio::TransmitterModel already owns the modulator chain and pulls an
 * AudioSource at audio::sample_rate (48 kHz); the app's job is therefore only
 * the front of proc_audiotx's pipeline — read the WAV, promote 8-bit the way
 * the firmware does ((v - 0x80) * 256), and resample the file rate to 48 kHz
 * with dsp::Resampler before handing floats to the transmitter, which does the
 * FM/AM modulation and the lift to the B200 rate.
 *
 *   WAV (mono 8/16-bit @ file rate) -> int16 -> float[-1,1]
 *        -> dsp::Resampler(file_rate -> 48 kHz) -> TransmitterModel::AudioSource
 *
 * Upstream is FM only (its AudioTXConfig sets AM/DSB/USB/LSB all false); the
 * host transmitter supports more, so a small mode selector offers NFM (the
 * upstream default, 5 kHz deviation), WFM and AM. Transmit never begins on its
 * own — only the Play button starts it — and the view says on screen that RF
 * output needs a USRP B200.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SOUNDBOARD_H__
#define __MB200_UI_SOUNDBOARD_H__

#include "../core/wav_file.hpp"
#include "../dsp/demod.hpp"  /* dsp::Resampler */
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_menu.hpp"
#include "ui_widget.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* Enumerates the playable WAV files in `dir`: regular files with a .WAV
 * extension (case-insensitive) that are mono PCM at 8 or 16 bits per sample —
 * the exact constraint upstream's refresh_list() applies. Files whose name
 * contains "shopping_cart" are skipped, as upstream skips them (the shopping
 * cart lock app parks its LF waveform there). Returns full paths, sorted the
 * way core::list_directory sorts. Static so the test can exercise it directly. */
std::vector<std::string> soundboard_list_wavs(const std::string& dir);

/* The front of proc_audiotx's pipeline: reads a mono WAV and produces float
 * audio at audio::sample_rate (48 kHz) for TransmitterModel::AudioSource.
 *
 * open()/close() run on the UI thread and only while the transmitter is
 * stopped (its DSP thread joined); render() runs on the transmitter's DSP
 * thread. The only values shared across the two threads — the play cursor and
 * the finished flag — are atomics, read by the UI for the progress bar and the
 * end-of-file handling. */
class WavAudioSource {
   public:
    /* Single-channel WAV samples pulled from disk per refill. */
    static constexpr size_t read_chunk = 4096;

    WavAudioSource() = default;

    WavAudioSource(const WavAudioSource&) = delete;
    WavAudioSource& operator=(const WavAudioSource&) = delete;

    /* Opens `path` and configures the resampler for its rate -> 48 kHz. False
     * on a file that will not open or is not mono 8/16-bit PCM. */
    bool open(const std::string& path);
    void close();
    bool is_open() const { return reader_.is_open(); }

    uint32_t wav_sample_rate() const { return wav_rate_; }
    uint16_t bits_per_sample() const { return reader_.bits_per_sample(); }

    /* Resampler ratio, file_rate / 48 kHz. Below 1 upsamples (the usual case
     * for an 8 kHz clip); above 1 downsamples a 96 kHz clip. */
    double resample_ratio() const { return resampler_.ratio(); }

    /* Total single-channel samples in the file, for the progress bar's max. */
    uint32_t total_samples() const { return total_samples_; }

    /* DSP-thread callback: fills `out` with up to `count` samples in [-1, 1] at
     * 48 kHz and returns how many it wrote. A short return means the file has
     * ended; finished() then latches true once the resampler has been drained. */
    size_t render(float* out, size_t count);

    /* UI-thread readouts. */
    uint64_t samples_played() const { return samples_played_.load(); }
    bool finished() const { return finished_.load(); }
    float progress() const;

   private:
    core::WavFileReader reader_{};
    dsp::Resampler resampler_{};
    uint32_t wav_rate_{48000};
    uint32_t total_samples_{0};

    /* Resampled output waiting to be handed out, and how much has been. */
    std::vector<float> pending_{};
    size_t pending_pos_{0};

    /* Scratch buffers reused across refills to avoid per-call allocation. */
    std::vector<int16_t> read_buf_{};
    std::vector<float> in_buf_{};

    bool eof_{false};
    std::atomic<uint64_t> samples_played_{0};
    std::atomic<bool> finished_{false};
};

class SoundboardView : public ui::View {
   public:
    SoundboardView();
    ~SoundboardView() override;

    SoundboardView(const SoundboardView&) = delete;
    SoundboardView& operator=(const SoundboardView&) = delete;

    std::string title() const override { return "Soundbrd TX"; }

    void focus() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    /* Modulation mode chosen by opt_mode_ (index order matches the options). */
    enum class Mod { NFM = 0, WFM = 1, AM = 2 };

    void refresh_list();
    void start_tx(size_t index);
    void stop();
    void on_finished();
    void set_idle_status();
    void file_error(const std::string& message);
    bool is_playing() const { return playing_; }

    std::string wav_directory_{};
    std::vector<std::string> files_{};
    size_t playing_index_{0};
    bool playing_{false};

    /* LFSR state for the Random shuffle, ported from firmware/common/
     * lfsr_random.cpp (Fibonacci, length 31, taps 31/18). */
    uint32_t lfsr_{1};

    /* --- widgets --- */
    ui::Labels labels_{
        {{0, 4}, "Freq", ui::Color::light_grey()},
        {{136, 4}, "Mod", ui::Color::light_grey()},
        {{0, 24}, "Tone", ui::Color::light_grey()},
    };

    ui::FrequencyField field_freq_{{40, 0}};

    ui::OptionsField opt_mode_{
        {168, 4},
        4,
        {{"NFM", 0}, {"WFM", 1}, {"AM ", 2}}};

    /* Built in the constructor from dsp::tones::ctcss, with a "None" first. */
    ui::OptionsField opt_tone_{{40, 24}, 12, {}};

    ui::MenuView menu_{{0, 44, 240, 168}, true};

    ui::Text text_empty_{{20, 112, 200, 16}, "No WAV files in data/WAV"};

    ui::Text text_status_{{0, 214, 240, 16}, ""};

    ui::ProgressBar progress_{{0, 234, 240, 8}};

    ui::Checkbox check_loop_{{0, 248}, 4, "Loop"};
    ui::Checkbox check_random_{{112, 248}, 6, "Random"};

    ui::Button button_play_{{0, 272, 116, 30}, "Play"};
    ui::Button button_refresh_{{124, 272, 116, 30}, "Refresh"};

    WavAudioSource source_{};
};

}  // namespace app

#endif /*__MB200_UI_SOUNDBOARD_H__*/
