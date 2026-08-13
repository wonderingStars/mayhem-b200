/*
 * mayhem-b200 — audio and image file IO tests.
 *
 * Expected values come from the RIFF/WAVE and BMP specifications and from the
 * upstream PortaPack implementations these were ported from:
 *
 *  - firmware/application/io_wave.hpp declares the exact byte layout the
 *    firmware writes: a 44-byte canonical header (`header_t`) and a 104-byte
 *    LIST/INFO chunk (`tags_t`) with a 12-byte IART field holding "PortaPack"
 *    and a 64-byte INAM field holding the title. The header's RIFF size is
 *    `sizeof(header_t) + data + info - 8`.
 *  - firmware/common/utility.hpp `ms_duration()` is
 *    `bytes * 1000 / (bytes_per_sample * sample_rate)`.
 *  - firmware/common/bmpfile.cpp `create()` writes a 54-byte header with
 *    bpp 24, planes 1, compression 0, image_data 0x36, BIH_size 0x28,
 *    h_res/v_res 100 and a negative height, and rows padded with
 *    `(width * 3 + 3) & ~3`.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include <process.h> /* _getpid: shared-temp names must be unique PER PROCESS */

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "bmp_file.hpp"
#include "ui.hpp"
#include "wav_file.hpp"

namespace {

std::string temp_path(const char* name) {
    std::error_code ec;
    /* Per-process subdir; see the PID note in the other test fixtures. */
    auto dir = std::filesystem::temp_directory_path(ec) /
               ("mb200_media_io_" + std::to_string(_getpid()));
    std::filesystem::create_directories(dir, ec);
    return (dir / name).string();
}

std::vector<uint8_t> slurp(const std::string& path) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) return {};
    return std::vector<uint8_t>{std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>()};
}

bool spit(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream f(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    if (!bytes.empty())
        f.write(reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    return f.good();
}

uint16_t le16(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<uint16_t>(b[off] | (b[off + 1] << 8));
}

uint32_t le32(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<uint32_t>(b[off]) |
           (static_cast<uint32_t>(b[off + 1]) << 8) |
           (static_cast<uint32_t>(b[off + 2]) << 16) |
           (static_cast<uint32_t>(b[off + 3]) << 24);
}

int32_t le32s(const std::vector<uint8_t>& b, size_t off) {
    return static_cast<int32_t>(le32(b, off));
}

bool tag_at(const std::vector<uint8_t>& b, size_t off, const char* tag) {
    if (off + 4 > b.size()) return false;
    return std::memcmp(b.data() + off, tag, 4) == 0;
}

/* A deterministic pattern that reaches both 16-bit extremes and changes sign. */
std::vector<int16_t> pattern_s16(size_t n) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; ++i)
        v[i] = static_cast<int16_t>((i * 2731) % 65536 - 32768);
    if (n > 2) {
        v[0] = -32768;
        v[1] = 32767;
        v[2] = 0;
    }
    return v;
}

/* Minimal hand-built 24-bit BMP so the reader is tested against bytes this
 * project's writer did not produce. Rows are given top first. */
std::vector<uint8_t> make_bmp24(uint32_t width,
                                uint32_t height,
                                bool bottom_up,
                                const std::vector<std::vector<uint8_t>>& rows_bgr) {
    const uint32_t bpr = (width * 3u + 3u) & ~3u;
    std::vector<uint8_t> b(54 + static_cast<size_t>(bpr) * height, 0);

    b[0] = 'B';
    b[1] = 'M';
    const uint32_t size = static_cast<uint32_t>(b.size());
    b[2] = static_cast<uint8_t>(size);
    b[3] = static_cast<uint8_t>(size >> 8);
    b[4] = static_cast<uint8_t>(size >> 16);
    b[5] = static_cast<uint8_t>(size >> 24);
    b[10] = 54; /* image_data */
    b[14] = 40; /* BIH_size */
    b[18] = static_cast<uint8_t>(width);
    b[19] = static_cast<uint8_t>(width >> 8);
    const int32_t h = bottom_up ? static_cast<int32_t>(height) : -static_cast<int32_t>(height);
    const uint32_t hu = static_cast<uint32_t>(h);
    b[22] = static_cast<uint8_t>(hu);
    b[23] = static_cast<uint8_t>(hu >> 8);
    b[24] = static_cast<uint8_t>(hu >> 16);
    b[25] = static_cast<uint8_t>(hu >> 24);
    b[26] = 1;  /* planes */
    b[28] = 24; /* bpp */

    for (uint32_t y = 0; y < height; ++y) {
        const uint32_t stored = bottom_up ? (height - y - 1) : y;
        const size_t off = 54 + static_cast<size_t>(stored) * bpr;
        for (size_t i = 0; i < rows_bgr[y].size(); ++i)
            b[off + i] = rows_bgr[y][i];
    }
    return b;
}

}  // namespace

/* =========================================================================
 * WAV
 * ====================================================================== */

