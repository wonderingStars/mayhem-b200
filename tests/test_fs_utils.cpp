/*
 * mayhem-b200 — filesystem helper tests.
 *
 * Everything here runs against a temporary directory the test creates and
 * removes itself. That directory is deliberately *outside* core::data_directory(),
 * which is what makes the delete guard testable: every deliberate delete in
 * these tests has to opt in with DeleteScope::Anywhere, and the one that does
 * not is expected to be refused.
 *
 * Expected values come from upstream where upstream has an opinion —
 * directories-first ordering and the "_1", "_2" unique-filename suffixes are
 * from firmware/application/apps/ui_fileman.cpp (insert_sorted,
 * get_unique_filename), case-insensitive extension matching from
 * path_iequal() in firmware/application/file.cpp, and the line splitting from
 * file_reader.hpp's BufferLineReader.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "file_path.hpp"
#include "fs_utils.hpp"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

/* Creates a uniquely named directory under the system temp directory and
 * removes it, and everything in it, when it goes out of scope. */
class TempDir {
   public:
    TempDir() {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();

        std::error_code ec;
        path_ = fs::temp_directory_path(ec) /
                ("mb200_fs_test_" + std::to_string(stamp) + "_" +
                 std::to_string(counter++));

        fs::create_directories(path_, ec);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    /* Generic form ('/' separators) to match what core's path helpers return. */
    std::string str() const { return path_.generic_string(); }
    std::string sub(const std::string& leaf) const {
        return core::path_join(str(), leaf);
    }

   private:
    fs::path path_{};
};

/* Setup helper: fails loudly rather than leaving a test to fail obscurely. */
void put_file(const std::string& path, std::string_view content) {
    const auto r = core::write_file(path, content);
    if (!r) ::mb200test::report_failure(__FILE__, __LINE__, "setup: " + r.message);
}

void put_dir(const std::string& path) {
    const auto r = core::create_directory(path);
    if (!r) ::mb200test::report_failure(__FILE__, __LINE__, "setup: " + r.message);
}

std::vector<std::string> names_of(const std::vector<core::DirEntry>& entries) {
    std::vector<std::string> names;
    for (const auto& e : entries) names.push_back(e.name);
    return names;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) {
        if (!s.empty()) s += ",";
        s += x;
    }
    return s;
}

}  // namespace

/* --- Listing -------------------------------------------------------------- */

TEST(list_directory_orders_directories_first_then_names) {
    TempDir tmp;
    put_dir(tmp.sub("zeta"));
    put_dir(tmp.sub("Alpha"));
    put_file(tmp.sub("b.txt"), "bb");
    put_file(tmp.sub("A.TXT"), "a");
    put_file(tmp.sub("c.dat"), "ccc");

    std::vector<core::DirEntry> entries;
    const auto r = core::list_directory(tmp.str(), entries);
    CHECK(r);

    /* Upstream's insert_sorted(): directories before files, then by name. */
    CHECK_STR_EQ(joined(names_of(entries)), "Alpha,zeta,A.TXT,b.txt,c.dat");

    CHECK(entries[0].is_directory);
    CHECK(entries[1].is_directory);
    CHECK(!entries[2].is_directory);

    /* Directories report size 0, as fileman_entry does upstream. */
    CHECK_EQ(entries[0].size, std::uintmax_t{0});
    CHECK_EQ(entries[2].size, std::uintmax_t{1});  /* "a" */
    CHECK_EQ(entries[3].size, std::uintmax_t{2});  /* "bb" */
    CHECK_EQ(entries[4].size, std::uintmax_t{3});  /* "ccc" */
}

TEST(list_directory_modified_time_is_a_unix_timestamp) {
    /* A file written a moment ago must date from a moment ago. This is the
     * check that catches a file_clock epoch (1601 on Windows) leaking through
     * unconverted, which would be off by ~11644473600 seconds. */
    TempDir tmp;
    put_file(tmp.sub("now.txt"), "x");

    std::vector<core::DirEntry> entries;
    CHECK(core::list_directory(tmp.str(), entries));
    CHECK_EQ(entries.size(), size_t{1});

    const long long delta = static_cast<long long>(entries[0].modified) -
                            static_cast<long long>(std::time(nullptr));
    CHECK(delta > -120 && delta < 120);
}

