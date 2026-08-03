/*
 * mayhem-b200 — DOOM shell IWAD-discovery tests.
 *
 * The DOOM app is a shell only (no engine); the one piece of real, testable
 * logic is locating an IWAD file. These tests exercise doom_wad::find_wad()
 * against a temporary directory the test creates and removes itself, covering:
 * absent, present-and-valid (IWAD / PWAD magic), present-but-not-a-WAD, a file
 * too short to carry the 4-byte magic, and the candidate-name priority order.
 *
 * The WAD magic ("IWAD"/"PWAD" as the first four bytes) is from the id Software
 * WAD format; the candidate filename list is what a doomgeneric port looks for.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "file_path.hpp"
#include "fs_utils.hpp"
#include "ui_doom.hpp"

#include <chrono>
#include <filesystem>
#include <string>

using namespace mb200test;
using namespace app::doom_wad;

namespace {

namespace fs = std::filesystem;

class TempDir {
   public:
    TempDir() {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::error_code ec;
        path_ = fs::temp_directory_path(ec) /
                ("mb200_doom_test_" + std::to_string(stamp) + "_" +
                 std::to_string(counter++));
        fs::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string str() const { return path_.generic_string(); }
    std::string sub(const std::string& leaf) const {
        return core::path_join(str(), leaf);
    }

   private:
    fs::path path_{};
};

void put_file(const std::string& path, const std::string& content) {
    const auto r = core::write_file(path, content);
    if (!r) report_failure(__FILE__, __LINE__, "setup: " + r.message);
}

}  // namespace

/* ---- candidate list and expected directory -------------------------------- */

TEST(doom_iwad_names_nonempty_primary_is_doom_wad) {
    const auto& names = iwad_names();
    CHECK(!names.empty());
    CHECK_STR_EQ(names.front(), "doom.wad");
}

TEST(doom_wad_directory_is_DOOM_under_data_dir) {
    const std::string dir = wad_directory();
    CHECK_STR_EQ(core::filename(dir), "DOOM");
    CHECK_STR_EQ(dir, core::path_join(core::data_directory(), "DOOM"));
    CHECK(core::is_within_data_directory(dir));
}

/* ---- absent -------------------------------------------------------------- */

TEST(doom_find_wad_absent) {
    TempDir tmp;
    const auto s = find_wad(tmp.str());
    CHECK(!s.present);
    CHECK(!s.looks_valid);
    CHECK_EQ(s.size, (std::uintmax_t)0);
    /* Reports the primary expected path so the UI can tell the user where. */
    CHECK_STR_EQ(core::filename(s.path), "doom.wad");
}

/* ---- present and valid --------------------------------------------------- */

TEST(doom_find_wad_present_valid_iwad) {
    TempDir tmp;
    const std::string content = std::string("IWAD") + std::string(60, '\0');
    put_file(tmp.sub("doom.wad"), content);

    const auto s = find_wad(tmp.str());
    CHECK(s.present);
    CHECK(s.looks_valid);
    CHECK_EQ(s.size, (std::uintmax_t)content.size());
    CHECK_STR_EQ(core::filename(s.path), "doom.wad");
}

TEST(doom_find_wad_present_valid_pwad) {
    TempDir tmp;
    const std::string content = std::string("PWAD") + std::string(20, 'x');
    put_file(tmp.sub("doom2.wad"), content);  /* only a lower-priority name */

    const auto s = find_wad(tmp.str());
    CHECK(s.present);
    CHECK(s.looks_valid);
    CHECK_STR_EQ(core::filename(s.path), "doom2.wad");
}

/* ---- present but not a WAD ------------------------------------------------ */

TEST(doom_find_wad_present_bad_magic) {
    TempDir tmp;
    put_file(tmp.sub("doom.wad"), "NOPE and then some bytes");

    const auto s = find_wad(tmp.str());
    CHECK(s.present);
    CHECK(!s.looks_valid);
}

TEST(doom_find_wad_present_too_short_for_magic) {
    TempDir tmp;
    put_file(tmp.sub("doom.wad"), "AB");  /* < 4 bytes */

    const auto s = find_wad(tmp.str());
    CHECK(s.present);
    CHECK(!s.looks_valid);
    CHECK_EQ(s.size, (std::uintmax_t)2);
}

/* ---- priority: doom.wad wins over a lower-priority candidate -------------- */

TEST(doom_find_wad_priority_order) {
    TempDir tmp;
    put_file(tmp.sub("doom2.wad"), std::string("IWAD") + std::string(8, 'a'));
    put_file(tmp.sub("doom.wad"), std::string("IWAD") + std::string(8, 'b'));

    const auto s = find_wad(tmp.str());
    CHECK(s.present);
    CHECK_STR_EQ(core::filename(s.path), "doom.wad");
}