TEST(wav_write_16bit_mono_header_matches_upstream_layout) {
    const auto path = temp_path("hdr16.wav");
    const auto samples = pattern_s16(1200);

    core::WavFileWriter w;
    CHECK(w.create(path, 8000, "1234567Hz"));
    CHECK(w.write_samples(samples.data(), samples.size()));
    CHECK(w.close());

    const auto b = slurp(path);

    /* 44-byte header + 2400 bytes of data + 104-byte LIST/INFO chunk. */
    CHECK_EQ(b.size(), static_cast<size_t>(44 + 2400 + 104));

    CHECK(tag_at(b, 0, "RIFF"));
    /* header_t: sizeof(header_t) + data + info - 8 == file size - 8. */
    CHECK_EQ(le32(b, 4), 44u + 2400u + 104u - 8u);
    CHECK(tag_at(b, 8, "WAVE"));

    CHECK(tag_at(b, 12, "fmt "));
    CHECK_EQ(le32(b, 16), 16u);    /* fmt cksize */
    CHECK_EQ(le16(b, 20), 1u);     /* wFormatTag: PCM */
    CHECK_EQ(le16(b, 22), 1u);     /* nChannels */
    CHECK_EQ(le32(b, 24), 8000u);  /* nSamplesPerSec */
    CHECK_EQ(le32(b, 28), 16000u); /* nAvgBytesPerSec == rate * blockalign */
    CHECK_EQ(le16(b, 32), 2u);     /* nBlockAlign */
    CHECK_EQ(le16(b, 34), 16u);    /* wBitsPerSample */

    CHECK(tag_at(b, 36, "data"));
    /* Upstream patches this after appending the tags and so overstates it by
     * 104; the real data length belongs here. */
    CHECK_EQ(le32(b, 40), 2400u);

    /* tags_t, byte for byte. */
    const size_t list = 44 + 2400;
    CHECK(tag_at(b, list, "LIST"));
    CHECK_EQ(le32(b, list + 4), 96u); /* sizeof(tags_t) - 8 */
    CHECK(tag_at(b, list + 8, "INFO"));
    CHECK(tag_at(b, list + 12, "IART"));
    CHECK_EQ(le32(b, list + 16), 12u);
    CHECK_STR_EQ(std::string(reinterpret_cast<const char*>(b.data() + list + 20)), "PortaPack");
    CHECK(tag_at(b, list + 32, "INAM"));
    CHECK_EQ(le32(b, list + 36), 64u);
    CHECK_STR_EQ(std::string(reinterpret_cast<const char*>(b.data() + list + 40)), "1234567Hz");
}

TEST(wav_roundtrip_16bit_mono_preserves_samples_and_fields) {
    const auto path = temp_path("rt16.wav");
    const auto samples = pattern_s16(1200);

    {
        core::WavFileWriter w;
        CHECK(w.create(path, 8000, "round trip"));
        CHECK(w.write_samples(samples.data(), samples.size()));
        CHECK(w.close());
    }

    core::WavFileReader r;
    CHECK(r.open(path));
    CHECK_EQ(r.channels(), 1u);
    CHECK_EQ(r.sample_rate(), 8000u);
    CHECK_EQ(r.bits_per_sample(), 16u);
    CHECK_EQ(r.bytes_per_sample(), 2u);
    CHECK_EQ(r.data_size(), 2400u);
    CHECK_EQ(r.sample_count(), 1200u);
    CHECK_EQ(r.frame_count(), 1200u);
    CHECK_STR_EQ(r.title(), "round trip");

    std::vector<int16_t> back(samples.size(), 0);
    CHECK_EQ(r.read_samples(back.data(), back.size()), samples.size());
    CHECK(back == samples);

    /* Nothing beyond the data chunk: the LIST bytes must not read as audio. */
    int16_t extra = 0;
    CHECK_EQ(r.read_samples(&extra, 1), static_cast<size_t>(0));
}

TEST(wav_roundtrip_8bit_mono_preserves_bytes) {
    const auto path = temp_path("rt8.wav");

    std::vector<uint8_t> raw(1200);
    for (size_t i = 0; i < raw.size(); ++i) raw[i] = static_cast<uint8_t>(i % 256);
    raw[0] = 0;
    raw[1] = 255;
    raw[2] = 128;

    {
        core::WavFileWriter w;
        CHECK(w.create(path, 8000, "eight bit", 1, 8));
        CHECK(w.write(raw.data(), raw.size()));
        CHECK(w.close());
    }

    const auto b = slurp(path);
    CHECK_EQ(b.size(), static_cast<size_t>(44 + 1200 + 104));
    CHECK_EQ(le16(b, 32), 1u);    /* nBlockAlign for 8-bit mono */
    CHECK_EQ(le16(b, 34), 8u);    /* wBitsPerSample */
    CHECK_EQ(le32(b, 28), 8000u); /* nAvgBytesPerSec */
    CHECK_EQ(le32(b, 40), 1200u); /* data cksize */

    core::WavFileReader r;
    CHECK(r.open(path));
    CHECK_EQ(r.bits_per_sample(), 8u);
    CHECK_EQ(r.bytes_per_sample(), 1u);
    CHECK_EQ(r.sample_count(), 1200u);
    CHECK_STR_EQ(r.title(), "eight bit");

    std::vector<uint8_t> back(raw.size(), 0);
    CHECK_EQ(r.read(back.data(), back.size()), raw.size());
    CHECK(back == raw);

    /* The WAV viewer's promotion of 8-bit PCM: (v - 0x80) * 256. */
    r.rewind();
    int16_t promoted[3] = {0, 0, 0};
    CHECK_EQ(r.read_samples(promoted, 3), static_cast<size_t>(3));
    CHECK_EQ(promoted[0], static_cast<int16_t>((0 - 0x80) * 256));
    CHECK_EQ(promoted[1], static_cast<int16_t>((255 - 0x80) * 256));
    CHECK_EQ(promoted[2], static_cast<int16_t>(0));
}