TEST(list_directory_extension_filter_is_case_insensitive) {
    TempDir tmp;
    put_dir(tmp.sub("sub"));
    put_file(tmp.sub("a.TXT"), "1");
    put_file(tmp.sub("b.txt"), "2");
    put_file(tmp.sub("c.dat"), "3");
    put_file(tmp.sub("noext"), "4");

    std::vector<core::DirEntry> entries;

    /* Directories are never filtered out — the file manager has to navigate. */
    CHECK(core::list_directory(tmp.str(), entries, ".txt"));
    CHECK_STR_EQ(joined(names_of(entries)), "sub,a.TXT,b.txt");

    /* The leading dot is optional. */
    CHECK(core::list_directory(tmp.str(), entries, "TXT"));
    CHECK_STR_EQ(joined(names_of(entries)), "sub,a.TXT,b.txt");

    /* Several extensions, the way a capture browser wants .C8/.CU8/.C16. */
    core::ListOptions many;
    many.extensions = {".txt", ".dat"};
    CHECK(core::list_directory(tmp.str(), entries, many));
    CHECK_STR_EQ(joined(names_of(entries)), "sub,a.TXT,b.txt,c.dat");

    /* A filter that matches nothing leaves only the directory. */
    CHECK(core::list_directory(tmp.str(), entries, ".png"));
    CHECK_STR_EQ(joined(names_of(entries)), "sub");

    /* An empty filter means "everything", as upstream's empty
     * extension_filter does. */
    CHECK(core::list_directory(tmp.str(), entries, ""));
    CHECK_STR_EQ(joined(names_of(entries)), "sub,a.TXT,b.txt,c.dat,noext");
}

TEST(list_directory_hidden_and_directory_options) {
    TempDir tmp;
    put_dir(tmp.sub("sub"));
    put_file(tmp.sub("visible.txt"), "v");
    put_file(tmp.sub(".hidden"), "h");

    std::vector<core::DirEntry> entries;

    CHECK(core::list_directory(tmp.str(), entries));
    CHECK_STR_EQ(joined(names_of(entries)), "sub,visible.txt");

    core::ListOptions with_hidden;
    with_hidden.include_hidden = true;
    CHECK(core::list_directory(tmp.str(), entries, with_hidden));
    CHECK_STR_EQ(joined(names_of(entries)), "sub,.hidden,visible.txt");

    core::ListOptions files_only;
    files_only.include_directories = false;
    CHECK(core::list_directory(tmp.str(), entries, files_only));
    CHECK_STR_EQ(joined(names_of(entries)), "visible.txt");
}

TEST(list_directory_reports_errors) {
    TempDir tmp;

    std::vector<core::DirEntry> entries;
    entries.push_back(core::DirEntry{"stale", false, 0, 0});

    const auto missing = core::list_directory(tmp.sub("no_such_dir"), entries);
    CHECK(!missing);
    CHECK(!missing.message.empty());
    /* The out vector is cleared even when the listing fails, so a caller
     * cannot redraw last time's contents. */
    CHECK(entries.empty());

    /* A file is not a directory. */
    put_file(tmp.sub("a.txt"), "x");
    const auto not_a_dir = core::list_directory(tmp.sub("a.txt"), entries);
    CHECK(!not_a_dir);
}

TEST(list_directory_of_empty_directory_succeeds_with_no_entries) {
    TempDir tmp;
    std::vector<core::DirEntry> entries;
    CHECK(core::list_directory(tmp.str(), entries));
    CHECK(entries.empty());
    CHECK(core::is_empty_directory(tmp.str()));
    CHECK_EQ(core::file_count(tmp.str()), 0);
}

