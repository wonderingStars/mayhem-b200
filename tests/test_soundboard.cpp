/*
 * mayhem-b200 — Soundboard tests.
 *
 * The deliverable is the front of proc_audiotx's pipeline reimplemented on the
 * host: read a mono WAV, promote 8-bit exactly as the firmware does
 * ((v - 0x80) * 256, io_wave.cpp / proc_audiotx.cpp), and resample the file
 * rate to the transmitter's 48 kHz audio rate. These tests pin:
 *
 *  - the dsp::Resampler ratio (input_rate / output_rate) and its output count,
 *  - the WAV -> float level (a full-scale int16 lands at +/-1, 8-bit promotes
 *    through the firmware's formula),
 *  - the resample count end-to-end through WavAudioSource for up-, down- and
 *    same-rate files, and its play cursor / finished latch,
 *  - the file enumeration filter: mono 8/16-bit .WAV only, "shopping_cart"
 *    names skipped, matching upstream's refresh_list().
 *
 * Expected values are derived from those specs, not from the code's own output.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include <process.h> /* _getpid: shared-temp names must be unique PER PROCESS */

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "audio_out.hpp"  /* audio::sample_rate */
#include "demod.hpp"      /* dsp::Resampler */
#include "fs_utils.hpp"
#include "ui_soundboard.hpp"
#include "wav_file.hpp"

using namespace mb200test;

namespace {

std::string make_dir(const char* name) {
    std::error_code ec;
    /* Per-process subdir: make_dir() remove_all()s this path, so a fixed
     * name here had one process deleting another's live fixture. */
    auto dir = std::filesystem::temp_directory_path(ec) /
               ("mb200_soundboard_" + std::to_string(_getpid())) / name;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

/* Writes a mono/stereo 8- or 16-bit WAV of `frames` frames using the project's
 * own writer, every single-channel sample set to `value`. */
bool write_wav(const std::string& path,
               uint32_t sample_rate,
               uint16_t channels,
               uint16_t bits,
               size_t frames,
               int16_t value) {
    core::WavFileWriter w;
    if (!w.create(path, sample_rate, "test", channels, bits))
        return false;
    std::vector<int16_t> samples(frames * channels, value);
    if (!w.write_samples(samples.data(), samples.size()))
        return false;
    return w.close();
}

void put_u32(std::ofstream& f, uint32_t v) {
    uint8_t b[4] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8),
                    static_cast<uint8_t>(v >> 16), static_cast<uint8_t>(v >> 24)};
    f.write(reinterpret_cast<const char*>(b), 4);
}
void put_u16(std::ofstream& f, uint16_t v) {
    uint8_t b[2] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    f.write(reinterpret_cast<const char*>(b), 2);
}

/* Hand-writes a canonical 44-byte-header PCM WAV — used for the 24-bit case the
 * project's writer intentionally will not produce. */
bool write_raw_wav(const std::string& path,
                   uint32_t sample_rate,
                   uint16_t channels,
                   uint16_t bits,
                   size_t frames) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    const uint16_t block_align = static_cast<uint16_t>(channels * bits / 8);
    const uint32_t data_size = static_cast<uint32_t>(frames) * block_align;
    f.write("RIFF", 4);
    put_u32(f, 36 + data_size);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    put_u32(f, 16);
    put_u16(f, 1);  /* PCM */
    put_u16(f, channels);
    put_u32(f, sample_rate);
    put_u32(f, sample_rate * block_align);
    put_u16(f, block_align);
    put_u16(f, bits);
    f.write("data", 4);
    put_u32(f, data_size);
    std::vector<uint8_t> zeros(data_size, 0);
    f.write(reinterpret_cast<const char*>(zeros.data()),
            static_cast<std::streamsize>(zeros.size()));
    return f.good();
}

/* Drains a WavAudioSource fully, returning every 48 kHz sample it produced. */
std::vector<float> drain(app::WavAudioSource& src) {
    std::vector<float> all;
    std::vector<float> buf(512);
    for (;;) {
        const size_t n = src.render(buf.data(), buf.size());
        all.insert(all.end(), buf.begin(), buf.begin() + n);
        if (n < buf.size()) break;
    }
    return all;
}

}  // namespace

/* --- dsp::Resampler, the ratio the whole pipeline rests on ----------------- */

TEST(soundboard_resampler_ratio_upsample) {
    dsp::Resampler r;
    r.configure(8000.0, 48000.0);
    CHECK_NEAR(r.ratio(), 8000.0 / 48000.0, 1e-9);

    /* One second of a constant at 8 kHz -> one second at 48 kHz. */
    std::vector<float> in(8000, 0.5f);
    std::vector<float> out;
    r.process(in.data(), in.size(), out);
    CHECK_NEAR(static_cast<double>(out.size()), 48000.0, 8.0);
    /* Interpolating equal values preserves the level exactly. */
    CHECK_NEAR(out[out.size() / 2], 0.5f, 1e-6);
}

TEST(soundboard_resampler_ratio_samerate) {
    dsp::Resampler r;
    r.configure(48000.0, 48000.0);
    CHECK_NEAR(r.ratio(), 1.0, 1e-12);

    std::vector<float> in(1000, -0.25f);
    std::vector<float> out;
    r.process(in.data(), in.size(), out);
    CHECK_EQ(out.size(), static_cast<size_t>(1000));
    CHECK_NEAR(out[500], -0.25f, 1e-6);
}

/* --- WavAudioSource end to end --------------------------------------------- */