TEST(wav_odd_length_8bit_data_is_aligned_so_tags_stay_readable) {
    /* RIFF chunks start on even offsets. An odd 8-bit payload needs the
     * alignment byte or the LIST chunk after it cannot be parsed. */
    const auto path = temp_path("odd8.wav");
    std::vector<uint8_t> raw(1001, 0x40);

    {
        core::WavFileWriter w;
        CHECK(w.create(path, 8000, "odd", 1, 8));
        CHECK(w.write(raw.data(), raw.size()));
        CHECK(w.close());
    }

    const auto b = slurp(path);
    CHECK_EQ(b.size(), static_cast<size_t>(44 + 1001 + 1 + 104));
    CHECK_EQ(le32(b, 4), 44u + 1001u + 1u + 104u - 8u);
    CHECK_EQ(le32(b, 40), 1001u);
    CHECK_EQ(b[44 + 1001], 0u); /* the pad byte */

    core::WavFileReader r;
    CHECK(r.open(path));
    CHECK_EQ(r.data_size(), 1001u);
    CHECK_STR_EQ(r.title(), "odd");
}

TEST(wav_duration_and_counts) {
    const auto path = temp_path("dur.wav");
    const auto samples = pattern_s16(1200);

    {
        core::WavFileWriter w;
        CHECK(w.create(path, 8000, "dur"));
        CHECK(w.write_samples(samples.data(), samples.size()));
        /* The writer reports the same figures before the file is closed. */
        CHECK_EQ(w.sample_count(), 1200u);
        CHECK_EQ(w.ms_duration(), 150u);
        CHECK(w.close());
    }

    core::WavFileReader r;
    CHECK(r.open(path));
    /* 1200 samples at 8000 Hz is 150 ms. Upstream's formula,
     * data_size * 1000 / (bytes_per_sample * sample_rate), gives
     * 2400 * 1000 / (2 * 8000) == 150 as well. */
    CHECK_EQ(r.ms_duration(), 150u);
    CHECK_EQ(r.sample_count(), 1200u);
    CHECK_EQ(r.frame_count(), 1200u);
}

TEST(wav_stereo_counts_frames_and_duration_separately) {
    const auto path = temp_path("stereo.wav");
    /* 600 frames of 2 channels: 1200 single-channel samples, 2400 bytes. */
    const auto samples = pattern_s16(1200);

    {
        core::WavFileWriter w;
        CHECK(w.create(path, 8000, "stereo", 2, 16));
        CHECK(w.write_samples(samples.data(), samples.size()));
        CHECK(w.close());
    }

    const auto b = slurp(path);
    CHECK_EQ(le16(b, 22), 2u);     /* nChannels */
    CHECK_EQ(le16(b, 32), 4u);     /* nBlockAlign == channels * 2 */
    CHECK_EQ(le32(b, 28), 32000u); /* nAvgBytesPerSec */

    core::WavFileReader r;
    CHECK(r.open(path));
    CHECK_EQ(r.channels(), 2u);
    /* sample_count() keeps upstream's meaning: single-channel samples. */
    CHECK_EQ(r.sample_count(), 1200u);
    CHECK_EQ(r.frame_count(), 600u);
    /* 600 frames at 8000 Hz is 75 ms. Upstream's channel-blind formula would
     * say 150; this divides by the channel count too. */
    CHECK_EQ(r.ms_duration(), 75u);
}

TEST(wav_seek_and_rewind) {
    const auto path = temp_path("seek.wav");
    const auto samples = pattern_s16(1000);

    {
        core::WavFileWriter w;
        CHECK(w.create(path, 48000, "seek"));
        CHECK(w.write_samples(samples.data(), samples.size()));
        CHECK(w.close());
    }

    core::WavFileReader r;
    CHECK(r.open(path));

    CHECK(r.data_seek(500));
    CHECK_EQ(r.tell_sample(), static_cast<uint64_t>(500));
    int16_t s = 0;
    CHECK_EQ(r.read_samples(&s, 1), static_cast<size_t>(1));
    CHECK_EQ(s, samples[500]);

    r.rewind();
    CHECK_EQ(r.tell_sample(), static_cast<uint64_t>(0));
    CHECK_EQ(r.read_samples(&s, 1), static_cast<size_t>(1));
    CHECK_EQ(s, samples[0]);

    /* Seeking to the very end is allowed and yields nothing; past it fails. */
    CHECK(r.data_seek(1000));
    CHECK_EQ(r.read_samples(&s, 1), static_cast<size_t>(0));
    CHECK(!r.data_seek(1001));

    /* A block read never crosses the end of the data chunk. */
    CHECK(r.data_seek(995));
    std::vector<int16_t> tail(50, 0);
    CHECK_EQ(r.read_samples(tail.data(), tail.size()), static_cast<size_t>(5));
}