/* --- Unique filenames ------------------------------------------------------ */

TEST(unique_filename_appends_counter_when_target_exists) {
    TempDir tmp;

    /* Nothing there: the name is returned unchanged. */
    CHECK_STR_EQ(core::unique_filename(tmp.str(), "a.txt"), "a.txt");

    /* Upstream get_unique_filename(): stem + "_" + serial + extension. */
    put_file(tmp.sub("a.txt"), "1");
    CHECK_STR_EQ(core::unique_filename(tmp.str(), "a.txt"), "a_1.txt");

    put_file(tmp.sub("a_1.txt"), "2");
    CHECK_STR_EQ(core::unique_filename(tmp.str(), "a.txt"), "a_2.txt");

    /* A name with no extension keeps having none. */
    put_file(tmp.sub("log"), "3");
    CHECK_STR_EQ(core::unique_filename(tmp.str(), "log"), "log_1");

    /* A directory of that name counts as taken, too. */
    put_dir(tmp.sub("data"));
    CHECK_STR_EQ(core::unique_filename(tmp.str(), "data"), "data_1");

    /* Only the leaf name comes back, never a path. */
    CHECK_STR_EQ(core::unique_filename(tmp.str(), "b.txt"), "b.txt");

    CHECK_STR_EQ(core::unique_filename(tmp.str(), ""), "");
}

/* --- Copy / move ----------------------------------------------------------- */

TEST(copy_preserves_content_and_leaves_the_source) {
    TempDir tmp;
    const std::string content = "freq=433920000\nmod=AM\n";
    put_file(tmp.sub("src.txt"), content);

    CHECK(core::copy_path(tmp.sub("src.txt"), tmp.sub("dst.txt")));

    std::string read_back;
    CHECK(core::read_file(tmp.sub("dst.txt"), read_back));
    CHECK_STR_EQ(read_back, content);
    CHECK(core::exists(tmp.sub("src.txt")));

    /* Refuses to clobber unless asked. */
    const auto clobber = core::copy_path(tmp.sub("src.txt"), tmp.sub("dst.txt"));
    CHECK(!clobber);
    CHECK(!clobber.message.empty());

    put_file(tmp.sub("other.txt"), "replacement");
    CHECK(core::copy_path(tmp.sub("other.txt"), tmp.sub("dst.txt"), /*overwrite*/ true));
    CHECK(core::read_file(tmp.sub("dst.txt"), read_back));
    CHECK_STR_EQ(read_back, "replacement");

    /* Missing source is an error, not a silent no-op. */
    const auto missing = core::copy_path(tmp.sub("nope.txt"), tmp.sub("x.txt"));
    CHECK(!missing);
    CHECK(!core::exists(tmp.sub("x.txt")));
}

TEST(copy_of_a_directory_is_recursive) {
    TempDir tmp;
    put_dir(tmp.sub("tree"));
    put_dir(tmp.sub("tree/inner"));
    put_file(tmp.sub("tree/top.txt"), "top");
    put_file(tmp.sub("tree/inner/deep.txt"), "deep");

    CHECK(core::copy_path(tmp.sub("tree"), tmp.sub("tree_copy")));

    std::string content;
    CHECK(core::read_file(tmp.sub("tree_copy/top.txt"), content));
    CHECK_STR_EQ(content, "top");
    CHECK(core::read_file(tmp.sub("tree_copy/inner/deep.txt"), content));
    CHECK_STR_EQ(content, "deep");

    /* Copying a directory inside itself would recurse forever. */
    const auto into_self = core::copy_path(tmp.sub("tree"), tmp.sub("tree/nested"));
    CHECK(!into_self);
    CHECK(!into_self.message.empty());
}

