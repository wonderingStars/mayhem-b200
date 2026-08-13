/*
 * mayhem-b200 — IQ capture file and replay tests.
 *
 * The expected byte layouts and metadata text here come from the PortaPack
 * firmware, not from this implementation: interleaved int16/int8 with no
 * header (application/file.cpp capture_file_sample_size, io_convert.cpp), a
 * CRLF "key=value" sidecar written by application/metadata_file.cpp, and the
 * integer-truncating duration arithmetic of common/utility.hpp ms_duration().
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include <process.h> /* _getpid: fixture dirs must be unique PER PROCESS */

#include "iq_file.hpp"
#include "replay_model.hpp"
#include "ring_buffer.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using core::cfloat;

/* A directory that removes itself, so a failing test does not leave megabytes
 * of IQ behind in the system temp folder. */
class TempDir {
   public:
    explicit TempDir(const char* tag) {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mb200_iq_" + std::string{tag} + "_" +
                 std::to_string(_getpid()) + "_" +
                 std::to_string(counter.fetch_add(1)));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string file(const std::string& name) const {
        return (path_ / name).string();
    }

   private:
    std::filesystem::path path_;
};

std::string read_text(const std::string& path) {
    std::ifstream f{path, std::ios::binary};
    return std::string{std::istreambuf_iterator<char>{f}, std::istreambuf_iterator<char>{}};
}

std::vector<char> read_bytes(const std::string& path) {
    std::ifstream f{path, std::ios::binary};
    return std::vector<char>{std::istreambuf_iterator<char>{f},
                             std::istreambuf_iterator<char>{}};
}

void write_text(const std::string& path, const std::string& text) {
    std::ofstream f{path, std::ios::binary | std::ios::trunc};
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

uint64_t file_bytes(const std::string& path) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(n);
}

/* A short sequence that exercises both rails, both signs and the middle of the
 * range. Full scale is included so clipping behaviour is visible. */
std::vector<cfloat> reference_sequence() {
    return {
        {1.0f, 0.0f},
        {0.0f, -1.0f},
        {-1.0f, 1.0f},
        {0.25f, -0.75f},
        {0.0f, 0.0f},
        {-0.5f, 0.5f},
    };
}

/* Deterministic filler for the longer files. Small amplitudes so C8's coarse
 * quantisation is not what the test is measuring. */
cfloat ramp_sample(uint64_t i) {
    const float f = static_cast<float>(i % 1000) / 2000.0f;
    return cfloat{f, -f};
}

/* Deadlines passed here are HANG GUARDS, not part of any property: they only
 * bound how long a genuinely broken replay can stall the suite, and the wait
 * returns the instant the predicate turns true. Size them for a saturated
 * machine, not a quiet one -- the replay paces itself with short sleeps whose
 * wakeups quantise to Windows' ~15.6 ms timer and stretch several-fold under
 * load; parallel builds on every core flaked a 5 s guard on 2026-08-13. */
bool wait_until(const std::function<bool()>& predicate, int timeout_ms) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

/* Collects everything ReplayModel hands out, and never refuses, so
 * samples_delivered() equals what the model produced. */
struct CollectingSink {
    std::mutex mutex;
    std::vector<cfloat> samples;

    radio::ReplayModel::SampleSink fn() {
        return [this](const cfloat* data, size_t count) {
            std::lock_guard<std::mutex> g{mutex};
            samples.insert(samples.end(), data, data + count);
            return count;
        };
    }

    size_t size() {
        std::lock_guard<std::mutex> g{mutex};
        return samples.size();
    }
};

/* Writes `count` ramp samples plus a matching sidecar; returns the data path. */
std::string make_capture(const TempDir& dir,
                         const std::string& name,
                         uint64_t count,
                         uint32_t sample_rate,
                         uint64_t center_frequency = 433'920'000,
                         bool write_sidecar = true) {
    const std::string path = dir.file(name);

    core::IqFileWriter writer;
    if (!writer.open(path)) return {};

    std::vector<cfloat> block;
    block.reserve(1024);
    for (uint64_t i = 0; i < count; i++) {
        block.push_back(ramp_sample(i));
        if (block.size() == 1024) {
            writer.write(block.data(), block.size());
            block.clear();
        }
    }
    if (!block.empty()) writer.write(block.data(), block.size());
    writer.close();

    if (write_sidecar) {
        core::CaptureMetadata md{};
        md.center_frequency = center_frequency;
        md.sample_rate = sample_rate;
        core::write_metadata_file(core::metadata_path_for(path), md);
    }

    return path;
}

}  // namespace

/* --- Format detection ------------------------------------------------------ */

TEST(iq_format_from_extension) {
    CHECK(core::iq_format_from_path("CAP_001.C16") == core::IqFormat::C16);
    CHECK(core::iq_format_from_path("cap_001.c16") == core::IqFormat::C16);
    CHECK(core::iq_format_from_path("CAP_001.C8") == core::IqFormat::C8);
    CHECK(core::iq_format_from_path("cap_001.c8") == core::IqFormat::C8);
    CHECK(core::iq_format_from_path("C:/CAPTURES/x.C16") == core::IqFormat::C16);

    /* .CU8 is a PortaPack read-only format that this module does not claim. */
    CHECK(core::iq_format_from_path("x.CU8") == core::IqFormat::Unknown);
    CHECK(core::iq_format_from_path("x.WAV") == core::IqFormat::Unknown);
    CHECK(core::iq_format_from_path("noextension") == core::IqFormat::Unknown);

    /* A dot in a directory name is not the file's extension. */
    CHECK(core::iq_format_from_path("C:/my.captures/recording") == core::IqFormat::Unknown);
}

