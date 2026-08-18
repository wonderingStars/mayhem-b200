/*
 * mayhem-b200 — Hard Reset tests.
 *
 * The app's real work is a settings wipe, so that is what is tested: after a
 * reset every stored value is gone and every accessor falls back to its default
 * (the whole point of a "hard reset"), and the emptied store survives a reload.
 * The *.ini count/delete helpers — the host analog of upstream's SETTINGS-folder
 * clear — are checked against a scratch directory. Expected values come from the
 * behaviour defined in settings.hpp, not from whatever the code happens to do.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"


#include "settings.hpp"
#include "ui_hard_reset.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

using core::Settings;

namespace {

std::string temp_path(const char* name) {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = std::filesystem::current_path();
    /* PID in the name: this path lives in the SHARED system temp dir, and a
     * fixed name is the same file in every concurrently-running test process
     * (one per git worktree is normal here). Concurrent suites then delete or
     * overwrite each other's fixtures mid-test -- observed 2026-08-13 as
     * intermittent read-back failures with the binary md5-pinned. */
    return (dir / (std::to_string(mb200test::test_pid()) + "_" + name)).string();
}

struct ScopedFile {
    std::string path;
    explicit ScopedFile(const std::string& p) : path{p} { remove(); }
    ~ScopedFile() { remove(); }
    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;
    void remove() const {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    bool exists() const {
        std::error_code ec;
        return std::filesystem::exists(path, ec);
    }
};

/* A scratch directory that deletes its whole subtree on the way in and out. */
struct ScopedDir {
    std::filesystem::path path;
    explicit ScopedDir(const char* name) {
        std::error_code ec;
        auto base = std::filesystem::temp_directory_path(ec);
        if (ec) base = std::filesystem::current_path();
        path = base / (std::to_string(mb200test::test_pid()) + "_" + name); /* see temp_path */
        remove();
        std::filesystem::create_directories(path, ec);
    }
    ~ScopedDir() { remove(); }
    ScopedDir(const ScopedDir&) = delete;
    ScopedDir& operator=(const ScopedDir&) = delete;
    void remove() const {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_file(const std::filesystem::path& p, const std::string& text) {
    std::ofstream out{p, std::ios::binary | std::ios::trunc};
    out << text;
}

}  // namespace

/* --- The settings wipe ------------------------------------------------- */

TEST(hard_reset_clears_every_setting) {
    ScopedFile file{temp_path("mb200_hardreset_clear.ini")};

    Settings s{file.path};
    s.set_uint("pmem", "target_frequency", 433'920'000);
    s.set_int("pmem", "correction_ppb", -1500);
    s.set_bool("pmem", "config_splash", false);
    s.set_string("app", "name", "Analog Audio");
    s.set_uint("app", "frequency", 88'500'000);
    CHECK(s.sections().size() > 0);

    /* The reset itself. */
    CHECK(app::hard_reset_clear_settings(s));

    /* In memory: nothing is stored, so has() is false everywhere and every read
     * returns the caller's default rather than the value that was there. */
    CHECK_EQ(s.sections().size(), static_cast<size_t>(0));
    CHECK(!s.has("pmem", "target_frequency"));
    CHECK(!s.has("app", "name"));
    CHECK_EQ(s.get_uint("pmem", "target_frequency", 7), static_cast<uint64_t>(7));
    CHECK_STR_EQ(s.get_string("app", "name", "default"), "default");

    /* On disk: the saved file reloads into an equally empty store. */
    CHECK(file.exists());
    Settings t{file.path};
    CHECK(t.load());
    CHECK_EQ(t.sections().size(), static_cast<size_t>(0));
    CHECK(!t.has("app", "frequency"));
}

TEST(hard_reset_makes_pmem_reads_fall_back_to_defaults) {
    ScopedFile file{temp_path("mb200_hardreset_pmem.ini")};

    /* Stand in for the [pmem] section core::pmem accessors read from. */
    Settings s{file.path};
    s.set_uint("pmem", "target_frequency", 27'000'000);
    CHECK_EQ(s.get_uint("pmem", "target_frequency",
                        core::pmem::target_frequency_reset_value),
             static_cast<uint64_t>(27'000'000));

    app::hard_reset_clear_settings(s);

    /* This is exactly what core::pmem::target_frequency() does after a reset:
     * the key is absent, so the firmware reset value comes back. */
    CHECK_EQ(s.get_uint("pmem", "target_frequency",
                        core::pmem::target_frequency_reset_value),
             static_cast<uint64_t>(core::pmem::target_frequency_reset_value));
}

/* --- The .ini folder clear -------------------------------------------- */

TEST(hard_reset_counts_only_ini_files) {
    ScopedDir dir{"mb200_hardreset_count"};

    write_file(dir.path / "audio.ini", "x");
    write_file(dir.path / "recon.INI", "x");   /* extension match is case-insensitive */
    write_file(dir.path / "notes.txt", "x");   /* not an .ini */
    write_file(dir.path / "blacklist", "x");   /* no extension */
    std::error_code ec;
    std::filesystem::create_directories(dir.path / "sub.ini", ec);  /* a dir, not a file */

    CHECK_EQ(app::hard_reset_count_ini_files(dir.path), static_cast<uint16_t>(2));
}

TEST(hard_reset_deletes_only_ini_files) {
    ScopedDir dir{"mb200_hardreset_delete"};

    write_file(dir.path / "a.ini", "x");
    write_file(dir.path / "b.ini", "x");
    write_file(dir.path / "keep.txt", "x");

    CHECK_EQ(app::hard_reset_count_ini_files(dir.path), static_cast<uint16_t>(2));

    const uint16_t removed = app::hard_reset_delete_ini_files(dir.path);
    CHECK_EQ(removed, static_cast<uint16_t>(2));

    /* The .ini files are gone; the unrelated file is untouched. */
    CHECK_EQ(app::hard_reset_count_ini_files(dir.path), static_cast<uint16_t>(0));
    std::error_code ec;
    CHECK(std::filesystem::exists(dir.path / "keep.txt", ec));
    CHECK(!std::filesystem::exists(dir.path / "a.ini", ec));
}

TEST(hard_reset_handles_a_missing_directory) {
    /* A first run has no SETTINGS folder yet — this must be zero, not a throw. */
    const auto missing = temp_path("mb200_hardreset_no_such_dir_xyz");
    std::error_code ec;
    std::filesystem::remove_all(missing, ec);

    CHECK_EQ(app::hard_reset_count_ini_files(missing), static_cast<uint16_t>(0));
    CHECK_EQ(app::hard_reset_delete_ini_files(missing), static_cast<uint16_t>(0));
}
