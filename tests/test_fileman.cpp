/*
 * mayhem-b200 — file manager tests.
 *
 * Two kinds of check:
 *
 *  1. The pure formatting helpers exported by ui_fileman.hpp (row/label/path
 *     text). Expected values come from the upstream layout it replaces
 *     (firmware/application/apps/ui_fileman.cpp: directory-first listing, the
 *     "name  size" rows, the parent ".." entry) and from to_string_file_size in
 *     string_format.cpp.
 *
 *  2. The fs_utils operations the view drives, exercised against a temporary
 *     directory the test owns — the create / list / rename / delete round trip a
 *     user performs, and the delete guard the view leans on (do_delete() calls
 *     core::may_delete() then core::delete_path() at the default
 *     DeleteScope::DataDirectoryOnly). The temp directory is deliberately
 *     *outside* core::data_directory(), so the guarded delete must be refused —
 *     the same property that keeps the file manager from deleting the user's
 *     files.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "file_path.hpp"
#include "fs_utils.hpp"
#include "ui_fileman.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

/* A uniquely named directory under the system temp dir, removed on scope exit. */
class TempDir {
   public:
    TempDir() {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::error_code ec;
        path_ = fs::temp_directory_path(ec) /
                ("mb200_fileman_test_" + std::to_string(stamp) + "_" +
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

void put_file(const std::string& path, std::string_view content) {
    const auto r = core::write_file(path, content);
    if (!r) ::mb200test::report_failure(__FILE__, __LINE__, "setup: " + r.message);
}

std::string joined(const std::vector<core::DirEntry>& entries) {
    std::string s;
    for (const auto& e : entries) {
        if (!s.empty()) s += ",";
        s += e.name;
    }
    return s;
}

}  // namespace

/* --- Size / count label ---------------------------------------------------- */

TEST(fileman_size_label_matches_upstream_columns) {
    /* Parent entry shows nothing, as upstream's ".." row does. */
    CHECK_STR_EQ(app::fileman_size_label(false, true, 999, 5), "");

    /* Directories show their entry count. */
    CHECK_STR_EQ(app::fileman_size_label(true, false, 0, 12), "12");
    CHECK_STR_EQ(app::fileman_size_label(true, false, 0, 0), "0");
    CHECK_STR_EQ(app::fileman_size_label(true, false, 0, -3), "0");  /* clamped */

    /* Files use to_string_file_size (divide-by-1024, truncating). */
    CHECK_STR_EQ(app::fileman_size_label(false, false, 0, 0), "0B");
    CHECK_STR_EQ(app::fileman_size_label(false, false, 5, 0), "5B");
    CHECK_STR_EQ(app::fileman_size_label(false, false, 1023, 0), "1023B");
    CHECK_STR_EQ(app::fileman_size_label(false, false, 1024, 0), "1kB");
    CHECK_STR_EQ(app::fileman_size_label(false, false, 1536, 0), "1kB");
    CHECK_STR_EQ(app::fileman_size_label(false, false, 2048, 0), "2kB");
    CHECK_STR_EQ(app::fileman_size_label(false, false, 1048576, 0), "1MB");
}

/* --- Path display relative to the browse root ------------------------------ */

TEST(fileman_relative_display_is_rooted_and_case_insensitive) {
    CHECK_STR_EQ(app::fileman_relative_display("/data", "/data"), "/");
    /* A trailing separator on either side is ignored. */
    CHECK_STR_EQ(app::fileman_relative_display("/data/", "/data"), "/");

    CHECK_STR_EQ(app::fileman_relative_display("/data/CAPTURES", "/data"), "/CAPTURES");
    CHECK_STR_EQ(app::fileman_relative_display("/data/A/B", "/data"), "/A/B");

    /* Backslashes are normalised to '/'. */
    CHECK_STR_EQ(app::fileman_relative_display("C:\\x\\data\\Sub", "C:\\x\\data"), "/Sub");

    /* The comparison is case-insensitive (ASCII), matching path_iequal(). */
    CHECK_STR_EQ(app::fileman_relative_display("C:/X/DATA/Sub", "c:/x/data"), "/Sub");

    /* A sibling that merely shares a prefix is not "under" root. */
    CHECK_STR_EQ(app::fileman_relative_display("/database", "/data"), "database");

    /* Anything genuinely outside root falls back to its leaf name. */
    CHECK_STR_EQ(app::fileman_relative_display("/other/thing", "/data"), "thing");

    /* An empty root means show the path as-is (normalised). */
    CHECK_STR_EQ(app::fileman_relative_display("/any/where", ""), "/any/where");
}

/* --- Left-truncation of an over-long line ---------------------------------- */

TEST(fileman_fit_left_keeps_the_tail_and_marks_the_cut) {
    /* Short enough: unchanged. */
    CHECK_STR_EQ(app::fileman_fit_left("abc", 5), "abc");
    CHECK_STR_EQ(app::fileman_fit_left("abcde", 5), "abcde");

    /* Too long: drop the head, keep max-1 tail chars, prefix '<'. */
    CHECK_STR_EQ(app::fileman_fit_left("abcdef", 5), "<cdef");
    CHECK_STR_EQ(app::fileman_fit_left("/very/long/path/name", 8), "<th/name");

    /* Degenerate widths. */
    CHECK_STR_EQ(app::fileman_fit_left("abcdef", 1), "<");
    CHECK_STR_EQ(app::fileman_fit_left("x", 0), "");
}

/* --- Full menu row --------------------------------------------------------- */

TEST(fileman_row_text_left_justifies_name_and_right_justifies_size) {
    /* File: name left, size right, exactly `columns` wide. */
    const auto file_row = app::fileman_row_text("a.txt", false, false, "5B", 12);
    CHECK_STR_EQ(file_row, "a.txt     5B");
    CHECK_EQ(file_row.size(), size_t{12});

    /* Directory: a '/' is appended to the name, count on the right. */
    const auto dir_row = app::fileman_row_text("dir", true, false, "3", 12);
    CHECK_STR_EQ(dir_row, "dir/       3");
    CHECK_EQ(dir_row.size(), size_t{12});

    /* Parent: ".." with no size, no padding. */
    CHECK_STR_EQ(app::fileman_row_text("..", false, true, "", 12), "..");

    /* A name too wide for the row is truncated (head kept) so the size still
     * fits with a one-column gap. */
    const auto long_row =
        app::fileman_row_text("averylongfilename.dat", false, false, "9kB", 12);
    CHECK_STR_EQ(long_row, "averylon 9kB");
    CHECK_EQ(long_row.size(), size_t{12});

    /* No right label, name fits: returned as-is. */
    CHECK_STR_EQ(app::fileman_row_text("f", false, false, "", 12), "f");

    /* Zero columns yields nothing. */
    CHECK_STR_EQ(app::fileman_row_text("f", false, false, "5B", 0), "");
}

/* --- fs_utils round trip the view drives ----------------------------------- */

TEST(fileman_create_list_rename_delete_round_trip) {
    TempDir tmp;

    /* New folder + new file, as the NewDir / NewFile buttons do. */
    CHECK(core::create_directory(tmp.sub("sub")));
    CHECK(core::create_file(tmp.sub("empty.bin")));
    put_file(tmp.sub("a.TXT"), "a");     /* 1 byte  */
    put_file(tmp.sub("b.txt"), "bb");    /* 2 bytes */

    /* The listing the view shows: directories first, then names, with sizes. */
    std::vector<core::DirEntry> entries;
    CHECK(core::list_directory(tmp.str(), entries));
    CHECK_STR_EQ(joined(entries), "sub,a.TXT,b.txt,empty.bin");
    CHECK(entries[0].is_directory);
    CHECK(!entries[1].is_directory);
    CHECK_EQ(entries[1].size, std::uintmax_t{1});  /* a.TXT */
    CHECK_EQ(entries[2].size, std::uintmax_t{2});  /* b.txt */

    /* The row text for the first file, as it would appear in the menu. */
    const auto label = app::fileman_size_label(entries[1].is_directory, false,
                                               entries[1].size, 0);
    CHECK_STR_EQ(app::fileman_row_text(entries[1].name, false, false, label, 28),
                 "a.TXT                     1B");

    /* Rename, as the Rename button does. */
    CHECK(core::rename_path(tmp.sub("a.TXT"), tmp.sub("renamed.txt")));
    CHECK(!core::exists(tmp.sub("a.TXT")));
    CHECK(core::exists(tmp.sub("renamed.txt")));

    /* Delete a file and the (now still-empty) folder. These are outside the data
     * directory, so the test must opt in with DeleteScope::Anywhere — the view
     * only ever deletes inside the data dir at the default scope. */
    CHECK(core::delete_path(tmp.sub("empty.bin"), core::DeleteScope::Anywhere));
    CHECK(!core::exists(tmp.sub("empty.bin")));
    CHECK(core::delete_path(tmp.sub("sub"), core::DeleteScope::Anywhere));
    CHECK(!core::exists(tmp.sub("sub")));
}

/* --- The delete guard the view relies on ----------------------------------- */

TEST(fileman_delete_of_a_protected_path_is_refused) {
    TempDir tmp;
    put_file(tmp.sub("victim.txt"), "still here");

    /* Precondition: the temp dir must be outside the data directory, or this
     * proves nothing. */
    CHECK(!core::is_within_data_directory(tmp.str()));

    /* do_delete() runs may_delete() first, then delete_path() at the default
     * scope; both must refuse a path outside the data directory. */
    const auto pre = core::may_delete(tmp.sub("victim.txt"));
    CHECK(!pre);
    CHECK(pre.message.find("data directory") != std::string::npos);

    const auto refused = core::delete_path(tmp.sub("victim.txt"));
    CHECK(!refused);
    CHECK(refused.message.find("data directory") != std::string::npos);
    CHECK(core::exists(tmp.sub("victim.txt")));  /* untouched */

    /* The data directory itself is refused even with the opt-in. */
    CHECK(!core::may_delete(core::data_directory(), core::DeleteScope::Anywhere));

    /* A path inside the data directory is permitted, so the guard is refusing
     * for its stated reason, not refusing everything. */
    CHECK(core::may_delete(core::path_join(core::captures_directory(), "x.c8")));
}