TEST(iq_format_frame_sizes_match_upstream) {
    /* capture_file_sample_size(): sizeof(complex16_t) == 4, complex8_t == 2. */
    CHECK_EQ(core::iq_format_frame_bytes(core::IqFormat::C16), size_t{4});
    CHECK_EQ(core::iq_format_frame_bytes(core::IqFormat::C8), size_t{2});
    CHECK_EQ(core::iq_format_frame_bytes(core::IqFormat::Unknown), size_t{0});

    CHECK_STR_EQ(core::iq_format_name(core::IqFormat::C16), "C16");
    CHECK_STR_EQ(core::iq_format_name(core::IqFormat::C8), "C8");
    CHECK_STR_EQ(core::iq_format_extension(core::IqFormat::C16), ".C16");
    CHECK_STR_EQ(core::iq_format_extension(core::IqFormat::C8), ".C8");
}

/* --- C16 ------------------------------------------------------------------- */

TEST(c16_round_trip_preserves_samples) {
    TempDir dir{"c16rt"};
    const std::string path = dir.file("RT.C16");
    const auto input = reference_sequence();

    core::IqFileWriter writer;
    CHECK(writer.open(path));
    CHECK(writer.format() == core::IqFormat::C16);
    CHECK(writer.write(input.data(), input.size()));
    writer.close();
    CHECK_EQ(writer.samples_written(), uint64_t{6});

    /* Interleaved int16, no header: 4 bytes per sample and nothing else. */
    CHECK_EQ(file_bytes(path), uint64_t{4 * 6});

    core::IqFileReader reader;
    CHECK(reader.open(path));
    CHECK(reader.format() == core::IqFormat::C16);
    CHECK_EQ(reader.total_samples(), uint64_t{6});

    std::vector<cfloat> output;
    CHECK_EQ(reader.read(output, 100), size_t{6});
    CHECK_EQ(output.size(), size_t{6});

    /* Quantisation is one step of 1/32767, so the round trip is good to half
     * of that. */
    for (size_t i = 0; i < input.size(); i++) {
        CHECK_NEAR(output[i].real(), input[i].real(), 2.0e-5);
        CHECK_NEAR(output[i].imag(), input[i].imag(), 2.0e-5);
    }
}

TEST(c16_on_disk_layout_is_interleaved_int16) {
    TempDir dir{"c16bytes"};
    const std::string path = dir.file("L.C16");

    const std::vector<cfloat> input{{1.0f, 0.0f}, {-1.0f, 0.25f}};
    core::IqFileWriter writer;
    CHECK(writer.open(path));
    CHECK(writer.write(input.data(), input.size()));
    writer.close();

    const auto raw = read_bytes(path);
    CHECK_EQ(raw.size(), size_t{8});

    const int16_t* v = reinterpret_cast<const int16_t*>(raw.data());
    /* Full scale for C16 is 32767, matching ReceiverModel's capture tap. */
    CHECK_EQ(v[0], int16_t{32767});
    CHECK_EQ(v[1], int16_t{0});
    CHECK_EQ(v[2], int16_t{-32767});
    /* 0.25 * 32767 = 8191.75, nearest integer 8192. */
    CHECK_EQ(v[3], int16_t{8192});
}

TEST(c16_clamps_out_of_range_input) {
    TempDir dir{"c16clamp"};
    const std::string path = dir.file("C.C16");

    /* An overdriven front end must saturate, not wrap to the opposite rail. */
    const std::vector<cfloat> input{{2.5f, -3.0f}};
    core::IqFileWriter writer;
    CHECK(writer.open(path));
    CHECK(writer.write(input.data(), input.size()));
    writer.close();

    const auto raw = read_bytes(path);
    const int16_t* v = reinterpret_cast<const int16_t*>(raw.data());
    CHECK_EQ(v[0], int16_t{32767});
    CHECK_EQ(v[1], int16_t{-32767});
}

/* --- C8 -------------------------------------------------------------------- */

TEST(c8_round_trip_preserves_samples_within_quantisation) {
    TempDir dir{"c8rt"};
    const std::string path = dir.file("RT.C8");
    const auto input = reference_sequence();

    core::IqFileWriter writer;
    CHECK(writer.open(path));
    CHECK(writer.format() == core::IqFormat::C8);
    CHECK(writer.write(input.data(), input.size()));
    writer.close();

    /* Two bytes per sample: half the size of the same capture in C16. */
    CHECK_EQ(file_bytes(path), uint64_t{2 * 6});

    core::IqFileReader reader;
    CHECK(reader.open(path));
    CHECK(reader.format() == core::IqFormat::C8);
    CHECK_EQ(reader.total_samples(), uint64_t{6});

    std::vector<cfloat> output;
    CHECK_EQ(reader.read(output, 6), size_t{6});

    /* One C8 step is 1/127, so half a step is 3.94e-3. */
    for (size_t i = 0; i < input.size(); i++) {
        CHECK_NEAR(output[i].real(), input[i].real(), 4.1e-3);
        CHECK_NEAR(output[i].imag(), input[i].imag(), 4.1e-3);
    }
}

