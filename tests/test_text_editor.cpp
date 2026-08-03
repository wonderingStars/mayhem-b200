/*
 * mayhem-b200 — Notepad line-buffer / model tests.
 *
 * Exercises app::TextFileModel against a temporary directory the test owns.
 * Expected line boundaries come from the same rule core::read_lines documents
 * (split on '\n', a trailing '\r' stripped, no trailing empty line for a file
 * ending in '\n', no lines for an empty file), which mirrors upstream
 * file_reader.hpp / FileWrapper. The final-newline flag is the model's own
 * addition, needed so a round trip preserves the terminating newline that
 * read_lines alone discards.
 *
 * NB: the model variable is named `mdl`, not `m`: the CHECK_EQ macro declares
 * an internal `std::string m`, and a local `m` would shadow it under /W4.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "fs_utils.hpp"
#include "ui_text_editor.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

class TempDir {
   public:
    TempDir() {
        static int counter = 0;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        std::error_code ec;
        path_ = fs::temp_directory_path(ec) /
                ("mb200_notepad_test_" + std::to_string(stamp) + "_" +
                 std::to_string(counter++));
        fs::create_directories(path_, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    std::string sub(const std::string& leaf) const {
        return core::path_join(path_.generic_string(), leaf);
    }

   private:
    fs::path path_{};
};

/* Writes content, failing the test loudly on a setup error. */
std::string put(const TempDir& dir, const std::string& leaf, std::string_view content) {
    const auto path = dir.sub(leaf);
    const auto r = core::write_file(path, content);
    if (!r) ::mb200test::report_failure(__FILE__, __LINE__, "setup: " + r.message);
    return path;
}

}  // namespace

using app::TextFileModel;

TEST(notepad_index_trailing_newline) {
    TempDir dir;
    const auto path = put(dir, "a.txt", "alpha\nbeta\ngamma\n");

    TextFileModel mdl;
    CHECK(mdl.open(path));
    CHECK(mdl.is_open());
    CHECK(!mdl.in_memory());  // index mode, no text loaded

    CHECK_EQ(mdl.line_count(), (size_t)3);
    CHECK_STR_EQ(mdl.get_line(0), "alpha");
    CHECK_STR_EQ(mdl.get_line(1), "beta");
    CHECK_STR_EQ(mdl.get_line(2), "gamma");
    CHECK_EQ(mdl.line_length(1), (size_t)4);
    CHECK(mdl.has_final_newline());
    CHECK_EQ(mdl.total_size(), (uint64_t)17);

    // get_text slices from a column with a maximum width.
    CHECK_STR_EQ(mdl.get_text(0, 2, 2), "ph");
    CHECK_STR_EQ(mdl.get_text(0, 10, 4), "");  // past end of line
}

TEST(notepad_index_no_trailing_newline) {
    TempDir dir;
    const auto path = put(dir, "b.txt", "alpha\nbeta\ngamma");

    TextFileModel mdl;
    CHECK(mdl.open(path));
    CHECK_EQ(mdl.line_count(), (size_t)3);
    CHECK_STR_EQ(mdl.get_line(2), "gamma");
    CHECK(!mdl.has_final_newline());
    CHECK_EQ(mdl.total_size(), (uint64_t)16);
}

TEST(notepad_empty_file) {
    TempDir dir;
    const auto path = put(dir, "empty.txt", "");

    TextFileModel mdl;
    CHECK(mdl.open(path));
    CHECK(mdl.is_open());
    CHECK_EQ(mdl.line_count(), (size_t)0);
    CHECK(!mdl.has_final_newline());
    CHECK_EQ(mdl.total_size(), (uint64_t)0);

    // Matches core::read_lines on the same file.
    std::vector<std::string> lines;
    CHECK(core::read_lines(path, lines));
    CHECK_EQ(lines.size(), (size_t)0);
}

TEST(notepad_single_line_variants) {
    TempDir dir;

    TextFileModel a;
    CHECK(a.open(put(dir, "s1.txt", "hello")));
    CHECK_EQ(a.line_count(), (size_t)1);
    CHECK_STR_EQ(a.get_line(0), "hello");
    CHECK(!a.has_final_newline());
    CHECK_EQ(a.total_size(), (uint64_t)5);

    TextFileModel b;
    CHECK(b.open(put(dir, "s2.txt", "hello\n")));
    CHECK_EQ(b.line_count(), (size_t)1);
    CHECK_STR_EQ(b.get_line(0), "hello");
    CHECK(b.has_final_newline());
    CHECK_EQ(b.total_size(), (uint64_t)6);
}