TEST(soundboard_source_upsample_16bit) {
    const std::string dir = make_dir("up16");
    const std::string path = (std::filesystem::path(dir) / "clip.wav").string();

    /* 4800 mono 16-bit samples at 8 kHz -> expect 4800 * 48000/8000 = 28800. */
    const size_t frames = 4800;
    const int16_t v = 16000;
    CHECK(write_wav(path, 8000, 1, 16, frames, v));

    app::WavAudioSource src;
    CHECK(src.open(path));
    CHECK_EQ(src.wav_sample_rate(), static_cast<uint32_t>(8000));
    CHECK_EQ(src.total_samples(), static_cast<uint32_t>(frames));
    CHECK_NEAR(src.resample_ratio(), 8000.0 / 48000.0, 1e-9);

    const auto out = drain(src);

    const double expected = static_cast<double>(frames) *
                            static_cast<double>(audio::sample_rate) / 8000.0;
    CHECK_NEAR(static_cast<double>(out.size()), expected, expected * 0.001 + 2.0);

    /* Level: 16000 / 32768 = 0.48828125, unchanged by resampling a constant. */
    CHECK_NEAR(out[out.size() / 2], 16000.0f / 32768.0f, 1e-4);

    /* The whole file was consumed and the finished latch is set. */
    CHECK_EQ(src.samples_played(), static_cast<uint64_t>(frames));
    CHECK(src.finished());
    CHECK_NEAR(src.progress(), 1.0f, 1e-6);
}

TEST(soundboard_source_downsample_16bit) {
    const std::string dir = make_dir("down16");
    const std::string path = (std::filesystem::path(dir) / "clip.wav").string();

    /* 9600 samples at 96 kHz -> expect 9600 * 48000/96000 = 4800. */
    const size_t frames = 9600;
    CHECK(write_wav(path, 96000, 1, 16, frames, -8000));

    app::WavAudioSource src;
    CHECK(src.open(path));
    CHECK_NEAR(src.resample_ratio(), 96000.0 / 48000.0, 1e-9);

    const auto out = drain(src);
    const double expected = static_cast<double>(frames) *
                            static_cast<double>(audio::sample_rate) / 96000.0;
    CHECK_NEAR(static_cast<double>(out.size()), expected, expected * 0.001 + 2.0);
    CHECK_NEAR(out[out.size() / 2], -8000.0f / 32768.0f, 1e-4);
    CHECK(src.finished());
}

TEST(soundboard_source_8bit_promotion) {
    const std::string dir = make_dir("prom8");
    const std::string path = (std::filesystem::path(dir) / "clip.wav").string();

    /* 8192 == 32 * 256 round-trips cleanly through the 8-bit writer/reader:
     * write (8192/256)+0x80 = 0xA0, read (0xA0-0x80)*256 = 8192 -> 0.25. */
    const size_t frames = 2000;
    CHECK(write_wav(path, 22050, 1, 8, frames, 8192));

    app::WavAudioSource src;
    CHECK(src.open(path));
    CHECK_EQ(src.bits_per_sample(), static_cast<uint16_t>(8));

    const auto out = drain(src);
    CHECK(!out.empty());
    CHECK_NEAR(out[out.size() / 2], 0.25f, 1e-4);
    CHECK_EQ(src.samples_played(), static_cast<uint64_t>(frames));
}

TEST(soundboard_source_full_scale_level) {
    const std::string dir = make_dir("fs16");
    const std::string path = (std::filesystem::path(dir) / "clip.wav").string();

    /* Minimum int16 promotes to exactly -1.0. */
    CHECK(write_wav(path, 48000, 1, 16, 1000, -32768));

    app::WavAudioSource src;
    CHECK(src.open(path));
    CHECK_NEAR(src.resample_ratio(), 1.0, 1e-12);
    const auto out = drain(src);
    CHECK_EQ(out.size(), static_cast<size_t>(1000));
    CHECK_NEAR(out[500], -1.0f, 1e-6);
}

TEST(soundboard_source_open_rejects_stereo) {
    const std::string dir = make_dir("rej");
    const std::string path = (std::filesystem::path(dir) / "stereo.wav").string();
    CHECK(write_wav(path, 44100, 2, 16, 100, 1000));

    app::WavAudioSource src;
    CHECK(!src.open(path));  /* stereo is not playable */
}

/* --- file enumeration ------------------------------------------------------ */

TEST(soundboard_list_wavs_filters) {
    const std::string dir = make_dir("list");
    namespace fs = std::filesystem;

    /* Two playable clips. */
    CHECK(write_wav((fs::path(dir) / "good1.wav").string(), 8000, 1, 16, 50, 0));
    CHECK(write_wav((fs::path(dir) / "good2.wav").string(), 22050, 1, 8, 50, 0));
    /* Rejected: stereo, 24-bit, non-WAV, and the shopping-cart LF waveform. */
    CHECK(write_wav((fs::path(dir) / "stereo.wav").string(), 44100, 2, 16, 50, 0));
    CHECK(write_raw_wav((fs::path(dir) / "hires.wav").string(), 48000, 1, 24, 50));
    {
        std::ofstream t((fs::path(dir) / "notes.txt").string());
        t << "not a wav";
    }
    CHECK(write_wav((fs::path(dir) / "shopping_cart_lock.wav").string(),
                    8000, 1, 16, 50, 0));

    const auto found = app::soundboard_list_wavs(dir);

    CHECK_EQ(found.size(), static_cast<size_t>(2));
    if (found.size() == 2) {
        /* list_directory sorts case-insensitively, so good1 precedes good2. */
        CHECK_STR_EQ(core::filename(found[0]), "good1.wav");
        CHECK_STR_EQ(core::filename(found[1]), "good2.wav");
    }
}

TEST(soundboard_list_wavs_missing_dir) {
    const auto found =
        app::soundboard_list_wavs("Z:/no/such/mb200/soundboard/dir");
    CHECK(found.empty());
}
