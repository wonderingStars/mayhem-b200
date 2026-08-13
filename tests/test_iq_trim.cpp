/*
 * mayhem-b200 — IQ Trim power-scan and trim tests.
 *
 * The expected results here are derived from the spec and from upstream's
 * iq::profile_capture / iq::compute_trim_range (application/iq_trim.cpp): power
 * is I*I + Q*Q on the raw stored integers, the trim range is the first and last
 * buckets above cutoff_percent of the peak power, and the end bucket is the one
 * *after* the last with signal. Synthetic captures with a known quiet-loud-quiet
 * envelope pin the detected range to the loud region; an all-quiet capture must
 * report no active region rather than the whole file.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include <process.h> /* _getpid: fixture dirs must be unique PER PROCESS */

#include "ui_iq_trim.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace {

using IQ16 = std::pair<int16_t, int16_t>;
using IQ8 = std::pair<int8_t, int8_t>;

/* A self-removing temp directory, so a failed test leaves no IQ behind. */
class TempDir {
   public:
    explicit TempDir(const char* tag) {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mb200_trim_" + std::string{tag} + "_" +
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

void write_c16(const std::string& path, const std::vector<IQ16>& samples) {
    std::ofstream f{path, std::ios::binary | std::ios::trunc};
    for (const auto& s : samples) {
        const int16_t iq[2] = {s.first, s.second};
        f.write(reinterpret_cast<const char*>(iq), sizeof(iq));
    }
}

std::vector<IQ16> read_c16(const std::string& path) {
    std::ifstream f{path, std::ios::binary};
    std::vector<IQ16> out;
    int16_t iq[2];
    while (f.read(reinterpret_cast<char*>(iq), sizeof(iq)))
        out.emplace_back(iq[0], iq[1]);
    return out;
}

uint64_t file_bytes(const std::string& path) {
    std::error_code ec;
    const auto n = std::filesystem::file_size(path, ec);
    return ec ? 0 : static_cast<uint64_t>(n);
}

/* quiet [0,q) — loud [q, q+l) — quiet [q+l, q+l+q).  loud = (32767, 0). */
std::vector<IQ16> envelope(size_t quiet, size_t loud) {
    std::vector<IQ16> v;
    v.reserve(2 * quiet + loud);
    for (size_t i = 0; i < quiet; ++i) v.emplace_back(0, 0);
    for (size_t i = 0; i < loud; ++i) v.emplace_back(32767, 0);
    for (size_t i = 0; i < quiet; ++i) v.emplace_back(0, 0);
    return v;
}

}  // namespace

/* --- profile_capture ------------------------------------------------------- */

TEST(profile_reports_capture_metadata) {
    TempDir dir{"meta"};
    const std::string path = dir.file("E.C16");
    write_c16(path, envelope(10'000, 10'000));  // 30 000 samples total

    std::vector<iqtrim::PowerBuckets::Bucket> buckets_data(30);
    iqtrim::PowerBuckets buckets{buckets_data.data(), buckets_data.size()};

    const auto info = iqtrim::profile_capture(path, buckets);
    CHECK(info.has_value());
    CHECK_EQ(info->sample_count, uint64_t{30'000});
    CHECK_EQ(static_cast<int>(info->sample_size), 4);
    CHECK_EQ(info->file_size, uint64_t{120'000});
    /* loud sample is (32767, 0): power = 32767^2, iq_max = 32767. */
    CHECK_EQ(info->max_power, uint32_t{1'073'676'289});
    CHECK_EQ(info->max_iq, uint32_t{32'767});
}

TEST(profile_rejects_unknown_and_missing_files) {
    TempDir dir{"bad"};

    /* Unknown extension: not a claimed IQ format. */
    const std::string bin = dir.file("X.BIN");
    write_c16(bin, {{1, 1}});
    std::vector<iqtrim::PowerBuckets::Bucket> b1(8);
    iqtrim::PowerBuckets pb1{b1.data(), b1.size()};
    CHECK(!iqtrim::profile_capture(bin, pb1).has_value());

    /* Missing file with a valid extension. */
    std::vector<iqtrim::PowerBuckets::Bucket> b2(8);
    iqtrim::PowerBuckets pb2{b2.data(), b2.size()};
    CHECK(!iqtrim::profile_capture(dir.file("gone.C16"), pb2).has_value());
}

/* --- compute_trim_range ---------------------------------------------------- */

TEST(trim_range_brackets_the_loud_region) {
    TempDir dir{"bracket"};
    const std::string path = dir.file("E.C16");
    /* 10k quiet, 10k loud, 10k quiet. 30 buckets => 1000 samples/bucket, so the
     * loud region is exactly buckets 10..19. */
    write_c16(path, envelope(10'000, 10'000));

    std::vector<iqtrim::PowerBuckets::Bucket> buckets_data(30);
    iqtrim::PowerBuckets buckets{buckets_data.data(), buckets_data.size()};

    const auto info = iqtrim::profile_capture(path, buckets);
    CHECK(info.has_value());

    const auto range = iqtrim::compute_trim_range(*info, buckets, 7);
    CHECK(range.has_signal);
    CHECK_EQ(static_cast<int>(range.sample_size), 4);

    /* end is the first bucket after the last with signal: bucket 20 => 20000. */
    CHECK_EQ(range.start_sample, uint64_t{10'000});
    CHECK_EQ(range.end_sample, uint64_t{20'000});

    /* The detected window brackets the loud region [10000, 20000). */
    CHECK(range.start_sample <= 10'000);
    CHECK(range.end_sample >= 20'000);
    /* ...and is nothing like the whole file. */
    CHECK(range.end_sample < info->sample_count);
}

TEST(trim_range_is_stable_across_cutoffs) {
    TempDir dir{"cutoffs"};
    const std::string path = dir.file("E.C16");
    write_c16(path, envelope(10'000, 10'000));

    std::vector<iqtrim::PowerBuckets::Bucket> buckets_data(30);
    iqtrim::PowerBuckets buckets{buckets_data.data(), buckets_data.size()};
    const auto info = iqtrim::profile_capture(path, buckets);
    CHECK(info.has_value());

    /* Quiet buckets are pure silence (power 0), so any 1..99 cutoff keeps only
     * the loud buckets — the window does not drift with the threshold. */
    for (uint8_t cutoff : {uint8_t{1}, uint8_t{7}, uint8_t{50}, uint8_t{99}}) {
        const auto range = iqtrim::compute_trim_range(*info, buckets, cutoff);
        CHECK(range.has_signal);
        CHECK_EQ(range.start_sample, uint64_t{10'000});
        CHECK_EQ(range.end_sample, uint64_t{20'000});
    }
}

TEST(entirely_quiet_capture_reports_no_active_region) {
    TempDir dir{"quiet"};
    const std::string path = dir.file("Q.C16");

    /* 30 000 samples of pure silence. */
    write_c16(path, std::vector<IQ16>(30'000, IQ16{0, 0}));

    std::vector<iqtrim::PowerBuckets::Bucket> buckets_data(30);
    iqtrim::PowerBuckets buckets{buckets_data.data(), buckets_data.size()};

    const auto info = iqtrim::profile_capture(path, buckets);
    CHECK(info.has_value());
    CHECK_EQ(info->max_power, uint32_t{0});

    const auto range = iqtrim::compute_trim_range(*info, buckets, 7);
    CHECK(!range.has_signal);
    /* No active region: an empty range, NOT the whole file. */
    CHECK_EQ(range.start_sample, range.end_sample);
    CHECK_EQ(range.start_sample, uint64_t{0});
    CHECK(range.end_sample < info->sample_count);
}

TEST(compute_range_handles_zero_buckets) {
    /* Degenerate but must not divide by zero or read a null bucket array. */
    iqtrim::CaptureInfo info{};
    info.sample_count = 1000;
    info.sample_size = 4;
    iqtrim::PowerBuckets empty{nullptr, 0};
    const auto range = iqtrim::compute_trim_range(info, empty, 7);
    CHECK(!range.has_signal);
    CHECK_EQ(range.start_sample, uint64_t{0});
    CHECK_EQ(range.end_sample, uint64_t{0});
}

/* --- amplify_iq_buffer ----------------------------------------------------- */

TEST(amplify_c16_scales_and_clamps_every_sample) {
    /* 8 int16 = 4 I/Q pairs. The last elements catch upstream's mult_count bug,
     * which would leave the tail of the block untouched. */
    int16_t buf[8] = {100, -100, 20000, -20000, 0, 5, -5, 30000};
    iqtrim::amplify_iq_buffer(reinterpret_cast<uint8_t*>(buf), sizeof(buf), 2, 4);

    CHECK_EQ(buf[0], int16_t{200});
    CHECK_EQ(buf[1], int16_t{-200});
    CHECK_EQ(buf[2], int16_t{32767});   // 40000 clamped
    CHECK_EQ(buf[3], int16_t{-32767});  // -40000 clamped
    CHECK_EQ(buf[4], int16_t{0});
    CHECK_EQ(buf[5], int16_t{10});
    CHECK_EQ(buf[6], int16_t{-10});     // tail: proves the whole block is done
    CHECK_EQ(buf[7], int16_t{32767});   // 60000 clamped
}

TEST(amplify_c8_scales_and_clamps_every_sample) {
    int8_t buf[8] = {10, -10, 100, -100, 0, 3, -3, 120};
    iqtrim::amplify_iq_buffer(reinterpret_cast<uint8_t*>(buf), sizeof(buf), 2, 2);

    CHECK_EQ(buf[0], int8_t{20});
    CHECK_EQ(buf[1], int8_t{-20});
    CHECK_EQ(buf[2], int8_t{127});   // 200 clamped
    CHECK_EQ(buf[3], int8_t{-127});  // -200 clamped
    CHECK_EQ(buf[4], int8_t{0});
    CHECK_EQ(buf[5], int8_t{6});
    CHECK_EQ(buf[6], int8_t{-6});    // tail
    CHECK_EQ(buf[7], int8_t{127});   // 240 clamped
}

TEST(amplify_by_one_is_a_no_op_shape) {
    int16_t buf[4] = {123, -456, 789, -1011};
    iqtrim::amplify_iq_buffer(reinterpret_cast<uint8_t*>(buf), sizeof(buf), 1, 4);
    CHECK_EQ(buf[0], int16_t{123});
    CHECK_EQ(buf[1], int16_t{-456});
    CHECK_EQ(buf[2], int16_t{789});
    CHECK_EQ(buf[3], int16_t{-1011});
}

/* --- trim_capture_with_range ----------------------------------------------- */

TEST(trim_extracts_the_requested_sample_range) {
    TempDir dir{"trim"};
    const std::string path = dir.file("R.C16");

    /* sample i = (i*100, -i*100), i in [0, 10). */
    std::vector<IQ16> in;
    for (int i = 0; i < 10; ++i)
        in.emplace_back(static_cast<int16_t>(i * 100), static_cast<int16_t>(-i * 100));
    write_c16(path, in);

    iqtrim::TrimRange range{3, 7, 4, true};  // keep samples 3,4,5,6
    CHECK(iqtrim::trim_capture_with_range(path, range, nullptr, 1));

    CHECK_EQ(file_bytes(path), uint64_t{4 * 4});  // 4 samples, 4 bytes each
    const auto out = read_c16(path);
    CHECK_EQ(out.size(), size_t{4});
    CHECK_EQ(out[0], (IQ16{300, -300}));
    CHECK_EQ(out[1], (IQ16{400, -400}));
    CHECK_EQ(out[2], (IQ16{500, -500}));
    CHECK_EQ(out[3], (IQ16{600, -600}));
}

TEST(trim_applies_amplification_to_the_kept_range) {
    TempDir dir{"trimamp"};
    const std::string path = dir.file("A.C16");

    std::vector<IQ16> in;
    for (int i = 0; i < 10; ++i)
        in.emplace_back(static_cast<int16_t>(i * 100), static_cast<int16_t>(-i * 100));
    write_c16(path, in);

    iqtrim::TrimRange range{2, 5, 4, true};  // keep samples 2,3,4
    CHECK(iqtrim::trim_capture_with_range(path, range, nullptr, 3));

    const auto out = read_c16(path);
    CHECK_EQ(out.size(), size_t{3});
    CHECK_EQ(out[0], (IQ16{600, -600}));     // (200,-200) * 3
    CHECK_EQ(out[1], (IQ16{900, -900}));     // (300,-300) * 3
    CHECK_EQ(out[2], (IQ16{1200, -1200}));   // (400,-400) * 3
}

TEST(trim_rejects_an_invalid_range) {
    TempDir dir{"trimbad"};
    const std::string path = dir.file("B.C16");
    std::vector<IQ16> in(10, IQ16{7, 7});
    write_c16(path, in);

    /* Empty and inverted ranges are refused, leaving the file untouched. */
    CHECK(!iqtrim::trim_capture_with_range(path, {5, 5, 4, true}, nullptr, 1));
    CHECK(!iqtrim::trim_capture_with_range(path, {7, 3, 4, true}, nullptr, 1));
    CHECK(!iqtrim::trim_capture_with_range(path, {0, 4, 0, false}, nullptr, 1));

    CHECK_EQ(file_bytes(path), uint64_t{10 * 4});  // still all ten samples
}

/* --- end to end: profile -> compute -> trim -------------------------------- */

TEST(profile_then_trim_keeps_only_the_active_region) {
    TempDir dir{"e2e"};
    const std::string path = dir.file("E.C16");
    write_c16(path, envelope(10'000, 10'000));

    std::vector<iqtrim::PowerBuckets::Bucket> buckets_data(30);
    iqtrim::PowerBuckets buckets{buckets_data.data(), buckets_data.size()};

    const auto info = iqtrim::profile_capture(path, buckets);
    CHECK(info.has_value());
    const auto range = iqtrim::compute_trim_range(*info, buckets, 7);
    CHECK(range.has_signal);

    CHECK(iqtrim::trim_capture_with_range(path, range, nullptr, 1));

    /* The trimmed file is exactly the 10 000 loud samples. */
    CHECK_EQ(file_bytes(path), uint64_t{10'000 * 4});
    const auto out = read_c16(path);
    CHECK_EQ(out.size(), size_t{10'000});
    CHECK_EQ(out.front(), (IQ16{32767, 0}));
    CHECK_EQ(out.back(), (IQ16{32767, 0}));
}