TEST(c8_on_disk_layout_is_interleaved_int8) {
    TempDir dir{"c8bytes"};
    const std::string path = dir.file("L.C8");

    const std::vector<cfloat> input{{1.0f, -1.0f}, {0.0f, 0.5f}};
    core::IqFileWriter writer;
    CHECK(writer.open(path));
    CHECK(writer.write(input.data(), input.size()));
    writer.close();

    const auto raw = read_bytes(path);
    CHECK_EQ(raw.size(), size_t{4});

    const int8_t* v = reinterpret_cast<const int8_t*>(raw.data());
    /* Full scale 127: upstream converts C16 to C8 by dividing by 256, and
     * 32767 / 256 rounds to the same rail. */
    CHECK_EQ(v[0], int8_t{127});
    CHECK_EQ(v[1], int8_t{-127});
    CHECK_EQ(v[2], int8_t{0});
    /* 0.5 * 127 = 63.5; nearest-even gives 64. */
    CHECK(v[3] == int8_t{63} || v[3] == int8_t{64});
}

TEST(explicit_format_overrides_the_extension) {
    TempDir dir{"override"};
    const std::string path = dir.file("RAW.BIN");
    const std::vector<cfloat> input{{0.5f, -0.5f}, {0.25f, 0.75f}};

    /* Without an override an unknown extension is refused rather than guessed. */
    core::IqFileWriter refused;
    CHECK(!refused.open(path));
    CHECK(!refused.error().empty());

    core::IqFileWriter writer;
    CHECK(writer.open(path, core::IqFormat::C16));
    CHECK(writer.write(input.data(), input.size()));
    writer.close();
    CHECK_EQ(file_bytes(path), uint64_t{8});

    core::IqFileReader reader;
    CHECK(!reader.open(path));
    CHECK(reader.open(path, core::IqFormat::C16));
    std::vector<cfloat> output;
    CHECK_EQ(reader.read(output, 2), size_t{2});
    CHECK_NEAR(output[1].imag(), 0.75f, 2.0e-5);
}

/* --- Metadata sidecar ------------------------------------------------------ */

TEST(metadata_path_replaces_the_extension) {
    CHECK_STR_EQ(core::metadata_path_for("CAP_001.C16"), "CAP_001.TXT");
    CHECK_STR_EQ(core::metadata_path_for("CAP_001.C8"), "CAP_001.TXT");
    CHECK_STR_EQ(core::metadata_path_for("C:/CAPTURES/a.C16"), "C:/CAPTURES/a.TXT");
    /* replace_extension() on a path with no extension appends one. */
    CHECK_STR_EQ(core::metadata_path_for("CAP_001"), "CAP_001.TXT");
}