TEST(notepad_crlf_stripped) {
    TempDir dir;
    const auto path = put(dir, "crlf.txt", "a\r\nbb\r\nccc\r\n");

    TextFileModel mdl;
    CHECK(mdl.open(path));
    CHECK_EQ(mdl.line_count(), (size_t)3);
    CHECK_STR_EQ(mdl.get_line(0), "a");
    CHECK_STR_EQ(mdl.get_line(1), "bb");
    CHECK_STR_EQ(mdl.get_line(2), "ccc");
    CHECK(mdl.has_final_newline());

    // Same lines core::read_lines yields.
    std::vector<std::string> lines;
    CHECK(core::read_lines(path, lines));
    CHECK_EQ(lines.size(), (size_t)3);
    CHECK_STR_EQ(lines[1], "bb");
}

TEST(notepad_blank_lines) {
    TempDir dir;

    TextFileModel a;
    CHECK(a.open(put(dir, "blanks.txt", "\n\n\n")));
    CHECK_EQ(a.line_count(), (size_t)3);
    CHECK_STR_EQ(a.get_line(0), "");
    CHECK_STR_EQ(a.get_line(2), "");
    CHECK(a.has_final_newline());

    TextFileModel b;
    CHECK(b.open(put(dir, "mid.txt", "x\n\ny\n")));
    CHECK_EQ(b.line_count(), (size_t)3);
    CHECK_STR_EQ(b.get_line(0), "x");
    CHECK_STR_EQ(b.get_line(1), "");
    CHECK_STR_EQ(b.get_line(2), "y");
}

TEST(notepad_open_missing_fails) {
    TempDir dir;
    TextFileModel mdl;
    CHECK(!mdl.open(dir.sub("does_not_exist.txt")));
    CHECK(!mdl.is_open());
}

TEST(notepad_insert_line_middle_and_end) {
    TempDir dir;
    const auto path = put(dir, "ins.txt", "a\nb\nc\n");

    TextFileModel mdl;
    CHECK(mdl.open(path));
    mdl.insert_line(1);  // empty line becomes line 1
    CHECK(mdl.in_memory());
    CHECK(mdl.dirty());
    CHECK_EQ(mdl.line_count(), (size_t)4);
    CHECK_STR_EQ(mdl.get_line(0), "a");
    CHECK_STR_EQ(mdl.get_line(1), "");
    CHECK_STR_EQ(mdl.get_line(2), "b");
    CHECK_STR_EQ(mdl.get_line(3), "c");

    // Insert past the end appends.
    mdl.insert_line(mdl.line_count());
    CHECK_EQ(mdl.line_count(), (size_t)5);
    CHECK_STR_EQ(mdl.get_line(4), "");
}

TEST(notepad_delete_line_to_empty) {
    TempDir dir;
    const auto path = put(dir, "del.txt", "a\nb\nc\n");

    TextFileModel mdl;
    CHECK(mdl.open(path));
    mdl.delete_line(1);
    CHECK_EQ(mdl.line_count(), (size_t)2);
    CHECK_STR_EQ(mdl.get_line(0), "a");
    CHECK_STR_EQ(mdl.get_line(1), "c");

    mdl.delete_line(0);
    mdl.delete_line(0);
    CHECK_EQ(mdl.line_count(), (size_t)0);

    // Deleting from an empty buffer is a no-op, not a crash.
    mdl.delete_line(0);
    CHECK_EQ(mdl.line_count(), (size_t)0);
}

TEST(notepad_set_and_append_line) {
    TempDir dir;
    const auto path = put(dir, "edit.txt", "one\ntwo\n");

    TextFileModel mdl;
    CHECK(mdl.open(path));
    mdl.set_line(0, "ONE!");
    CHECK_STR_EQ(mdl.get_line(0), "ONE!");
    CHECK_EQ(mdl.line_count(), (size_t)2);

    mdl.append_line("three");
    CHECK_EQ(mdl.line_count(), (size_t)3);
    CHECK_STR_EQ(mdl.get_line(2), "three");

    // Out-of-range set is ignored.
    mdl.set_line(99, "nope");
    CHECK_EQ(mdl.line_count(), (size_t)3);
}

TEST(notepad_materialize_equivalence) {
    TempDir dir;
    const auto path = put(dir, "mat.txt", "one\ntwo\nthree\nfour");

    TextFileModel mdl;
    CHECK(mdl.open(path));

    std::vector<std::string> from_index;
    for (size_t i = 0; i < mdl.line_count(); ++i)
        from_index.push_back(mdl.get_line(i));

    mdl.materialize();
    CHECK(mdl.in_memory());
    CHECK_EQ(mdl.line_count(), from_index.size());
    for (size_t i = 0; i < mdl.line_count(); ++i)
        CHECK_STR_EQ(mdl.get_line(i), from_index[i]);
}