TEST(move_preserves_content_and_removes_the_source) {
    TempDir tmp;
    const std::string content = "no trailing newline";
    put_file(tmp.sub("src.txt"), content);
    put_dir(tmp.sub("dest"));

    CHECK(core::move_path(tmp.sub("src.txt"), tmp.sub("dest/moved.txt")));

    std::string read_back;
    CHECK(core::read_file(tmp.sub("dest/moved.txt"), read_back));
    CHECK_STR_EQ(read_back, content);
    CHECK(!core::exists(tmp.sub("src.txt")));

    /* rename_path is the same operation and must not clobber. */
    put_file(tmp.sub("a.txt"), "A");
    put_file(tmp.sub("b.txt"), "B");
    const auto clash = core::rename_path(tmp.sub("a.txt"), tmp.sub("b.txt"));
    CHECK(!clash);
    CHECK(core::exists(tmp.sub("a.txt")));
    CHECK(core::read_file(tmp.sub("b.txt"), read_back));
    CHECK_STR_EQ(read_back, "B");

    CHECK(core::rename_path(tmp.sub("a.txt"), tmp.sub("renamed.txt")));
    CHECK(!core::exists(tmp.sub("a.txt")));
    CHECK(core::read_file(tmp.sub("renamed.txt"), read_back));
    CHECK_STR_EQ(read_back, "A");

    const auto missing = core::move_path(tmp.sub("gone.txt"), tmp.sub("z.txt"));
    CHECK(!missing);
}

/* --- Create --------------------------------------------------------------- */

TEST(create_file_refuses_to_truncate_an_existing_file) {
    TempDir tmp;

    CHECK(core::create_file(tmp.sub("new.txt")));
    CHECK(core::exists(tmp.sub("new.txt")));
    CHECK_EQ(core::file_size(tmp.sub("new.txt")), std::uintmax_t{0});

    put_file(tmp.sub("new.txt"), "precious");

    /* Deliberate deviation from upstream's FA_CREATE_ALWAYS: the content
     * survives and the caller is told why. */
    const auto again = core::create_file(tmp.sub("new.txt"));
    CHECK(!again);
    CHECK(!again.message.empty());

    std::string content;
    CHECK(core::read_file(tmp.sub("new.txt"), content));
    CHECK_STR_EQ(content, "precious");
}

TEST(create_directory_reports_existing_and_missing_parents) {
    TempDir tmp;

    CHECK(core::create_directory(tmp.sub("d")));
    CHECK(core::is_directory(tmp.sub("d")));

    CHECK(!core::create_directory(tmp.sub("d")));
    /* One level only; the recursive form is core::ensure_directory(). */
    CHECK(!core::create_directory(tmp.sub("missing/child")));
    CHECK(core::ensure_directory(tmp.sub("missing/child")));
    CHECK(core::is_directory(tmp.sub("missing/child")));
}

/* --- Delete ---------------------------------------------------------------- */

TEST(delete_of_a_missing_path_is_an_error) {
    TempDir tmp;

    const auto one = core::delete_path(tmp.sub("nope.txt"), core::DeleteScope::Anywhere);
    CHECK(!one);
    CHECK(!one.message.empty());

    const auto rec = core::delete_recursive(tmp.sub("nope_dir"),
                                            core::DeleteScope::Anywhere);
    CHECK(!rec);
    CHECK(!rec.message.empty());

    const auto empty_path = core::delete_path("", core::DeleteScope::Anywhere);
    CHECK(!empty_path);
}

TEST(delete_path_removes_files_and_empty_directories_only) {
    TempDir tmp;
    put_file(tmp.sub("a.txt"), "x");
    put_dir(tmp.sub("empty"));
    put_dir(tmp.sub("full"));
    put_file(tmp.sub("full/inner.txt"), "y");

    CHECK(core::delete_path(tmp.sub("a.txt"), core::DeleteScope::Anywhere));
    CHECK(!core::exists(tmp.sub("a.txt")));

    CHECK(core::delete_path(tmp.sub("empty"), core::DeleteScope::Anywhere));
    CHECK(!core::exists(tmp.sub("empty")));

    /* Upstream's fileman refuses this too, and tells you to use Clean. */
    const auto non_empty = core::delete_path(tmp.sub("full"), core::DeleteScope::Anywhere);
    CHECK(!non_empty);
    CHECK(core::exists(tmp.sub("full/inner.txt")));

    CHECK(core::delete_recursive(tmp.sub("full"), core::DeleteScope::Anywhere));
    CHECK(!core::exists(tmp.sub("full")));
}