TEST(wav_truncated_header_is_rejected) {
    /* Nothing at all. */
    const auto empty = temp_path("trunc_empty.wav");
    CHECK(spit(empty, {}));
    core::WavFileReader r0;
    CHECK(!r0.open(empty));
    CHECK(!r0.is_open());

    /* Shorter than 'RIFF' size 'WAVE'. */
    const auto tiny = temp_path("trunc_tiny.wav");
    CHECK(spit(tiny, {'R', 'I', 'F', 'F', 0, 0, 0, 0}));
    core::WavFileReader r1;
    CHECK(!r1.open(tiny));

    /* Right form, no chunks at all. */
    const auto bare = temp_path("trunc_bare.wav");
    CHECK(spit(bare, {'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'A', 'V', 'E'}));
    core::WavFileReader r2;
    CHECK(!r2.open(bare));

    /* Build a good file, then chop it at telling points. */
    const auto good = temp_path("trunc_src.wav");
    {
        const auto samples = pattern_s16(64);
        core::WavFileWriter w;
        CHECK(w.create(good, 8000, "chop"));
        CHECK(w.write_samples(samples.data(), samples.size()));
        CHECK(w.close());
    }
    const auto full = slurp(good);
    CHECK(full.size() > 44);

    /* fmt chunk header present, its body missing. */
    const auto cut20 = temp_path("trunc_20.wav");
    CHECK(spit(cut20, {full.begin(), full.begin() + 20}));
    core::WavFileReader r3;
    CHECK(!r3.open(cut20));

    /* fmt complete, no data chunk. */
    const auto cut36 = temp_path("trunc_36.wav");
    CHECK(spit(cut36, {full.begin(), full.begin() + 36}));
    core::WavFileReader r4;
    CHECK(!r4.open(cut36));

    /* Not a RIFF file. */
    const auto junk = temp_path("trunc_junk.wav");
    CHECK(spit(junk, std::vector<uint8_t>(200, 0xa5)));
    core::WavFileReader r5;
    CHECK(!r5.open(junk));

    /* A missing file. */
    core::WavFileReader r6;
    CHECK(!r6.open(temp_path("does_not_exist.wav")));

    /* A rejected open leaves the reader reusable. */
    CHECK(r6.open(good));
    CHECK_EQ(r6.sample_count(), 64u);
}

TEST(wav_overstated_data_chunk_is_clamped_to_the_file) {
    /* Upstream's own writer claims 104 more data bytes than it wrote. The
     * reader must not hand back bytes the file does not contain. */
    const auto path = temp_path("overstated.wav");
    std::vector<uint8_t> b;
    auto push32 = [&b](uint32_t v) {
        b.push_back(static_cast<uint8_t>(v));
        b.push_back(static_cast<uint8_t>(v >> 8));
        b.push_back(static_cast<uint8_t>(v >> 16));
        b.push_back(static_cast<uint8_t>(v >> 24));
    };
    auto push_tag = [&b](const char* t) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<uint8_t>(t[i]));
    };

    push_tag("RIFF");
    push32(36 + 5000);
    push_tag("WAVE");
    push_tag("fmt ");
    push32(16);
    b.push_back(1); b.push_back(0);  /* PCM */
    b.push_back(1); b.push_back(0);  /* mono */
    push32(8000);
    push32(16000);
    b.push_back(2); b.push_back(0);
    b.push_back(16); b.push_back(0);
    push_tag("data");
    push32(5000); /* claimed */
    for (int i = 0; i < 40; ++i) b.push_back(static_cast<uint8_t>(i));

    CHECK(spit(path, b));

    core::WavFileReader r;
    CHECK(r.open(path));
    CHECK_EQ(r.data_size(), 40u);
    CHECK_EQ(r.sample_count(), 20u);
    std::vector<uint8_t> back(100, 0xff);
    CHECK_EQ(r.read(back.data(), back.size()), static_cast<size_t>(40));
}

TEST(wav_writer_rejects_impossible_formats) {
    core::WavFileWriter w1;
    CHECK(!w1.create(temp_path("bad_rate.wav"), 0, "x"));
    core::WavFileWriter w2;
    CHECK(!w2.create(temp_path("bad_ch.wav"), 8000, "x", 0, 16));
    core::WavFileWriter w3;
    CHECK(!w3.create(temp_path("bad_bits.wav"), 8000, "x", 1, 24));
    core::WavFileWriter w4;
    CHECK(!w4.create(temp_path("no_such_dir/nope.wav"), 8000, "x"));
}

TEST(wav_title_is_truncated_to_the_inam_field) {
    /* Upstream's tags_t holds 64 bytes and NUL-terminates, so 63 characters. */
    const auto path = temp_path("longtitle.wav");
    const std::string long_title(200, 'A');
    const std::vector<int16_t> samples(8, 0);

    {
        core::WavFileWriter w;
        CHECK(w.create(path, 8000, long_title));
        CHECK(w.write_samples(samples.data(), samples.size()));
        CHECK(w.close());
    }

    core::WavFileReader r;
    CHECK(r.open(path));
    CHECK_EQ(r.title().size(), static_cast<size_t>(63));
    CHECK_STR_EQ(r.title(), std::string(63, 'A'));
}

TEST(wav_empty_recording_is_still_a_valid_file) {
    const auto path = temp_path("empty.wav");
    {
        core::WavFileWriter w;
        CHECK(w.create(path, 44100, "silence"));
        CHECK(w.close());
    }

    const auto b = slurp(path);
    CHECK_EQ(b.size(), static_cast<size_t>(44 + 104));
    CHECK_EQ(le32(b, 40), 0u);

    core::WavFileReader r;
    CHECK(r.open(path));
    CHECK_EQ(r.data_size(), 0u);
    CHECK_EQ(r.sample_count(), 0u);
    CHECK_EQ(r.ms_duration(), 0u);
    CHECK_STR_EQ(r.title(), "silence");
}

TEST(wav_destructor_closes_and_patches_the_sizes) {
    const auto path = temp_path("dtor.wav");
    const auto samples = pattern_s16(100);
    {
        core::WavFileWriter w;
        CHECK(w.create(path, 8000, "dtor"));
        CHECK(w.write_samples(samples.data(), samples.size()));
        /* No explicit close(). */
    }

    const auto b = slurp(path);
    CHECK_EQ(b.size(), static_cast<size_t>(44 + 200 + 104));
    CHECK_EQ(le32(b, 40), 200u);
    CHECK_EQ(le32(b, 4), static_cast<uint32_t>(b.size() - 8));
}

/* =========================================================================
 * BMP
 * ====================================================================== */

TEST(bmp_created_header_matches_upstream_layout) {
    const auto path = temp_path("hdr.bmp");
    /* Width 3: a row is 9 bytes, padded to 12. */
    std::vector<ui::Color> px(3 * 2, ui::Color::black());

    CHECK(core::save_bmp_rgb565(path, px.data(), 3, 2));

    const auto b = slurp(path);
    CHECK_EQ(b.size(), static_cast<size_t>(54 + 12 * 2));

    CHECK_EQ(b[0], static_cast<uint8_t>('B'));
    CHECK_EQ(b[1], static_cast<uint8_t>('M'));
    CHECK_EQ(le32(b, 2), 54u + 24u); /* file size */
    CHECK_EQ(le16(b, 6), 0u);        /* reserved_1 */
    CHECK_EQ(le16(b, 8), 0u);        /* reserved_2 */
    CHECK_EQ(le32(b, 10), 0x36u);    /* image_data */
    CHECK_EQ(le32(b, 14), 0x28u);    /* BIH_size */
    CHECK_EQ(le32(b, 18), 3u);       /* width */
    CHECK_EQ(le32s(b, 22), -2);      /* height: negative == top-down */
    CHECK_EQ(le16(b, 26), 1u);       /* planes */
    CHECK_EQ(le16(b, 28), 24u);      /* bpp */
    CHECK_EQ(le32(b, 30), 0u);       /* compression */
    CHECK_EQ(le32(b, 34), 24u);      /* data_size */
    CHECK_EQ(le32(b, 38), 100u);     /* h_res */
    CHECK_EQ(le32(b, 42), 100u);     /* v_res */
    CHECK_EQ(le32(b, 46), 0u);       /* colors_count */
    CHECK_EQ(le32(b, 50), 0u);       /* icolors_count */
}

TEST(bmp_row_padding_puts_pixels_at_the_right_offsets) {
    /* Width 3 x 24 bpp is 9 bytes, so each row carries 3 pad bytes. Every
     * colour here is exactly representable in RGB565. */
    const auto path = temp_path("pad3.bmp");
    ui::Color px[6] = {
        ui::Color(248, 0, 0), ui::Color(0, 252, 0), ui::Color(0, 0, 248),
        ui::Color(8, 4, 8), ui::Color(128, 128, 128), ui::Color(255, 255, 255)};

    CHECK(core::save_bmp_rgb565(path, px, 3, 2));

    const auto b = slurp(path);
    CHECK_EQ(b.size(), static_cast<size_t>(54 + 24));

    /* Top row first (top-down), BGR order. */
    const size_t r0 = 54;
    CHECK_EQ(b[r0 + 0], 0u);   CHECK_EQ(b[r0 + 1], 0u);   CHECK_EQ(b[r0 + 2], 248u);
    CHECK_EQ(b[r0 + 3], 0u);   CHECK_EQ(b[r0 + 4], 252u); CHECK_EQ(b[r0 + 5], 0u);
    CHECK_EQ(b[r0 + 6], 248u); CHECK_EQ(b[r0 + 7], 0u);   CHECK_EQ(b[r0 + 8], 0u);
    CHECK_EQ(b[r0 + 9], 0u);   CHECK_EQ(b[r0 + 10], 0u);  CHECK_EQ(b[r0 + 11], 0u);

    const size_t r1 = 54 + 12;
    CHECK_EQ(b[r1 + 0], 8u);    CHECK_EQ(b[r1 + 1], 4u);    CHECK_EQ(b[r1 + 2], 8u);
    CHECK_EQ(b[r1 + 3], 128u);  CHECK_EQ(b[r1 + 4], 128u);  CHECK_EQ(b[r1 + 5], 128u);
    CHECK_EQ(b[r1 + 6], 248u);  CHECK_EQ(b[r1 + 7], 252u);  CHECK_EQ(b[r1 + 8], 248u);
    CHECK_EQ(b[r1 + 9], 0u);    CHECK_EQ(b[r1 + 10], 0u);   CHECK_EQ(b[r1 + 11], 0u);
}

TEST(bmp_roundtrip_preserves_pixels_for_padded_widths) {
    /* 3 pixels: 9 bytes -> 12. 5 pixels: 15 bytes -> 16. 4 pixels: 12, exact. */
    const uint32_t widths[] = {3, 5, 4, 1};
    const uint32_t heights[] = {2, 3, 3, 7};

    for (size_t k = 0; k < 4; ++k) {
        const uint32_t w = widths[k];
        const uint32_t h = heights[k];
        const auto path = temp_path(("rt_" + std::to_string(w) + "x" +
                                     std::to_string(h) + ".bmp")
                                        .c_str());

        std::vector<ui::Color> src(static_cast<size_t>(w) * h);
        for (size_t i = 0; i < src.size(); ++i) {
            src[i] = ui::Color(static_cast<uint8_t>((i * 37) & 0xff),
                               static_cast<uint8_t>((i * 91) & 0xff),
                               static_cast<uint8_t>((i * 53) & 0xff));
        }

        CHECK(core::save_bmp_rgb565(path, src.data(), w, h));

        /* File size proves the padding went in. */
        const uint32_t bpr = core::bmp_bytes_per_row(w, 3);
        CHECK_EQ(bpr, (w * 3u + 3u) & ~3u);
        CHECK_EQ(slurp(path).size(), static_cast<size_t>(54 + bpr * h));

        std::vector<ui::Color> back;
        uint32_t bw = 0, bh = 0;
        CHECK(core::load_bmp_rgb565(path, back, bw, bh));
        CHECK_EQ(bw, w);
        CHECK_EQ(bh, h);
        CHECK_EQ(back.size(), src.size());

        for (size_t i = 0; i < src.size(); ++i) CHECK_EQ(back[i].v, src[i].v);
    }
}

TEST(bmp_rgb565_conversion_stays_within_the_quantisation_error) {
    /* RGB565 keeps 5 bits of red, 6 of green, 5 of blue, so a 24-bit value
     * survives a round trip to within 7 / 3 / 7. */
    const auto path = temp_path("quant.bmp");

    struct RGB {
        uint8_t r, g, b;
    };
    const RGB truth[8] = {{0, 0, 0},       {255, 255, 255}, {1, 1, 1},
                          {7, 3, 7},       {200, 100, 50},  {123, 45, 67},
                          {254, 253, 252}, {8, 4, 8}};

    std::vector<ui::Color> src(8);
    for (size_t i = 0; i < 8; ++i) src[i] = ui::Color(truth[i].r, truth[i].g, truth[i].b);

    CHECK(core::save_bmp_rgb565(path, src.data(), 8, 1));

    std::vector<ui::Color> back;
    uint32_t w = 0, h = 0;
    CHECK(core::load_bmp_rgb565(path, back, w, h));
    CHECK_EQ(w, 8u);
    CHECK_EQ(h, 1u);

    for (size_t i = 0; i < 8; ++i) {
        ui::Color c = back[i];
        CHECK(std::abs(static_cast<int>(c.r()) - static_cast<int>(truth[i].r)) <= 7);
        CHECK(std::abs(static_cast<int>(c.g()) - static_cast<int>(truth[i].g)) <= 3);
        CHECK(std::abs(static_cast<int>(c.b()) - static_cast<int>(truth[i].b)) <= 7);
        /* Truncation, not rounding: never above the original. */
        CHECK(c.r() <= truth[i].r);
        CHECK(c.g() <= truth[i].g);
        CHECK(c.b() <= truth[i].b);
    }

    /* Values that already sit on the grid survive exactly. */
    ui::Color exact = back[0];
    CHECK_EQ(exact.r(), 0u);
    ui::Color white = back[1];
    CHECK_EQ(white.r(), 248u);
    CHECK_EQ(white.g(), 252u);
    CHECK_EQ(white.b(), 248u);
}

TEST(bmp_reads_a_bottom_up_file_top_row_first) {
    /* Everything this project writes is top-down, but files from elsewhere are
     * usually bottom-up. y == 0 must still be the top row. */
    const auto path = temp_path("bottomup.bmp");
    const std::vector<std::vector<uint8_t>> rows = {
        {0, 0, 248, 0, 252, 0},     /* top:    red,  green */
        {248, 0, 0, 128, 128, 128}, /* bottom: blue, grey  */
    };
    CHECK(spit(path, make_bmp24(2, 2, true, rows)));

    core::BmpFile bmp;
    CHECK(bmp.open(path, true));
    CHECK(bmp.is_bottomup());
    CHECK_EQ(bmp.get_width(), 2u);
    CHECK_EQ(bmp.get_real_height(), 2u);
    CHECK_EQ(bmp.bytes_per_row(), 8u); /* 2 * 3 = 6 -> 8 */

    std::vector<ui::Color> back;
    uint32_t w = 0, h = 0;
    CHECK(core::load_bmp_rgb565(path, back, w, h));
    CHECK_EQ(back[0].v, ui::Color(248, 0, 0).v);
    CHECK_EQ(back[1].v, ui::Color(0, 252, 0).v);
    CHECK_EQ(back[2].v, ui::Color(0, 0, 248).v);
    CHECK_EQ(back[3].v, ui::Color(128, 128, 128).v);
}

TEST(bmp_read_next_px_cnt_does_not_run_into_the_padding) {
    /* Width 3 leaves 3 pad bytes per row; asking for the whole row must return
     * the row, and asking for more must cross into the next row, not the pad. */
    const auto path = temp_path("cnt.bmp");
    ui::Color px[6] = {
        ui::Color(248, 0, 0), ui::Color(0, 252, 0), ui::Color(0, 0, 248),
        ui::Color(8, 4, 8), ui::Color(128, 128, 128), ui::Color(255, 255, 255)};
    CHECK(core::save_bmp_rgb565(path, px, 3, 2));

    core::BmpFile bmp;
    CHECK(bmp.open(path, true));
    CHECK_EQ(bmp.bytes_per_row(), 12u);
    CHECK(!bmp.is_bottomup());

    ui::Color row[3]{};
    CHECK(bmp.seek(0, 0));
    CHECK(bmp.read_next_px_cnt(row, 3, false));
    for (int i = 0; i < 3; ++i) CHECK_EQ(row[i].v, px[i].v);
    /* advance == false leaves the cursor where it was. */
    CHECK_EQ(bmp.cursor_x(), 0u);
    CHECK_EQ(bmp.cursor_y(), 0u);

    CHECK(bmp.seek(0, 1));
    CHECK(bmp.read_next_px_cnt(row, 3, false));
    for (int i = 0; i < 3; ++i) CHECK_EQ(row[i].v, px[3 + i].v);

    /* Straddling the row boundary skips the padding. */
    ui::Color pair[2]{};
    CHECK(bmp.seek(2, 0));
    CHECK(bmp.read_next_px_cnt(pair, 2, true));
    CHECK_EQ(pair[0].v, px[2].v);
    CHECK_EQ(pair[1].v, px[3].v);
    CHECK_EQ(bmp.cursor_x(), 1u);
    CHECK_EQ(bmp.cursor_y(), 1u);

    /* Asking for more than the image holds fails. */
    ui::Color over[8]{};
    CHECK(bmp.seek(0, 0));
    CHECK(!bmp.read_next_px_cnt(over, 7, false));
}

TEST(bmp_sequential_reads_and_writes_respect_the_image_bounds) {
    const auto path = temp_path("bounds.bmp");
    std::vector<ui::Color> px(3 * 2, ui::Color::black());
    CHECK(core::save_bmp_rgb565(path, px.data(), 3, 2));

    core::BmpFile bmp;
    CHECK(bmp.open(path, false));

    CHECK(!bmp.seek(3, 0)); /* x past the right edge */
    CHECK(!bmp.seek(0, 2)); /* y past the bottom */
    CHECK(bmp.seek(2, 1));  /* the last pixel */

    ui::Color c{};
    CHECK(bmp.read_next_px(c));  /* reads it, then latches the end */
    CHECK(bmp.at_end());
    CHECK(!bmp.read_next_px(c)); /* upstream would hand back the same pixel */
    CHECK(!bmp.write_next_px(ui::Color::white()));

    /* Writes land where they were seeked and are visible on reopen. */
    CHECK(bmp.seek(1, 0));
    CHECK(bmp.write_next_px(ui::Color(255, 255, 255)));
    CHECK_EQ(bmp.cursor_x(), 2u);
    CHECK_EQ(bmp.cursor_y(), 0u);
    bmp.close();

    std::vector<ui::Color> back;
    uint32_t w = 0, h = 0;
    CHECK(core::load_bmp_rgb565(path, back, w, h));
    CHECK_EQ(back[1].v, ui::Color(255, 255, 255).v);
    CHECK_EQ(back[0].v, ui::Color::black().v);
}

TEST(bmp_readonly_file_cannot_be_written_or_grown) {
    const auto path = temp_path("ro.bmp");
    std::vector<ui::Color> px(4, ui::Color::black());
    CHECK(core::save_bmp_rgb565(path, px.data(), 2, 2));

    core::BmpFile bmp;
    CHECK(bmp.open(path, true));
    CHECK(bmp.is_read_only());
    CHECK(bmp.seek(0, 0));
    CHECK(!bmp.write_next_px(ui::Color::white()));
    CHECK(!bmp.expand_y(8));
}

TEST(bmp_expand_y_grows_the_image_and_keeps_what_was_there) {
    /* This is how the SSTV, WEFAX and APT receivers build an image: one line at
     * a time into a file that starts a single row tall. */
    const auto path = temp_path("expand.bmp");

    core::BmpFile bmp;
    CHECK(bmp.create(path, 3, 1));
    CHECK_EQ(bmp.get_real_height(), 1u);
    CHECK_EQ(bmp.bytes_per_row(), 12u);
    CHECK(!bmp.is_bottomup());

    CHECK(bmp.seek(0, 0));
    CHECK(bmp.write_next_px(ui::Color(248, 0, 0)));
    CHECK(bmp.write_next_px(ui::Color(0, 252, 0)));
    CHECK(bmp.write_next_px(ui::Color(0, 0, 248)));

    CHECK(bmp.expand_y(3));
    CHECK_EQ(bmp.get_real_height(), 3u);
    /* The cursor lands at the start of the first new row. */
    CHECK_EQ(bmp.cursor_x(), 0u);
    CHECK_EQ(bmp.cursor_y(), 1u);

    CHECK(bmp.write_next_px(ui::Color(255, 255, 255)));

    /* expand_y_delta is expand_y(height + delta). */
    CHECK(bmp.expand_y_delta(1));
    CHECK_EQ(bmp.get_real_height(), 4u);
    CHECK_EQ(bmp.cursor_y(), 3u);

    /* Shrinking is a no-op, as upstream has it. */
    CHECK(bmp.expand_y(2));
    CHECK_EQ(bmp.get_real_height(), 4u);

    bmp.close();

    const auto b = slurp(path);
    CHECK_EQ(b.size(), static_cast<size_t>(54 + 12 * 4));
    CHECK_EQ(le32(b, 2), 54u + 48u);  /* header size field kept in step */
    CHECK_EQ(le32(b, 34), 48u);       /* data_size */
    CHECK_EQ(le32s(b, 22), -4);       /* still top-down */

    std::vector<ui::Color> back;
    uint32_t w = 0, h = 0;
    CHECK(core::load_bmp_rgb565(path, back, w, h));
    CHECK_EQ(w, 3u);
    CHECK_EQ(h, 4u);
    /* Row 0 survived the two expansions untouched. */
    CHECK_EQ(back[0].v, ui::Color(248, 0, 0).v);
    CHECK_EQ(back[1].v, ui::Color(0, 252, 0).v);
    CHECK_EQ(back[2].v, ui::Color(0, 0, 248).v);
    /* The pixel written into the first new row. */
    CHECK_EQ(back[3].v, ui::Color(255, 255, 255).v);
    /* Growth is zero-filled, i.e. black. */
    CHECK_EQ(back[4].v, ui::Color::black().v);
    CHECK_EQ(back[11].v, ui::Color::black().v);
}

TEST(bmp_expand_y_fills_new_rows_with_the_background_colour) {
    const auto path = temp_path("expandbg.bmp");

    core::BmpFile bmp;
    bmp.set_bg_color(ui::Color(0, 0, 248));
    CHECK(bmp.create(path, 3, 2));
    bmp.close();

    std::vector<ui::Color> back;
    uint32_t w = 0, h = 0;
    CHECK(core::load_bmp_rgb565(path, back, w, h));
    CHECK_EQ(back.size(), static_cast<size_t>(6));
    for (auto& c : back) CHECK_EQ(c.v, ui::Color(0, 0, 248).v);
}

TEST(bmp_rejects_files_it_cannot_decode) {
    core::BmpFile missing;
    CHECK(!missing.open(temp_path("no_such.bmp"), true));

    /* Wrong signature. */
    const auto bad_sig = temp_path("badsig.bmp");
    auto b = make_bmp24(2, 2, false, {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}});
    b[0] = 'X';
    CHECK(spit(bad_sig, b));
    core::BmpFile f1;
    CHECK(!f1.open(bad_sig, true));

    /* 8-bit palette images: upstream declares these not implemented. */
    const auto pal = temp_path("pal8.bmp");
    auto p = make_bmp24(2, 2, false, {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}});
    p[28] = 8;
    CHECK(spit(pal, p));
    core::BmpFile f2;
    CHECK(!f2.open(pal, true));

    /* An unsupported depth. */
    const auto odd = temp_path("bpp1.bmp");
    auto o = make_bmp24(2, 2, false, {{0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}});
    o[28] = 1;
    CHECK(spit(odd, o));
    core::BmpFile f3;
    CHECK(!f3.open(odd, true));

    /* A file too short to hold a header. */
    const auto stub = temp_path("stub.bmp");
    CHECK(spit(stub, std::vector<uint8_t>(20, 0)));
    core::BmpFile f4;
    CHECK(!f4.open(stub, true));

    /* Degenerate sizes. */
    core::BmpFile f5;
    CHECK(!f5.create(temp_path("zero.bmp"), 0, 4));
    core::BmpFile f6;
    CHECK(!f6.create(temp_path("zero2.bmp"), 4, 0));

    std::vector<ui::Color> out;
    uint32_t w = 0, h = 0;
    CHECK(!core::load_bmp_rgb565(temp_path("no_such.bmp"), out, w, h));
    CHECK(!core::save_bmp_rgb565(temp_path("zero3.bmp"), nullptr, 2, 2));
}

