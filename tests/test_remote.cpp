/*
 * mayhem-b200 — Remote app tests.
 *
 * The hardware-free, load-bearing parts of the Remote app are:
 *   1. the .REM file format — RemoteEntryModel::to_string/parse and
 *      RemoteModel::load/save — which must stay byte-compatible with the
 *      PortaPack so a remote authored on either side loads on the other;
 *   2. the comma helpers that format matches upstream's split_string/join/
 *      parse_int character for character;
 *   3. button lookup — delete_entry (pointer lookup), entry_at, index_of and
 *      find_by_name.
 *
 * RF replay itself needs a USRP B200 and is not covered here.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"


#include "ui_remote.hpp"  /* app:: models + comma helpers */

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace {

class TempDir {
   public:
    explicit TempDir(const char* tag) {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("mb200_rem_" + std::string{tag} + "_" +
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

void write_text(const std::string& path, const std::string& content) {
    std::ofstream f{path, std::ios::binary | std::ios::trunc};
    f.write(content.data(), static_cast<std::streamsize>(content.size()));
}

app::RemoteEntryModel make_entry(std::string path, std::string name, uint8_t icon,
                                 uint8_t bg, uint8_t fg, uint64_t freq,
                                 uint32_t rate) {
    app::RemoteEntryModel e{};
    e.path = std::move(path);
    e.name = std::move(name);
    e.icon = icon;
    e.bg_color = bg;
    e.fg_color = fg;
    e.center_frequency = freq;
    e.sample_rate = rate;
    return e;
}

}  // namespace

/* --- comma helpers (upstream split_string / join / parse_int) -------------- */

TEST(remote_split_matches_upstream) {
    auto a = app::remote_split("a,b,c", ',');
    CHECK_EQ(a.size(), size_t{3});
    CHECK_STR_EQ(std::string{a[0]}, "a");
    CHECK_STR_EQ(std::string{a[1]}, "b");
    CHECK_STR_EQ(std::string{a[2]}, "c");

    /* A trailing separator yields a trailing empty column. */
    auto b = app::remote_split("a,b,", ',');
    CHECK_EQ(b.size(), size_t{3});
    CHECK_STR_EQ(std::string{b[2]}, "");

    /* No separator: one column, the whole string. */
    auto c = app::remote_split("solo", ',');
    CHECK_EQ(c.size(), size_t{1});
    CHECK_STR_EQ(std::string{c[0]}, "solo");

    /* Empty input yields no columns. */
    CHECK_EQ(app::remote_split("", ',').size(), size_t{0});

    /* A lone separator yields two empty columns. */
    auto d = app::remote_split(",", ',');
    CHECK_EQ(d.size(), size_t{2});
    CHECK_STR_EQ(std::string{d[0]}, "");
    CHECK_STR_EQ(std::string{d[1]}, "");
}

TEST(remote_join_matches_upstream) {
    CHECK_STR_EQ(app::remote_join(',', {"a", "b", "c"}), "a,b,c");
    CHECK_STR_EQ(app::remote_join(',', {"x"}), "x");
    CHECK_STR_EQ(app::remote_join(',', {}), "");
    CHECK_STR_EQ(app::remote_join(',', {"", ""}), ",");
}

TEST(remote_parse_uint_stops_at_first_nondigit) {
    CHECK_EQ(app::remote_parse_uint("123"), uint64_t{123});
    CHECK_EQ(app::remote_parse_uint("123abc"), uint64_t{123});
    CHECK_EQ(app::remote_parse_uint(""), uint64_t{0});
    CHECK_EQ(app::remote_parse_uint("abc"), uint64_t{0});
    CHECK_EQ(app::remote_parse_uint("433920000"), uint64_t{433'920'000});
}

/* --- RemoteEntryModel line format ------------------------------------------ */

TEST(entry_to_string_is_the_seven_field_form) {
    const auto e = make_entry("/CAPTURES/DOOR.C16", "Door", 18, 3, 1,
                              433'920'000, 500'000);
    CHECK_STR_EQ(e.to_string(),
                 "/CAPTURES/DOOR.C16,Door,18,3,1,433920000,500000");
}

TEST(entry_line_round_trips) {
    const auto in = make_entry("/CAPTURES/GATE.C8", "Gate", 4, 9, 8,
                               315'000'000, 250'000);

    const auto parsed = app::RemoteEntryModel::parse(in.to_string());
    CHECK(parsed.has_value());
    if (!parsed) return;

    CHECK_STR_EQ(parsed->path, in.path);
    CHECK_STR_EQ(parsed->name, in.name);
    CHECK_EQ(parsed->icon, in.icon);
    CHECK_EQ(parsed->bg_color, in.bg_color);
    CHECK_EQ(parsed->fg_color, in.fg_color);
    CHECK_EQ(parsed->center_frequency, in.center_frequency);
    CHECK_EQ(parsed->sample_rate, in.sample_rate);
}

TEST(entry_parse_rejects_too_few_columns) {
    /* Upstream requires at least 7 comma columns. */
    CHECK(!app::RemoteEntryModel::parse("").has_value());
    CHECK(!app::RemoteEntryModel::parse("just,a,name").has_value());
    CHECK(!app::RemoteEntryModel::parse("p,n,1,2,3,400").has_value());  /* 6 */
}

TEST(entry_parse_accepts_exactly_seven_and_ignores_extra) {
    /* Seven is enough; anything past the seventh column is dropped. */
    auto seven = app::RemoteEntryModel::parse("p,n,1,2,3,400,500");
    CHECK(seven.has_value());

    auto more = app::RemoteEntryModel::parse("p,n,1,2,3,400,500,junk,and,more");
    CHECK(more.has_value());
    if (!more) return;
    CHECK_STR_EQ(more->path, "p");
    CHECK_STR_EQ(more->name, "n");
    CHECK_EQ(more->icon, uint8_t{1});
    CHECK_EQ(more->center_frequency, uint64_t{400});
    CHECK_EQ(more->sample_rate, uint32_t{500});
}

TEST(entry_parse_defaults_bad_numbers_to_zero) {
    /* parse_int leaves an unparseable field at 0 (path/name are free text). */
    auto e = app::RemoteEntryModel::parse("p,n,x,y,z,q,w");
    CHECK(e.has_value());
    if (!e) return;
    CHECK_STR_EQ(e->path, "p");
    CHECK_STR_EQ(e->name, "n");
    CHECK_EQ(e->icon, uint8_t{0});
    CHECK_EQ(e->bg_color, uint8_t{0});
    CHECK_EQ(e->fg_color, uint8_t{0});
    CHECK_EQ(e->center_frequency, uint64_t{0});
    CHECK_EQ(e->sample_rate, uint32_t{0});
}

TEST(entry_parse_out_of_range_index_becomes_zero) {
    /* checked_assign leaves an out-of-range value at 0 (icon is uint8_t). */
    auto e = app::RemoteEntryModel::parse("p,n,300,2,3,400,500");
    CHECK(e.has_value());
    if (!e) return;
    CHECK_EQ(e->icon, uint8_t{0});
}

/* --- RemoteModel file save/load -------------------------------------------- */

TEST(model_save_load_round_trip) {
    TempDir dir{"io"};
    const std::string path = dir.file("HOUSE.REM");

    app::RemoteModel out{};
    out.name = "House";
    out.entries.push_back(make_entry("/CAPTURES/A.C16", "Lights", 2, 3, 1,
                                     433'920'000, 500'000));
    out.entries.push_back(make_entry("/CAPTURES/B.C8", "Fan", 5, 9, 8,
                                     315'000'000, 250'000));

    CHECK(out.save(path));

    app::RemoteModel in{};
    CHECK(in.load(path));
    CHECK_STR_EQ(in.name, "House");
    CHECK_EQ(in.entries.size(), size_t{2});
    if (in.entries.size() != 2) return;

    for (size_t i = 0; i < 2; ++i) {
        CHECK_STR_EQ(in.entries[i].path, out.entries[i].path);
        CHECK_STR_EQ(in.entries[i].name, out.entries[i].name);
        CHECK_EQ(in.entries[i].icon, out.entries[i].icon);
        CHECK_EQ(in.entries[i].bg_color, out.entries[i].bg_color);
        CHECK_EQ(in.entries[i].fg_color, out.entries[i].fg_color);
        CHECK_EQ(in.entries[i].center_frequency, out.entries[i].center_frequency);
        CHECK_EQ(in.entries[i].sample_rate, out.entries[i].sample_rate);
    }
}

TEST(model_load_takes_first_line_as_name_and_skips_noise) {
    TempDir dir{"parse"};
    const std::string path = dir.file("LAYOUT.REM");

    /* First non-blank, non-comment line is the name; comments and unparseable
     * lines (too few columns) are skipped. */
    write_text(path,
               "# a comment\n"
               "My Remote\n"
               "p1,A,2,3,1,100,200\n"
               "   \n"                 /* not empty, not '#': parses, too few, dropped */
               "# note\n"
               "p2,B,4,5,6,700,800\n");

    app::RemoteModel model{};
    CHECK(model.load(path));
    CHECK_STR_EQ(model.name, "My Remote");
    CHECK_EQ(model.entries.size(), size_t{2});
    if (model.entries.size() != 2) return;
    CHECK_STR_EQ(model.entries[0].name, "A");
    CHECK_STR_EQ(model.entries[1].name, "B");
    CHECK_EQ(model.entries[1].center_frequency, uint64_t{700});
}

TEST(model_load_trims_the_name_line) {
    TempDir dir{"trim"};
    const std::string path = dir.file("SP.REM");
    write_text(path, "   Spaced Name   \np,X,0,0,0,0,0\n");

    app::RemoteModel model{};
    CHECK(model.load(path));
    CHECK_STR_EQ(model.name, "Spaced Name");
    CHECK_EQ(model.entries.size(), size_t{1});
}

TEST(model_load_missing_file_returns_false) {
    TempDir dir{"missing"};
    app::RemoteModel model{};
    CHECK(!model.load(dir.file("does_not_exist.REM")));
}

TEST(model_save_load_empty_remote) {
    TempDir dir{"empty"};
    const std::string path = dir.file("EMPTY.REM");

    app::RemoteModel out{};
    out.name = "Empty";
    CHECK(out.save(path));

    app::RemoteModel in{};
    CHECK(in.load(path));
    CHECK_STR_EQ(in.name, "Empty");
    CHECK_EQ(in.entries.size(), size_t{0});
}

/* --- button lookup --------------------------------------------------------- */

TEST(delete_entry_removes_by_pointer_lookup) {
    app::RemoteModel model{};
    model.entries.push_back(make_entry("a", "A", 0, 0, 0, 0, 0));
    model.entries.push_back(make_entry("b", "B", 0, 0, 0, 0, 0));
    model.entries.push_back(make_entry("c", "C", 0, 0, 0, 0, 0));

    /* Delete the middle entry via a pointer into the vector. */
    CHECK(model.delete_entry(&model.entries[1]));
    CHECK_EQ(model.entries.size(), size_t{2});
    CHECK_STR_EQ(model.entries[0].name, "A");
    CHECK_STR_EQ(model.entries[1].name, "C");
}

TEST(delete_entry_rejects_foreign_pointer) {
    app::RemoteModel model{};
    model.entries.push_back(make_entry("a", "A", 0, 0, 0, 0, 0));

    app::RemoteEntryModel not_ours = make_entry("x", "X", 0, 0, 0, 0, 0);
    CHECK(!model.delete_entry(&not_ours));
    CHECK_EQ(model.entries.size(), size_t{1});
}

TEST(entry_at_maps_slot_to_entry) {
    app::RemoteModel model{};
    model.entries.push_back(make_entry("a", "A", 0, 0, 0, 0, 0));
    model.entries.push_back(make_entry("b", "B", 0, 0, 0, 0, 0));

    CHECK(model.entry_at(0) == &model.entries[0]);
    CHECK(model.entry_at(1) == &model.entries[1]);
    CHECK(model.entry_at(2) == nullptr);
    CHECK(model.entry_at(99) == nullptr);
}

TEST(index_of_is_the_inverse_of_entry_at) {
    app::RemoteModel model{};
    model.entries.push_back(make_entry("a", "A", 0, 0, 0, 0, 0));
    model.entries.push_back(make_entry("b", "B", 0, 0, 0, 0, 0));
    model.entries.push_back(make_entry("c", "C", 0, 0, 0, 0, 0));

    CHECK_EQ(model.index_of(&model.entries[0]), 0);
    CHECK_EQ(model.index_of(&model.entries[2]), 2);

    app::RemoteEntryModel foreign = make_entry("z", "Z", 0, 0, 0, 0, 0);
    CHECK_EQ(model.index_of(&foreign), -1);
}

TEST(find_by_name_returns_first_match) {
    app::RemoteModel model{};
    model.entries.push_back(make_entry("a", "Door", 0, 0, 0, 0, 0));
    model.entries.push_back(make_entry("b", "Gate", 0, 0, 0, 0, 0));
    model.entries.push_back(make_entry("c", "Gate", 0, 0, 0, 0, 0));

    const auto* found = model.find_by_name("Gate");
    CHECK(found == &model.entries[1]);           /* first match, not the second */
    CHECK(model.find_by_name("Door") == &model.entries[0]);
    CHECK(model.find_by_name("Missing") == nullptr);
}