/* --- The guard ------------------------------------------------------------- */

TEST(delete_guard_rejects_paths_outside_the_data_directory) {
    TempDir tmp;
    put_file(tmp.sub("victim.txt"), "still here");

    /* Precondition: if the system temp directory ever ends up inside the data
     * directory this test proves nothing, so say so rather than fail oddly. */
    CHECK(!core::is_within_data_directory(tmp.str()));

    const auto refused = core::delete_path(tmp.sub("victim.txt"));
    CHECK(!refused);
    CHECK(refused.message.find("data directory") != std::string::npos);
    CHECK(core::exists(tmp.sub("victim.txt")));

    put_dir(tmp.sub("tree"));
    put_file(tmp.sub("tree/inner.txt"), "also here");
    const auto refused_rec = core::delete_recursive(tmp.sub("tree"));
    CHECK(!refused_rec);
    CHECK(core::exists(tmp.sub("tree/inner.txt")));

    /* The same delete goes through once the caller opts in. */
    CHECK(core::delete_path(tmp.sub("victim.txt"), core::DeleteScope::Anywhere));
    CHECK(!core::exists(tmp.sub("victim.txt")));
}

TEST(delete_guard_refuses_the_data_directory_itself_in_both_scopes) {
    /* These cases are checked through may_delete(), which cannot delete
     * anything, and never through delete_recursive(). A test that asks a real
     * delete routine to consider C:\ is one implementation bug away from being
     * a catastrophe, and this suite is run against deliberately broken builds. */
    const auto data = core::data_directory();

    const auto guarded = core::may_delete(data);
    CHECK(!guarded);
    CHECK(guarded.message.find("data directory") != std::string::npos);

    const auto opted_in = core::may_delete(data, core::DeleteScope::Anywhere);
    CHECK(!opted_in);
    CHECK(opted_in.message.find("data directory") != std::string::npos);

    /* A filesystem root is refused even with the opt-in. */
    const auto root = fs::path{core::data_directory()}.root_path().string();
    CHECK(!root.empty());
    const auto root_delete = core::may_delete(root, core::DeleteScope::Anywhere);
    CHECK(!root_delete);
    CHECK(root_delete.message.find("root") != std::string::npos);

    /* Something inside the data directory is allowed, so the checks above are
     * refusing for their stated reason and not just refusing everything. */
    CHECK(core::may_delete(core::path_join(core::captures_directory(), "x.c8")));
}

TEST(is_within_directory_normalises_dot_dot_and_is_case_insensitive_on_windows) {
    TempDir tmp;
    const std::string root = tmp.str();

    CHECK(core::is_within_directory(root, root));
    CHECK(core::is_within_directory(core::path_join(root, "a/b.txt"), root));

    /* A ".." escape must not sneak past. */
    CHECK(!core::is_within_directory(core::path_join(root, "../elsewhere.txt"), root));
    CHECK(core::is_within_directory(core::path_join(root, "a/../b.txt"), root));

    /* A sibling whose name merely starts with the root's name is outside. */
    CHECK(!core::is_within_directory(root + "_sibling/x.txt", root));

    CHECK(!core::is_within_directory("", root));
    CHECK(!core::is_within_directory(root, ""));

    /* Paths under the data directory are recognised even when they do not
     * exist — the fileman asks before creating things. */
    CHECK(core::is_within_data_directory(
        core::path_join(core::captures_directory(), "does_not_exist.c8")));

#if defined(_WIN32)
    std::string shouty = root;
    for (auto& c : shouty) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    CHECK(core::is_within_directory(core::path_join(shouty, "a.txt"), root));
#endif
}

/* --- Path helpers ---------------------------------------------------------- */