TEST(bmp_reads_a_32_bit_file) {
    /* The screenshot viewer has to cope with 32-bit BMPs from other tools. */
    const auto path = temp_path("bgra32.bmp");
    std::vector<uint8_t> b(54 + 8, 0);
    b[0] = 'B';
    b[1] = 'M';
    b[2] = static_cast<uint8_t>(b.size());
    b[10] = 54;
    b[14] = 40;
    b[18] = 2;                     /* width 2 */
    b[22] = 0xff; b[23] = 0xff;    /* height -1 (top-down) */
    b[24] = 0xff; b[25] = 0xff;
    b[26] = 1;
    b[28] = 32;
    /* BGRA: blue pixel then a mid grey. */
    b[54] = 248; b[55] = 0;   b[56] = 0;   b[57] = 255;
    b[58] = 128; b[59] = 128; b[60] = 128; b[61] = 255;
    CHECK(spit(path, b));

    core::BmpFile bmp;
    CHECK(bmp.open(path, true));
    CHECK_EQ(bmp.bits_per_pixel(), 32u);
    CHECK_EQ(bmp.bytes_per_pixel(), 4u);
    CHECK_EQ(bmp.bytes_per_row(), 8u);

    ui::Color c{};
    CHECK(bmp.seek(0, 0));
    CHECK(bmp.read_next_px(c));
    CHECK_EQ(c.v, ui::Color(0, 0, 248).v);
    CHECK(bmp.read_next_px(c));
    CHECK_EQ(c.v, ui::Color(128, 128, 128).v);
}