TEST(metadata_without_position_writes_only_two_lines) {
    TempDir dir{"metamin"};
    const std::string path = dir.file("M.TXT");

    core::CaptureMetadata md{};
    md.center_frequency = 433'920'000;
    md.sample_rate = 500'000;
    CHECK(core::write_metadata_file(path, md));

    /* Exactly what application/metadata_file.cpp emits, CRLF included. */
    CHECK_STR_EQ(read_text(path),
                 "center_frequency=433920000\r\nsample_rate=500000\r\n");

    core::CaptureMetadata back{};
    CHECK(core::read_metadata_file(path, back));
    CHECK_EQ(back.center_frequency, uint64_t{433'920'000});
    CHECK_EQ(back.sample_rate, uint32_t{500'000});

    /* Absent optional fields come back as their defaults, not as garbage. */
    CHECK_NEAR(back.latitude, 0.0f, 0.0);
    CHECK_NEAR(back.longitude, 0.0f, 0.0);
    CHECK_EQ(static_cast<int>(back.satinuse), 0);
    CHECK(!back.has_position());
}

TEST(metadata_with_position_round_trips) {
    TempDir dir{"metagps"};
    const std::string path = dir.file("G.TXT");

    core::CaptureMetadata md{};
    md.center_frequency = 1'090'000'000;
    md.sample_rate = 2'000'000;
    md.latitude = 51.5f;      /* exactly representable, so the 7-digit */
    md.longitude = -0.125f;   /* fixed-point text is unambiguous       */
    md.satinuse = 9;
    CHECK(md.has_position());
    CHECK(core::write_metadata_file(path, md));

    CHECK_STR_EQ(read_text(path),
                 "center_frequency=1090000000\r\n"
                 "sample_rate=2000000\r\n"
                 "latitude=51.5000000\r\n"
                 "longitude=-0.1250000\r\n"
                 "satinuse=9\r\n");

    core::CaptureMetadata back{};
    CHECK(core::read_metadata_file(path, back));
    CHECK_EQ(back.center_frequency, uint64_t{1'090'000'000});
    CHECK_EQ(back.sample_rate, uint32_t{2'000'000});
    CHECK_NEAR(back.latitude, 51.5f, 1.0e-6);
    CHECK_NEAR(back.longitude, -0.125f, 1.0e-6);
    CHECK_EQ(static_cast<int>(back.satinuse), 9);
    CHECK(back.has_position());
}

TEST(metadata_position_gate_needs_both_coordinates) {
    /* metadata_file.cpp writes the GPS block only when latitude and longitude
     * are both non-zero and both below 200. */
    TempDir dir{"metagate"};

    core::CaptureMetadata half{};
    half.center_frequency = 100'000'000;
    half.sample_rate = 250'000;
    half.latitude = 51.5f;
    half.longitude = 0.0f;
    half.satinuse = 4;
    CHECK(!half.has_position());

    const std::string path = dir.file("H.TXT");
    CHECK(core::write_metadata_file(path, half));
    CHECK(read_text(path).find("latitude") == std::string::npos);

    core::CaptureMetadata out_of_range{};
    out_of_range.latitude = 999.0f;
    out_of_range.longitude = 1.0f;
    CHECK(!out_of_range.has_position());
}

TEST(metadata_reader_rejects_incomplete_and_missing_files) {
    TempDir dir{"metabad"};
    core::CaptureMetadata out{};

    /* Missing file. */
    CHECK(!core::read_metadata_file(dir.file("does_not_exist.TXT"), out));

    /* read_metadata_file() treats a missing centre frequency or sample rate as
     * a failed parse, not as a zero-valued capture. */
    const std::string no_rate = dir.file("NR.TXT");
    write_text(no_rate, "center_frequency=433920000\r\n");
    CHECK(!core::read_metadata_file(no_rate, out));

    const std::string no_freq = dir.file("NF.TXT");
    write_text(no_freq, "sample_rate=500000\r\n");
    CHECK(!core::read_metadata_file(no_freq, out));

    const std::string empty = dir.file("E.TXT");
    write_text(empty, "");
    CHECK(!core::read_metadata_file(empty, out));
}

TEST(metadata_reader_skips_malformed_lines) {
    TempDir dir{"metaskip"};
    const std::string path = dir.file("S.TXT");

    /* Unix line endings, a comment, a key with no '=', a line with two of them
     * (upstream's split_string yields three columns and the line is dropped),
     * and an unknown key. The good lines still parse. */
    write_text(path,
               "# a comment\n"
               "center_frequency=88500000\n"
               "garbage\n"
               "a=b=c\n"
               "unknown_key=42\n"
               "sample_rate=1500000\n"
               "satinuse=7\n");

    core::CaptureMetadata md{};
    CHECK(core::read_metadata_file(path, md));
    CHECK_EQ(md.center_frequency, uint64_t{88'500'000});
    CHECK_EQ(md.sample_rate, uint32_t{1'500'000});
    CHECK_EQ(static_cast<int>(md.satinuse), 7);
}

/* --- Sample count and duration --------------------------------------------- */

TEST(inspect_reports_sample_count_and_duration) {
    TempDir dir{"inspect"};

    /* 1000 samples at 48 kHz. ms_duration() is integer arithmetic, so
     * 1000 * 1000 / 48000 truncates to 20 ms, not 20.83. */
    const std::string path = make_capture(dir, "D.C16", 1000, 48'000);
    CHECK(!path.empty());

    const auto info = core::inspect_iq_file(path);
    CHECK(info.valid);
    CHECK(info.error.empty());
    CHECK(info.format == core::IqFormat::C16);
    CHECK_EQ(info.file_bytes, uint64_t{4000});
    CHECK_EQ(info.samples, uint64_t{1000});
    CHECK(!info.truncated);
    CHECK(info.has_metadata);
    CHECK_EQ(info.metadata.center_frequency, uint64_t{433'920'000});
    CHECK_NEAR(info.sample_rate, 48'000.0, 1e-9);
    CHECK_NEAR(info.duration_seconds, 1000.0 / 48000.0, 1e-12);
    CHECK_EQ(info.duration_ms, uint32_t{20});
}

TEST(inspect_c8_halves_the_byte_count) {
    TempDir dir{"inspect8"};
    const std::string path = make_capture(dir, "D.C8", 1000, 500'000);

    const auto info = core::inspect_iq_file(path);
    CHECK(info.valid);
    CHECK(info.format == core::IqFormat::C8);
    CHECK_EQ(info.file_bytes, uint64_t{2000});
    CHECK_EQ(info.samples, uint64_t{1000});
    CHECK_EQ(info.duration_ms, uint32_t{2});
}

TEST(duration_ms_matches_upstream_arithmetic) {
    /* ms_duration(bytes, rate, bytes_per_sample) == samples * 1000 / rate. */
    CHECK_EQ(core::iq_duration_ms(48'000, 48'000), uint32_t{1000});
    CHECK_EQ(core::iq_duration_ms(1000, 48'000), uint32_t{20});
    CHECK_EQ(core::iq_duration_ms(0, 48'000), uint32_t{0});
    /* Upstream returns 0 rather than dividing by zero. */
    CHECK_EQ(core::iq_duration_ms(48'000, 0), uint32_t{0});
}

TEST(inspect_without_a_sidecar_reports_no_duration) {
    TempDir dir{"nosidecar"};
    const std::string path = make_capture(dir, "N.C16", 500, 0, 0, false);

    const auto info = core::inspect_iq_file(path);
    CHECK(info.valid);
    CHECK(!info.has_metadata);
    CHECK_EQ(info.samples, uint64_t{500});
    /* No recorded rate means no honest duration to report. */
    CHECK_NEAR(info.duration_seconds, 0.0, 1e-12);
    CHECK_EQ(info.duration_ms, uint32_t{0});
}

TEST(inspect_rejects_unknown_and_missing_files) {
    TempDir dir{"inspectbad"};

    const std::string wav = dir.file("X.WAV");
    write_text(wav, "not iq");
    const auto unknown = core::inspect_iq_file(wav);
    CHECK(!unknown.valid);
    CHECK(!unknown.error.empty());
    CHECK(unknown.format == core::IqFormat::Unknown);

    const auto missing = core::inspect_iq_file(dir.file("gone.C16"));
    CHECK(!missing.valid);
    CHECK(!missing.error.empty());
    CHECK_EQ(missing.samples, uint64_t{0});
}

/* --- Reader position and seeking ------------------------------------------- */

TEST(reader_tracks_position_and_seeks_to_the_right_sample) {
    TempDir dir{"seek"};
    const std::string path = make_capture(dir, "S.C16", 2000, 500'000);

    core::IqFileReader reader;
    CHECK(reader.open(path));
    CHECK_EQ(reader.total_samples(), uint64_t{2000});
    CHECK_EQ(reader.position(), uint64_t{0});
    CHECK_EQ(reader.remaining(), uint64_t{2000});
    CHECK(!reader.at_end());

    std::vector<cfloat> block;
    CHECK_EQ(reader.read(block, 300), size_t{300});
    CHECK_EQ(reader.position(), uint64_t{300});
    CHECK_EQ(reader.remaining(), uint64_t{1700});

    /* Seek lands on the sample with that index, counting from zero. */
    CHECK(reader.seek(1234));
    CHECK_EQ(reader.position(), uint64_t{1234});
    CHECK_EQ(reader.read(block, 1), size_t{1});
    CHECK_NEAR(block[0].real(), ramp_sample(1234).real(), 2.0e-5);
    CHECK_NEAR(block[0].imag(), ramp_sample(1234).imag(), 2.0e-5);
    CHECK_EQ(reader.position(), uint64_t{1235});

    /* Rewinding gives sample zero again. */
    CHECK(reader.rewind());
    CHECK_EQ(reader.read(block, 1), size_t{1});
    CHECK_NEAR(block[0].real(), ramp_sample(0).real(), 2.0e-5);

    /* A short read at the end, then nothing. */
    CHECK(reader.seek(1990));
    CHECK_EQ(reader.read(block, 100), size_t{10});
    CHECK(reader.at_end());
    CHECK_EQ(reader.read(block, 100), size_t{0});
    CHECK(!reader.failed());

    /* Seeking exactly to the end is legal; past it is not. */
    CHECK(reader.seek(2000));
    CHECK(reader.at_end());
    CHECK(!reader.seek(2001));
    CHECK(!reader.error().empty());
}

TEST(reader_duration_uses_the_sidecar_rate) {
    TempDir dir{"dur"};
    const std::string path = make_capture(dir, "R.C16", 96'000, 48'000);

    core::IqFileReader reader;
    CHECK(reader.open(path));
    CHECK(reader.has_metadata());
    CHECK_NEAR(reader.sample_rate(), 48'000.0, 1e-9);
    CHECK_NEAR(reader.duration_seconds(), 2.0, 1e-9);
    /* An explicit rate overrides the sidecar, for a file replayed at a
     * different speed. */
    CHECK_NEAR(reader.duration_seconds(96'000.0), 1.0, 1e-9);
    CHECK_NEAR(reader.duration_seconds(0.0), 0.0, 1e-12);
}

/* --- Failure cases --------------------------------------------------------- */

TEST(reader_reports_a_missing_file_instead_of_crashing) {
    TempDir dir{"missing"};

    core::IqFileReader reader;
    CHECK(!reader.open(dir.file("nothing_here.C16")));
    CHECK(!reader.is_open());
    CHECK(!reader.error().empty());
    CHECK_EQ(reader.total_samples(), uint64_t{0});

    /* Every operation on a closed reader is a safe no-op. */
    std::vector<cfloat> block;
    CHECK_EQ(reader.read(block, 64), size_t{0});
    CHECK_EQ(block.size(), size_t{0});
    CHECK(!reader.seek(0));
    CHECK_NEAR(reader.duration_seconds(), 0.0, 1e-12);
}

TEST(reader_reports_a_truncated_file) {
    TempDir dir{"trunc"};
    const std::string path = dir.file("T.C16");

    /* Ten whole C16 samples plus two stray bytes — a capture cut short. */
    write_text(path, std::string(4 * 10 + 2, '\0'));

    core::IqFileReader strict;
    CHECK(!strict.open(path));
    CHECK(!strict.is_open());
    CHECK(strict.error().find("truncated") != std::string::npos);

    /* inspect() still says what is usable, so a browser can explain itself. */
    const auto info = core::inspect_iq_file(path);
    CHECK(!info.valid);
    CHECK(info.truncated);
    CHECK_EQ(info.samples, uint64_t{10});
    CHECK_EQ(info.file_bytes, uint64_t{42});

    /* Opting in reads the whole samples and drops the partial one. */
    core::IqFileReader lenient;
    CHECK(lenient.open(path, core::IqFormat::Unknown, true));
    CHECK(lenient.truncated());
    CHECK_EQ(lenient.total_samples(), uint64_t{10});
    std::vector<cfloat> block;
    CHECK_EQ(lenient.read(block, 100), size_t{10});
    CHECK(!lenient.failed());
}

TEST(reader_handles_an_empty_file) {
    TempDir dir{"empty"};
    const std::string path = dir.file("Z.C16");
    write_text(path, "");

    /* Zero bytes is a whole number of samples, so this opens — but there is
     * nothing in it and the reader must say so rather than blocking. */
    core::IqFileReader reader;
    CHECK(reader.open(path));
    CHECK_EQ(reader.total_samples(), uint64_t{0});
    CHECK(reader.at_end());

    std::vector<cfloat> block;
    CHECK_EQ(reader.read(block, 64), size_t{0});
    CHECK(!reader.failed());
}

TEST(writer_reports_an_unwritable_path) {
    core::IqFileWriter writer;
    CHECK(!writer.open("Z:/definitely/not/a/real/path/capture.C16"));
    CHECK(!writer.is_open());
    CHECK(!writer.error().empty());

    /* Writing to a writer that never opened fails rather than crashing. */
    const std::vector<cfloat> one{{0.5f, 0.5f}};
    CHECK(!writer.write(one.data(), one.size()));
}

/* --- ReplayModel ----------------------------------------------------------- */

TEST(replay_open_reports_the_file_and_its_metadata) {
    TempDir dir{"replayopen"};
    const std::string path = make_capture(dir, "P.C16", 20'000, 2'000'000, 868'000'000);

    radio::ReplayModel replay;
    CHECK(replay.open(path));
    CHECK(replay.is_open());
    CHECK(replay.has_metadata());
    CHECK(replay.format() == core::IqFormat::C16);
    CHECK_EQ(replay.total_samples(), uint64_t{20'000});
    CHECK_EQ(replay.center_frequency(), uint64_t{868'000'000});
    CHECK_NEAR(replay.file_sample_rate(), 2'000'000.0, 1e-9);
    CHECK_NEAR(replay.duration_seconds(), 0.01, 1e-9);
    CHECK(replay.state() == radio::ReplayModel::State::Stopped);
    CHECK_NEAR(replay.progress(), 0.0, 1e-12);
}

TEST(replay_open_rejects_a_missing_file) {
    TempDir dir{"replaymissing"};

    radio::ReplayModel replay;
    CHECK(!replay.open(dir.file("gone.C16")));
    CHECK(!replay.is_open());
    CHECK(!replay.error().empty());
    CHECK(!replay.play());
}

TEST(replay_without_a_sink_refuses_to_play) {
    TempDir dir{"replaynosink"};
    const std::string path = make_capture(dir, "P.C16", 1000, 500'000);

    radio::ReplayModel replay;
    CHECK(replay.open(path));
    CHECK(!replay.has_sink());
    CHECK(!replay.play());
    CHECK(!replay.error().empty());
    CHECK(replay.state() == radio::ReplayModel::State::Stopped);
}

TEST(replay_streams_the_whole_file_in_order) {
    TempDir dir{"replayall"};
    constexpr uint64_t count = 20'000;
    const std::string path = make_capture(dir, "P.C16", count, 2'000'000);

    CollectingSink sink;
    radio::ReplayModel replay;
    CHECK(replay.set_sink(sink.fn()));
    CHECK(replay.open(path));
    CHECK(replay.play());
    CHECK(replay.playing());

    CHECK(wait_until([&] { return replay.finished(); }, 30'000));

    /* Without resampling every file sample reaches the sink exactly once. */
    CHECK_EQ(replay.samples_delivered(), count);
    CHECK_EQ(sink.size(), static_cast<size_t>(count));
    CHECK_EQ(replay.position_samples(), count);
    CHECK_NEAR(replay.progress(), 1.0, 1e-12);
    CHECK(replay.state() == radio::ReplayModel::State::Stopped);

    std::lock_guard<std::mutex> g{sink.mutex};
    CHECK_NEAR(sink.samples.front().real(), ramp_sample(0).real(), 2.0e-5);
    CHECK_NEAR(sink.samples[777].real(), ramp_sample(777).real(), 2.0e-5);
    CHECK_NEAR(sink.samples.back().imag(), ramp_sample(count - 1).imag(), 2.0e-5);
}

TEST(replay_resamples_to_the_output_rate) {
    TempDir dir{"replayresamp"};
    constexpr uint64_t count = 10'000;
    const std::string path = make_capture(dir, "P.C16", count, 1'000'000);

    CollectingSink sink;
    radio::ReplayModel replay;
    CHECK(replay.set_sink(sink.fn()));
    CHECK(replay.open(path));

    /* Twice the file's rate: the linear resampler emits exactly two output
     * samples per input sample for a ratio of 1:2. */
    replay.set_output_sample_rate(2'000'000.0);
    CHECK_NEAR(replay.effective_output_rate(), 2'000'000.0, 1e-9);

    CHECK(replay.play());
    CHECK(wait_until([&] { return replay.finished(); }, 30'000));

    CHECK_EQ(replay.samples_delivered(), count * 2);
    CHECK_EQ(replay.position_samples(), count);
    /* Time on air is unchanged: twice the samples at twice the rate. */
    CHECK_NEAR(replay.duration_seconds(), 0.01, 1e-9);
}

TEST(replay_feeds_a_ring_buffer_sink) {
    TempDir dir{"replayring"};
    constexpr uint64_t count = 4000;
    const std::string path = make_capture(dir, "P.C16", count, 2'000'000);

    dsp::RingBuffer<dsp::cfloat> ring{1 << 14};

    radio::ReplayModel replay;
    CHECK(replay.set_ring_sink(&ring));
    CHECK(replay.has_sink());
    CHECK(replay.open(path));
    CHECK(replay.play());
    CHECK(wait_until([&] { return replay.finished(); }, 30'000));

    CHECK_EQ(replay.samples_delivered(), count);

    std::vector<dsp::cfloat> drained(count);
    CHECK_EQ(ring.read(drained.data(), drained.size()), static_cast<size_t>(count));
    CHECK_NEAR(drained[0].real(), ramp_sample(0).real(), 2.0e-5);
    CHECK_NEAR(drained[count - 1].real(), ramp_sample(count - 1).real(), 2.0e-5);
}

TEST(replay_stalls_rather_than_dropping_when_the_sink_is_full) {
    TempDir dir{"replaystall"};
    constexpr uint64_t count = 20'000;
    const std::string path = make_capture(dir, "P.C16", count, 2'000'000);

    /* A ring far too small for the file: playback must wait for space, not
     * throw samples away. */
    dsp::RingBuffer<dsp::cfloat> ring{1024};

    radio::ReplayModel replay;
    CHECK(replay.set_ring_sink(&ring));
    CHECK(replay.open(path));
    CHECK(replay.play());

    std::vector<dsp::cfloat> drained;
    drained.reserve(count);
    std::vector<dsp::cfloat> chunk(512);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (drained.size() < count && std::chrono::steady_clock::now() < deadline) {
        const size_t got = ring.read(chunk.data(), chunk.size());
        drained.insert(drained.end(), chunk.begin(),
                       chunk.begin() + static_cast<ptrdiff_t>(got));
        if (got == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    CHECK_EQ(drained.size(), static_cast<size_t>(count));
    CHECK(replay.stalls() > 0);
    /* Order is preserved across the stalls. */
    CHECK_NEAR(drained[19'999].imag(), ramp_sample(19'999).imag(), 2.0e-5);
    replay.stop();
}

TEST(replay_loops_until_stopped) {
    TempDir dir{"replayloop"};
    constexpr uint64_t count = 4000;
    const std::string path = make_capture(dir, "P.C16", count, 2'000'000);

    CollectingSink sink;
    radio::ReplayModel replay;
    CHECK(replay.set_sink(sink.fn()));
    CHECK(replay.open(path));
    replay.set_loop(true);
    CHECK(replay.loop());
    CHECK(replay.play());

    CHECK(wait_until([&] { return replay.loops_completed() >= 2; }, 5000));
    replay.stop();

    CHECK(replay.loops_completed() >= 2);
    CHECK(!replay.finished());
    CHECK(replay.state() == radio::ReplayModel::State::Stopped);
    /* At least two full passes reached the sink. */
    CHECK(replay.samples_delivered() >= count * 2);

    /* stop() rewinds. */
    CHECK_EQ(replay.position_samples(), uint64_t{0});
}

TEST(replay_pause_freezes_the_position) {
    TempDir dir{"replaypause"};
    /* 50 kHz over 25 000 samples is half a second of signal, long enough that
     * a pause visibly stops it. */
    constexpr uint64_t count = 25'000;
    const std::string path = make_capture(dir, "P.C16", count, 50'000);

    CollectingSink sink;
    radio::ReplayModel replay;
    CHECK(replay.set_sink(sink.fn()));
    CHECK(replay.open(path));
    replay.set_prefill_ms(0.0);
    CHECK(replay.play());

    CHECK(wait_until([&] { return replay.position_samples() > 0; }, 2000));
    replay.pause();
    CHECK(replay.paused());

    const uint64_t at_pause = replay.position_samples();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    CHECK_EQ(replay.position_samples(), at_pause);
    CHECK(!replay.finished());

    /* Resuming does not skip ahead to make up the paused time. */
    CHECK(replay.resume());
    CHECK(replay.playing());
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    CHECK(replay.position_samples() > at_pause);
    CHECK(replay.position_samples() < count);

    replay.stop();
}

TEST(replay_paces_at_the_file_sample_rate) {
    TempDir dir{"replaypace"};
    /* Half a second of signal with no prefill: after 60 ms only a fraction of
     * it can have gone out if the pacing clock is doing its job. */
    constexpr uint64_t count = 25'000;
    const std::string path = make_capture(dir, "P.C16", count, 50'000);

    CollectingSink sink;
    radio::ReplayModel replay;
    CHECK(replay.set_sink(sink.fn()));
    CHECK(replay.open(path));
    replay.set_prefill_ms(0.0);
    CHECK_NEAR(replay.duration_seconds(), 0.5, 1e-9);

    const auto started = std::chrono::steady_clock::now();
    CHECK(replay.play());
    std::this_thread::sleep_for(std::chrono::milliseconds(60));

    const uint64_t after_60ms = replay.samples_delivered();
    CHECK(after_60ms > 0);
    /* 60 ms of a 500 ms file is 12%; allow a wide margin for scheduler
     * granularity but insist it is nothing like the whole file. */
    CHECK(after_60ms < count / 2);

    CHECK(wait_until([&] { return replay.finished(); }, 30'000));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const double seconds = std::chrono::duration<double>(elapsed).count();

    CHECK_EQ(replay.samples_delivered(), count);
    /* It took roughly the file's own duration, not the disk's. */
    CHECK(seconds > 0.4);
    CHECK(seconds < 2.0);
}

TEST(replay_seek_moves_the_read_point) {
    TempDir dir{"replayseek"};
    constexpr uint64_t count = 20'000;
    const std::string path = make_capture(dir, "P.C16", count, 2'000'000);

    CollectingSink sink;
    radio::ReplayModel replay;
    CHECK(replay.set_sink(sink.fn()));
    CHECK(replay.open(path));

    CHECK(replay.seek_samples(15'000));
    CHECK_EQ(replay.position_samples(), uint64_t{15'000});
    CHECK_NEAR(replay.progress(), 0.75, 1e-12);

    CHECK(replay.play());
    CHECK(wait_until([&] { return replay.finished(); }, 30'000));

    /* Only the tail after the seek point went out. */
    CHECK_EQ(replay.samples_delivered(), uint64_t{5000});
    {
        std::lock_guard<std::mutex> g{sink.mutex};
        CHECK_NEAR(sink.samples.front().real(), ramp_sample(15'000).real(), 2.0e-5);
    }

    /* Seeking by time uses the file's sample rate. */
    CHECK(replay.seek_seconds(0.005));
    CHECK_EQ(replay.position_samples(), uint64_t{10'000});

    /* Past the end clamps rather than failing. */
    CHECK(replay.seek_seconds(60.0));
    CHECK_EQ(replay.position_samples(), count);

    CHECK(!replay.seek_samples(count + 1));
    CHECK(!replay.error().empty());
}

TEST(replay_play_after_finishing_starts_over) {
    TempDir dir{"replayrestart"};
    constexpr uint64_t count = 4000;
    const std::string path = make_capture(dir, "P.C16", count, 2'000'000);

    CollectingSink sink;
    radio::ReplayModel replay;
    CHECK(replay.set_sink(sink.fn()));
    CHECK(replay.open(path));

    CHECK(replay.play());
    CHECK(wait_until([&] { return replay.finished(); }, 30'000));
    CHECK_EQ(replay.samples_delivered(), count);

    /* A second play() rewinds instead of returning immediately at EOF. */
    CHECK(replay.play());
    CHECK(wait_until([&] { return replay.finished(); }, 30'000));
    CHECK_EQ(replay.samples_delivered(), count);
    CHECK_EQ(sink.size(), static_cast<size_t>(count * 2));
}

TEST(replay_falls_back_to_the_default_rate_without_a_sidecar) {
    TempDir dir{"replaynometa"};
    const std::string path = make_capture(dir, "P.C16", 10'000, 0, 0, false);

    radio::ReplayModel replay;
    /* Upstream's PlaylistView falls back to 500 kHz; here it is settable, and
     * has_metadata() tells the UI the rate is a guess. */
    CHECK_NEAR(replay.default_sample_rate(), 500'000.0, 1e-9);
    replay.set_default_sample_rate(1'000'000.0);
    CHECK(replay.open(path));
    CHECK(!replay.has_metadata());
    CHECK_NEAR(replay.file_sample_rate(), 1'000'000.0, 1e-9);
    CHECK_NEAR(replay.duration_seconds(), 0.01, 1e-9);
    CHECK_EQ(replay.center_frequency(), uint64_t{0});
}

TEST(replay_refuses_an_empty_capture) {
    TempDir dir{"replayempty"};
    const std::string path = dir.file("Z.C16");
    write_text(path, "");

    CollectingSink sink;
    radio::ReplayModel replay;
    CHECK(replay.set_sink(sink.fn()));
    CHECK(replay.open(path));
    CHECK_EQ(replay.total_samples(), uint64_t{0});

    /* Playing nothing forever, especially with loop on, is worse than a clear
     * refusal. */
    replay.set_loop(true);
    CHECK(!replay.play());
    CHECK(!replay.error().empty());
    CHECK(replay.state() == radio::ReplayModel::State::Stopped);
}

TEST(replay_rejects_a_sink_change_while_playing) {
    TempDir dir{"replayswap"};
    const std::string path = make_capture(dir, "P.C16", 25'000, 50'000);

    CollectingSink sink;
    radio::ReplayModel replay;
    CHECK(replay.set_sink(sink.fn()));
    CHECK(replay.open(path));
    replay.set_prefill_ms(0.0);
    CHECK(replay.play());

    CollectingSink other;
    CHECK(!replay.set_sink(other.fn()));
    CHECK(!replay.clear_sink());
    CHECK(!replay.error().empty());

    replay.stop();

    /* Stopped, it can be rewired. */
    CHECK(replay.set_sink(other.fn()));
    CHECK(replay.clear_sink());
    CHECK(!replay.has_sink());
}