TEST(path_helpers) {
    CHECK_STR_EQ(core::path_join("a", "b"), "a/b");
    CHECK_STR_EQ(core::path_join("a/", "b"), "a/b");
    CHECK_STR_EQ(core::path_join("a/b", "c.txt"), "a/b/c.txt");
    CHECK_STR_EQ(core::path_join("", "b"), "b");
    CHECK_STR_EQ(core::path_join("a", ""), "a");

    CHECK_STR_EQ(core::parent_path("a/b/c.txt"), "a/b");
    CHECK_STR_EQ(core::parent_path("c.txt"), "");

    CHECK_STR_EQ(core::filename("a/b/c.txt"), "c.txt");
    CHECK_STR_EQ(core::filename("c.txt"), "c.txt");

    CHECK_STR_EQ(core::stem("a/b/LOG_001.TXT"), "LOG_001");
    CHECK_STR_EQ(core::stem("a/b/noext"), "noext");
    CHECK_STR_EQ(core::stem("a/b/c.tar.gz"), "c.tar");

    CHECK_STR_EQ(core::extension("a/b/LOG_001.TXT"), ".TXT");
    CHECK_STR_EQ(core::extension("a/b/noext"), "");
    CHECK_STR_EQ(core::extension("a/b/c.tar.gz"), ".gz");

    /* Partner-file lookups in the fileman swap .TXT for .C8 and back. */
    CHECK_STR_EQ(core::replace_extension("a/b.TXT", ".C8"), "a/b.C8");
    CHECK_STR_EQ(core::replace_extension("a/b.TXT", "C8"), "a/b.C8");
    CHECK_STR_EQ(core::replace_extension("a/b.TXT", ""), "a/b");
    CHECK_STR_EQ(core::replace_extension("a/b", ".C8"), "a/b.C8");
}

/* --- Text I/O -------------------------------------------------------------- */

TEST(read_write_round_trip_is_byte_exact) {
    TempDir tmp;
    const std::string content = "line one\nline two\nno trailing newline";

    CHECK(core::write_file(tmp.sub("t.txt"), content));

    std::string read_back;
    CHECK(core::read_file(tmp.sub("t.txt"), read_back));
    CHECK_STR_EQ(read_back, content);
    CHECK_EQ(core::file_size(tmp.sub("t.txt")), std::uintmax_t{content.size()});

    /* Writing again truncates rather than appending. */
    CHECK(core::write_file(tmp.sub("t.txt"), "short"));
    CHECK(core::read_file(tmp.sub("t.txt"), read_back));
    CHECK_STR_EQ(read_back, "short");

    CHECK(core::write_file(tmp.sub("empty.txt"), ""));
    CHECK(core::read_file(tmp.sub("empty.txt"), read_back));
    CHECK_STR_EQ(read_back, "");

    /* A missing file is an error and leaves the output empty. */
    read_back = "stale";
    const auto missing = core::read_file(tmp.sub("gone.txt"), read_back);
    CHECK(!missing);
    CHECK(!missing.message.empty());
    CHECK_STR_EQ(read_back, "");

    /* A directory is not a text file. */
    put_dir(tmp.sub("d"));
    CHECK(!core::read_file(tmp.sub("d"), read_back));
}

TEST(read_lines_without_trailing_newline_yields_the_final_line) {
    TempDir tmp;
    put_file(tmp.sub("a.txt"), "first\nsecond\nthird");

    std::vector<std::string> lines;
    CHECK(core::read_lines(tmp.sub("a.txt"), lines));
    CHECK_EQ(lines.size(), size_t{3});
    CHECK_STR_EQ(lines[0], "first");
    CHECK_STR_EQ(lines[1], "second");
    CHECK_STR_EQ(lines[2], "third");
}