TEST(notepad_serialize_matches_bytes) {
    TempDir dir;

    TextFileModel a;
    CHECK(a.open(put(dir, "ser1.txt", "a\nb\n")));
    CHECK_STR_EQ(a.serialize(), "a\nb\n");

    TextFileModel b;
    CHECK(b.open(put(dir, "ser2.txt", "a\nb")));
    CHECK_STR_EQ(b.serialize(), "a\nb");
}

TEST(notepad_roundtrip_preserves_trailing_newline) {
    TempDir dir;

    // With trailing newline.
    {
        const std::string content = "line1\nline2\nline3\n";
        const auto src = put(dir, "rt_a.txt", content);
        TextFileModel mdl;
        CHECK(mdl.open(src));
        const auto dst = dir.sub("rt_a_out.txt");
        CHECK(mdl.save(dst));

        std::string reloaded;
        CHECK(core::read_file(dst, reloaded));
        CHECK_STR_EQ(reloaded, content);  // byte-exact
    }

    // Without trailing newline.
    {
        const std::string content = "line1\nline2\nline3";
        const auto src = put(dir, "rt_b.txt", content);
        TextFileModel mdl;
        CHECK(mdl.open(src));
        const auto dst = dir.sub("rt_b_out.txt");
        CHECK(mdl.save(dst));

        std::string reloaded;
        CHECK(core::read_file(dst, reloaded));
        CHECK_STR_EQ(reloaded, content);  // byte-exact, no newline added
    }
}

TEST(notepad_save_then_reload_after_edits) {
    TempDir dir;
    const auto src = put(dir, "flow.txt", "one\ntwo\nthree\n");

    TextFileModel mdl;
    CHECK(mdl.open(src));
    mdl.set_line(1, "TWO");
    mdl.delete_line(0);
    mdl.append_line("four");
    // Buffer is now: ["TWO", "three", "four"], final newline preserved.

    const auto dst = dir.sub("flow_out.txt");
    CHECK(mdl.save(dst));
    CHECK(!mdl.dirty());  // save clears dirty

    // Reload with a second model and confirm the content.
    TextFileModel r;
    CHECK(r.open(dst));
    CHECK_EQ(r.line_count(), (size_t)3);
    CHECK_STR_EQ(r.get_line(0), "TWO");
    CHECK_STR_EQ(r.get_line(1), "three");
    CHECK_STR_EQ(r.get_line(2), "four");
    CHECK(r.has_final_newline());

    // And byte-exact through read_file.
    std::string reloaded;
    CHECK(core::read_file(dst, reloaded));
    CHECK_STR_EQ(reloaded, "TWO\nthree\nfour\n");
}

TEST(notepad_large_file_indexing) {
    TempDir dir;

    // Build a file bigger than the 64 KiB scan chunk (each line is padded to a
    // fixed 32 bytes) so the newline scan spans multiple chunks and on-demand
    // reads seek to lines on either side of the 65536-byte boundary.
    std::string content;
    const size_t n = 4000;               // 4000 * 32 = 128000 bytes > 65536
    for (size_t i = 0; i < n; ++i) {
        std::string line = "line" + std::to_string(i);
        line.resize(31, '.');            // pad to 31 chars of content
        content += line;
        content += '\n';                 // 32 bytes per line total
    }
    const auto path = put(dir, "big.txt", content);
    CHECK(content.size() > 65536u);
    CHECK_EQ(content.size(), (size_t)(n * 32));

    TextFileModel mdl;
    CHECK(mdl.open(path));
    CHECK(!mdl.in_memory());  // still index mode, text not loaded
    CHECK_EQ(mdl.line_count(), n);
    CHECK_STR_EQ(mdl.get_line(0), std::string("line0") + std::string(26, '.'));
    CHECK_STR_EQ(mdl.get_line(n - 1), std::string("line3999") + std::string(23, '.'));

    // The 65536-byte chunk boundary falls inside line 2048 (2048*32 = 65536),
    // so reading its neighbours exercises seeks on both sides of the boundary.
    CHECK_STR_EQ(mdl.get_line(2047), std::string("line2047") + std::string(23, '.'));
    CHECK_STR_EQ(mdl.get_line(2048), std::string("line2048") + std::string(23, '.'));
    CHECK(mdl.has_final_newline());
    CHECK_EQ(mdl.total_size(), (uint64_t)content.size());
}