TEST(bmp_16_bit_argb1555_roundtrips_through_the_encoder) {
    /* Upstream decodes ARGB1555 by expanding each 5-bit field with
     * (x << 3) | (x >> 2); its encoder forgets to shift the 8-bit channel down
     * first and truncates red into the alpha bit. This checks the pair agree. */
    const auto path = temp_path("argb1555.bmp");
    std::vector<uint8_t> b(54 + 8, 0);
    b[0] = 'B';
    b[1] = 'M';
    b[2] = static_cast<uint8_t>(b.size());
    b[10] = 54;
    b[14] = 40;
    b[18] = 2;
    b[22] = 0xff; b[23] = 0xff;
    b[24] = 0xff; b[25] = 0xff;
    b[26] = 1;
    b[28] = 16;
    CHECK(spit(path, b));

    core::BmpFile bmp;
    CHECK(bmp.open(path, false));
    CHECK_EQ(bmp.bits_per_pixel(), 16u);
    CHECK_EQ(bmp.bytes_per_pixel(), 2u);

    CHECK(bmp.seek(0, 0));
    CHECK(bmp.write_next_px(ui::Color(248, 0, 0)));
    CHECK(bmp.write_next_px(ui::Color(0, 0, 248)));

    /* Pure red is r5 = 0x1f, so the stored word is 0xFC00 with the alpha bit
     * set: low byte 0x00, high byte 0xFC. */
    bmp.close();
    const auto out = slurp(path);
    CHECK_EQ(out[54], 0x00u);
    CHECK_EQ(out[55], 0xfcu);
    /* Pure blue is b5 = 0x1f: low byte 0x1F, high byte 0x80. */
    CHECK_EQ(out[56], 0x1fu);
    CHECK_EQ(out[57], 0x80u);

    core::BmpFile again;
    CHECK(again.open(path, true));
    ui::Color c{};
    CHECK(again.seek(0, 0));
    CHECK(again.read_next_px(c));
    /* 5 -> 8 bit expansion of 0x1f is 0xff, quantised back to 0xf8 red. */
    CHECK_EQ(c.v, ui::Color(255, 0, 0).v);
    CHECK(again.read_next_px(c));
    CHECK_EQ(c.v, ui::Color(0, 0, 255).v);
}

TEST(bmp_bytes_per_row_matches_the_format_rule) {
    /* (width * bytes_per_pixel + 3) & ~3, straight from bmpfile.cpp. */
    struct Case {
        uint32_t width;
        uint32_t bytes_per_pixel;
        uint32_t expected;
    };
    const Case cases[] = {
        {1, 3, 4}, {2, 3, 8},  {3, 3, 12},   {4, 3, 12},
        {5, 3, 16}, {240, 3, 720}, {3, 4, 12}, {3, 2, 8},
    };
    for (const auto& c : cases)
        CHECK_EQ(core::bmp_bytes_per_row(c.width, c.bytes_per_pixel), c.expected);
}