TEST(read_lines_terminator_handling) {
    TempDir tmp;
    std::vector<std::string> lines;

    /* A trailing newline does not add an empty last line. */
    put_file(tmp.sub("lf.txt"), "a\nb\n");
    CHECK(core::read_lines(tmp.sub("lf.txt"), lines));
    CHECK_EQ(lines.size(), size_t{2});
    CHECK_STR_EQ(lines[1], "b");

    /* CRLF reads the same as LF: no stray '\r' at the end of a line. */
    put_file(tmp.sub("crlf.txt"), "a\r\nb\r\nc");
    CHECK(core::read_lines(tmp.sub("crlf.txt"), lines));
    CHECK_EQ(lines.size(), size_t{3});
    CHECK_STR_EQ(lines[0], "a");
    CHECK_STR_EQ(lines[1], "b");
    CHECK_STR_EQ(lines[2], "c");

    /* Blank lines in the middle are preserved — freqman files have them. */
    put_file(tmp.sub("blank.txt"), "a\n\nb");
    CHECK(core::read_lines(tmp.sub("blank.txt"), lines));
    CHECK_EQ(lines.size(), size_t{3});
    CHECK_STR_EQ(lines[1], "");

    /* A lone newline is one empty line, not two. */
    put_file(tmp.sub("nl.txt"), "\n");
    CHECK(core::read_lines(tmp.sub("nl.txt"), lines));
    CHECK_EQ(lines.size(), size_t{1});
    CHECK_STR_EQ(lines[0], "");

    /* An empty file has no lines at all. */
    put_file(tmp.sub("empty.txt"), "");
    CHECK(core::read_lines(tmp.sub("empty.txt"), lines));
    CHECK(lines.empty());

    /* Long lines are not truncated by any internal buffer. */
    const std::string long_line(5000, 'x');
    put_file(tmp.sub("long.txt"), long_line + "\nshort");
    CHECK(core::read_lines(tmp.sub("long.txt"), lines));
    CHECK_EQ(lines.size(), size_t{2});
    CHECK_EQ(lines[0].size(), size_t{5000});
    CHECK_STR_EQ(lines[1], "short");

    const auto missing = core::read_lines(tmp.sub("gone.txt"), lines);
    CHECK(!missing);
    CHECK(lines.empty());
}

TEST(for_each_line_can_stop_early) {
    TempDir tmp;
    put_file(tmp.sub("a.txt"), "one\ntwo\nthree\nfour");

    int seen = 0;
    std::string second;
    const auto r = core::for_each_line(tmp.sub("a.txt"), [&](std::string_view line) {
        ++seen;
        if (seen == 2) second = std::string{line};
        return seen < 2;  /* stop after the second line */
    });

    /* Stopping early is not a failure. */
    CHECK(r);
    CHECK_EQ(seen, 2);
    CHECK_STR_EQ(second, "two");
}

/* --- Queries --------------------------------------------------------------- */

TEST(query_helpers) {
    TempDir tmp;
    put_file(tmp.sub("a.txt"), "12345");
    put_dir(tmp.sub("d"));

    CHECK(core::exists(tmp.sub("a.txt")));
    CHECK(!core::exists(tmp.sub("nope")));
    CHECK(!core::exists(""));

    CHECK(core::is_regular_file(tmp.sub("a.txt")));
    CHECK(!core::is_regular_file(tmp.sub("d")));
    CHECK(core::is_directory(tmp.sub("d")));
    CHECK(!core::is_directory(tmp.sub("a.txt")));

    CHECK(core::is_empty_directory(tmp.sub("d")));
    CHECK(!core::is_empty_directory(tmp.sub("a.txt")));
    CHECK(!core::is_empty_directory(tmp.sub("nope")));

    CHECK_EQ(core::file_size(tmp.sub("a.txt")), std::uintmax_t{5});
    CHECK_EQ(core::file_size(tmp.sub("nope")), std::uintmax_t{0});

    CHECK_EQ(core::file_count(tmp.str()), 2);
    /* -1, not 0, so "unreadable" is distinguishable from "empty". */
    CHECK_EQ(core::file_count(tmp.sub("nope")), -1);
}
