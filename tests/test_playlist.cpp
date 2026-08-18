/*
 * mayhem-b200 — Replay / Playlist-editor tests.
 *
 * Two things are checkable without hardware:
 *   1. the .PPL parse/write helpers shared by both apps (app::parse_ppl_line,
 *      format_ppl_line, parse_ppl) — tested against the format upstream's
 *      PlaylistView::load_file / save_file and PlaylistItemEditView use;
 *   2. the progress/position maths the Replay UI reads off radio::ReplayModel,
 *      exercised with a small synthetic capture written via core::iq_file.
 *
 * RF output itself needs a USRP B200 and is not covered here.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"


#include "iq_file.hpp"
#include "replay_model.hpp"
#include "string_format.hpp"
#include "ui_playlist_editor.hpp"  /* app:: PPL helpers */

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using core::cfloat;

class TempDir {
   public:
    explicit TempDir(const char* tag) {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mb200_ppl_" + std::string{tag} + "_" +
                 std::to_string(mb200test::test_pid()) + "_" +
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

/* Writes `count` trivial samples plus (optionally) a matching .TXT sidecar. */
std::string make_capture(const TempDir& dir,
                         const std::string& name,
                         uint64_t count,
                         uint32_t sample_rate,
                         uint64_t center_frequency = 433'920'000,
                         bool write_sidecar = true) {
    const std::string path = dir.file(name);

    core::IqFileWriter writer;
    if (!writer.open(path)) return {};

    std::vector<cfloat> block(256, cfloat{0.1f, -0.1f});
    uint64_t written = 0;
    while (written < count) {
        const size_t n = static_cast<size_t>(
            std::min<uint64_t>(block.size(), count - written));
        writer.write(block.data(), n);
        written += n;
    }
    writer.close();

    if (write_sidecar) {
        core::CaptureMetadata md{};
        md.center_frequency = center_frequency;
        md.sample_rate = sample_rate;
        core::write_metadata_file(core::metadata_path_for(path), md);
    }
    return path;
}

std::vector<std::string> read_lines(const std::string& path) {
    std::vector<std::string> out;
    std::ifstream f{path, std::ios::binary};
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

}  // namespace

/* --- .PPL line parsing ----------------------------------------------------- */

TEST(ppl_line_round_trips) {
    const app::PplEntry in{"/CAPTURES/FOO.C16", 250};
    const std::string line = app::format_ppl_line(in);
    CHECK_STR_EQ(line, "/CAPTURES/FOO.C16,250");

    app::PplEntry out;
    CHECK(app::parse_ppl_line(line, out));
    CHECK_STR_EQ(out.path, in.path);
    CHECK_EQ(out.delay_ms, in.delay_ms);
}

TEST(ppl_zero_delay_round_trips) {
    /* Upstream build_item always writes the delay, even when it is 0. */
    const std::string line = app::format_ppl_line({"A.C16", 0});
    CHECK_STR_EQ(line, "A.C16,0");

    app::PplEntry out;
    CHECK(app::parse_ppl_line(line, out));
    CHECK_STR_EQ(out.path, "A.C16");
    CHECK_EQ(out.delay_ms, uint32_t{0});
}

TEST(ppl_parse_trims_whitespace) {
    app::PplEntry out;
    CHECK(app::parse_ppl_line("  a/b.C16 , 100 ", out));
    CHECK_STR_EQ(out.path, "a/b.C16");
    CHECK_EQ(out.delay_ms, uint32_t{100});
}

TEST(ppl_parse_path_only_defaults_delay_to_zero) {
    app::PplEntry out;
    CHECK(app::parse_ppl_line("solo.C16", out));
    CHECK_STR_EQ(out.path, "solo.C16");
    CHECK_EQ(out.delay_ms, uint32_t{0});
}

TEST(ppl_parse_rejects_comments_and_blanks) {
    app::PplEntry out;
    CHECK(!app::parse_ppl_line("# a comment", out));
    CHECK(!app::parse_ppl_line("", out));
    CHECK(!app::parse_ppl_line("   ", out));  /* trims to empty path */
}

TEST(ppl_parse_uint_stops_at_first_nondigit) {
    /* The host stand-in for upstream's atoi/parse_int on the delay column. */
    CHECK_EQ(app::ppl_parse_uint("123"), uint32_t{123});
    CHECK_EQ(app::ppl_parse_uint("123abc"), uint32_t{123});
    CHECK_EQ(app::ppl_parse_uint(""), uint32_t{0});
    CHECK_EQ(app::ppl_parse_uint("abc"), uint32_t{0});
    CHECK_EQ(app::ppl_parse_uint("99999"), uint32_t{99999});
}

TEST(ppl_parse_extra_columns_are_ignored) {
    /* Upstream split_string keeps only cols[0] (path) and cols[1] (delay). */
    app::PplEntry out;
    CHECK(app::parse_ppl_line("x.C16,42,junk,more", out));
    CHECK_STR_EQ(out.path, "x.C16");
    CHECK_EQ(out.delay_ms, uint32_t{42});
}

TEST(ppl_parse_whole_file_skips_noise) {
    const std::vector<std::string> lines = {
        "# header comment",
        "",
        "one.C16,0",
        "   ",
        "two.C16,500",
        "# trailing note",
        "three.C8",
    };

    const auto entries = app::parse_ppl(lines);
    CHECK_EQ(entries.size(), size_t{3});
    CHECK_STR_EQ(entries[0].path, "one.C16");
    CHECK_EQ(entries[0].delay_ms, uint32_t{0});
    CHECK_STR_EQ(entries[1].path, "two.C16");
    CHECK_EQ(entries[1].delay_ms, uint32_t{500});
    CHECK_STR_EQ(entries[2].path, "three.C8");
    CHECK_EQ(entries[2].delay_ms, uint32_t{0});
}

/* --- .PPL file save/load (the real I/O path both apps use) ----------------- */

TEST(ppl_file_save_load_round_trip) {
    TempDir dir{"io"};
    const std::string path = dir.file("LIST.PPL");

    const std::vector<app::PplEntry> original = {
        {"/CAPTURES/A.C16", 0},
        {"/CAPTURES/B.C16", 1500},
        {"/CAPTURES/C.C8", 250},
    };

    /* Write exactly as PlaylistView::save_playlist does. */
    std::string content;
    for (const auto& e : original)
        content += app::format_ppl_line(e) + "\n";
    {
        std::ofstream f{path, std::ios::binary | std::ios::trunc};
        f.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    /* Read back as load_playlist does (read_lines + parse_ppl). */
    const auto reloaded = app::parse_ppl(read_lines(path));

    CHECK_EQ(reloaded.size(), original.size());
    for (size_t i = 0; i < original.size() && i < reloaded.size(); i++) {
        CHECK_STR_EQ(reloaded[i].path, original[i].path);
        CHECK_EQ(reloaded[i].delay_ms, original[i].delay_ms);
    }
}

/* --- PlaylistItemEditView parse/build (matches the shared helpers) ---------- */

TEST(item_edit_build_matches_format_helper) {
    /* build_item() must produce the same text parse_ppl_line consumes, so an
     * entry survives an edit unchanged. */
    const std::string built = app::format_ppl_line({"cap.C16", 750});
    app::PplEntry back;
    CHECK(app::parse_ppl_line(built, back));
    CHECK_STR_EQ(back.path, "cap.C16");
    CHECK_EQ(back.delay_ms, uint32_t{750});
}

/* --- Replay progress / position maths -------------------------------------- */

TEST(replay_reports_total_samples_and_duration) {
    TempDir dir{"dur"};
    const std::string path = make_capture(dir, "CAP.C16", 48'000, 48'000);
    CHECK(!path.empty());

    radio::ReplayModel rm;
    CHECK(rm.open(path));
    CHECK(rm.has_metadata());
    CHECK_NEAR(rm.file_sample_rate(), 48'000.0, 1.0);
    CHECK_EQ(rm.total_samples(), uint64_t{48'000});
    CHECK_NEAR(rm.duration_seconds(), 1.0, 1e-6);
}

TEST(replay_progress_and_elapsed_track_the_read_point) {
    TempDir dir{"seek"};
    const std::string path = make_capture(dir, "CAP.C16", 48'000, 48'000);

    radio::ReplayModel rm;
    CHECK(rm.open(path));

    /* Fresh open sits at the start. */
    CHECK_NEAR(rm.progress(), 0.0, 1e-9);
    CHECK_NEAR(rm.elapsed_seconds(), 0.0, 1e-9);

    /* Seeking is legal while stopped and moves both progress and elapsed. */
    CHECK(rm.seek_samples(24'000));
    CHECK_EQ(rm.position_samples(), uint64_t{24'000});
    CHECK_NEAR(rm.progress(), 0.5, 1e-6);
    CHECK_NEAR(rm.elapsed_seconds(), 0.5, 1e-6);

    /* Seeking by time lands on the matching sample. */
    CHECK(rm.seek_seconds(0.25));
    CHECK_EQ(rm.position_samples(), uint64_t{12'000});
    CHECK_NEAR(rm.progress(), 0.25, 1e-6);

    /* Seeking to the very end is legal and reads as complete. */
    CHECK(rm.seek_samples(48'000));
    CHECK_NEAR(rm.progress(), 1.0, 1e-9);
}

TEST(replay_progress_is_zero_before_a_file_is_open) {
    radio::ReplayModel rm;
    CHECK_NEAR(rm.progress(), 0.0, 1e-9);
    CHECK_NEAR(rm.duration_seconds(), 0.0, 1e-9);
    CHECK_EQ(rm.total_samples(), uint64_t{0});
}

TEST(replay_falls_back_to_default_rate_without_a_sidecar) {
    /* Mirrors what Replay shows as "no meta": no sidecar => the fallback rate
     * drives the duration/position maths, and has_metadata() is false. */
    TempDir dir{"nometa"};
    const std::string path =
        make_capture(dir, "RAW.C16", 100'000, 0, 0, /*write_sidecar=*/false);

    radio::ReplayModel rm;
    rm.set_default_sample_rate(500'000.0);
    CHECK(rm.open(path));
    CHECK(!rm.has_metadata());
    CHECK_NEAR(rm.file_sample_rate(), 500'000.0, 1.0);
    CHECK_EQ(rm.total_samples(), uint64_t{100'000});
    CHECK_NEAR(rm.duration_seconds(), 0.2, 1e-6);
}

/* --- The metadata the Replay UI displays per entry ------------------------- */

TEST(inspect_gives_the_entry_metadata_replay_shows) {
    TempDir dir{"inspect"};
    const std::string path = make_capture(dir, "CAP.C16", 96'000, 48'000,
                                          915'000'000);

    const core::IqFileInfo info = core::inspect_iq_file(path);
    CHECK(info.valid);
    CHECK(info.has_metadata);
    CHECK_EQ(info.samples, uint64_t{96'000});
    CHECK_EQ(info.file_bytes, uint64_t{96'000 * 4});  /* C16 = 4 bytes/sample */
    CHECK_NEAR(info.sample_rate, 48'000.0, 1.0);
    CHECK_EQ(info.metadata.center_frequency, uint64_t{915'000'000});

    /* Duration used by the on-screen readout: 96000 / 48000 = 2000 ms. */
    CHECK_EQ(info.duration_ms, uint32_t{2000});
    CHECK_EQ(core::iq_duration_ms(info.samples,
                                  static_cast<uint32_t>(info.sample_rate)),
             uint32_t{2000});
}
