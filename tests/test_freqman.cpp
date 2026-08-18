/*
 * mayhem-b200 — frequency manager tests.
 *
 * Every expected value here was derived from the firmware's
 * application/freqman_db.cpp: the option tables (so an index means the same
 * thing in both), the field order to_freqman_string emits, the validation
 * rules, and the quirks a real .TXT depends on — `bw=` only counting after
 * `m=`, and an entry with no modulation inheriting the previous one's. If a
 * file written here does not load on a PortaPack, one of these is wrong.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"


#include "file_path.hpp"
#include "freqman_db.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

using core::freqman_entry;
using core::freqman_invalid_index;
using core::freqman_type;

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

    explicit ScopedFile(const std::string& p)
        : path{p} { remove(); }
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
    void write(const std::string& text) const {
        std::ofstream f{path, std::ios::binary | std::ios::trunc};
        f << text;
    }
    std::string read() const {
        std::ifstream f{path, std::ios::binary};
        return std::string{std::istreambuf_iterator<char>{f},
                           std::istreambuf_iterator<char>{}};
    }
};

freqman_entry parsed(const char* line) {
    freqman_entry entry;
    core::parse_freqman_entry(line, entry);
    return entry;
}

}  // namespace

/* --- Option tables ---------------------------------------------------- */

TEST(freqman_option_tables_match_upstream_indices) {
    /* These indices are what gets written into a file, so they are part of the
     * format, not an implementation detail. */
    CHECK_EQ(static_cast<int>(core::freqman_find_modulation("AM")), 0);
    CHECK_EQ(static_cast<int>(core::freqman_find_modulation("NFM")), 1);
    CHECK_EQ(static_cast<int>(core::freqman_find_modulation("WFM")), 2);
    CHECK_EQ(static_cast<int>(core::freqman_find_modulation("SPEC")), 3);
    CHECK_EQ(static_cast<int>(core::freqman_find_modulation("AMFM")), 4);
    CHECK_EQ(static_cast<int>(core::freqman_find_modulation("FMAM")), 5);
    CHECK_EQ(core::freqman_find_modulation("BOGUS"), freqman_invalid_index);

    CHECK_STR_EQ(core::freqman_entry_get_modulation_string(2), "WFM");
    CHECK_STR_EQ(core::freqman_entry_get_modulation_string(freqman_invalid_index), "");

    /* NFM bandwidths. */
    CHECK_EQ(static_cast<int>(core::freqman_find_bandwidth(1, "8k5")), 0);
    CHECK_EQ(static_cast<int>(core::freqman_find_bandwidth(1, "12k5")), 2);
    CHECK_STR_EQ(core::freqman_entry_get_bandwidth_string(1, 3), "16k");

    /* WFM's table is deliberately not in value order: position 0 is "80k",
     * whose radio configuration index is 2. */
    CHECK_EQ(static_cast<int>(core::freqman_find_bandwidth(2, "80k")), 0);
    CHECK_EQ(static_cast<int>(core::freqman_find_bandwidth(2, "200k")), 2);
    CHECK_EQ(core::freqman_entry_get_bandwidth_value(2, 0), 2);
    CHECK_EQ(core::freqman_entry_get_bandwidth_value(2, 2), 0);

    /* SPEC stores Hz rather than a configuration index. */
    CHECK_EQ(core::freqman_entry_get_bandwidth_value(3, 0), 12500);

    CHECK_EQ(core::freqman_entry_get_bandwidth_value(1, freqman_invalid_index), -1);
    CHECK_EQ(core::freqman_entry_get_bandwidth_value(freqman_invalid_index, 0), -1);

    /* Steps. */
    CHECK_EQ(static_cast<int>(core::freqman_find_step("25kHz")), 11);
    CHECK_EQ(core::freqman_entry_get_step_value(11), 25000);
    CHECK_STR_EQ(core::freqman_entry_get_step_string_short(11), "25kHz");
    CHECK_STR_EQ(core::freqman_entry_get_step_string(11), "25kHz   (N1)");
    CHECK_EQ(core::freqman_find_step("25kHz   (N1)"), freqman_invalid_index);
    CHECK_EQ(core::freqman_entry_get_step_value(freqman_invalid_index), -1);
}

TEST(freqman_tone_keys) {
    /* CTCSS 88.5 Hz is table entry 9, "8 YB", stored as 8850 centihertz. */
    CHECK_EQ(static_cast<int>(core::freqman_parse_tone_key("88.5")), 9);
    CHECK_STR_EQ(core::freqman_tone_key_value_string(9), "88.5");
    CHECK_STR_EQ(core::freqman_tone_key_name(9), "8 YB");

    /* Whole numbers, and a value inside the +/- 4 Hz match window. */
    CHECK_EQ(static_cast<int>(core::freqman_parse_tone_key("67")), 1);
    CHECK_EQ(static_cast<int>(core::freqman_parse_tone_key("88.6")), 9);

    /* Nothing within tolerance -> invalid index. */
    CHECK_EQ(core::freqman_parse_tone_key("500"), freqman_invalid_index);

    /* Empty and junk must not read off the end of the split. */
    CHECK_EQ(static_cast<int>(core::freqman_parse_tone_key("")), 0);  /* "None" */
    CHECK_EQ(static_cast<int>(core::freqman_parse_tone_key("0")), 0);

    CHECK_STR_EQ(core::freqman_tone_key_value_string(freqman_invalid_index), "");
}

/* --- Parsing each entry form ------------------------------------------ */

TEST(freqman_parses_single_frequency) {
    freqman_entry e;
    CHECK(core::parse_freqman_entry("f=437500000,d=Description", e));

    CHECK_EQ(e.type, freqman_type::Single);
    CHECK_EQ(e.frequency_a, static_cast<int64_t>(437500000));
    CHECK_EQ(e.frequency_b, static_cast<int64_t>(0));
    CHECK_STR_EQ(e.description, "Description");
    CHECK_EQ(e.modulation, freqman_invalid_index);
    CHECK_EQ(e.bandwidth, freqman_invalid_index);
    CHECK_EQ(e.step, freqman_invalid_index);
    CHECK_EQ(e.tone, freqman_invalid_index);
}

TEST(freqman_parses_range) {
    freqman_entry e;
    CHECK(core::parse_freqman_entry("a=430000000,b=440000000,d=Range description", e));

    CHECK_EQ(e.type, freqman_type::Range);
    CHECK_EQ(e.frequency_a, static_cast<int64_t>(430000000));
    CHECK_EQ(e.frequency_b, static_cast<int64_t>(440000000));
    CHECK_STR_EQ(e.description, "Range description");
}

TEST(freqman_parses_hamradio_and_repeater) {
    freqman_entry ham;
    CHECK(core::parse_freqman_entry("r=145600000,t=145000000,d=Ham", ham));
    CHECK_EQ(ham.type, freqman_type::HamRadio);
    CHECK_EQ(ham.frequency_a, static_cast<int64_t>(145600000));
    CHECK_EQ(ham.frequency_b, static_cast<int64_t>(145000000));

    freqman_entry rep;
    CHECK(core::parse_freqman_entry("l=145600000,t=145000000,d=Repeater", rep));
    CHECK_EQ(rep.type, freqman_type::Repeater);
    CHECK_EQ(rep.frequency_a, static_cast<int64_t>(145600000));
    CHECK_EQ(rep.frequency_b, static_cast<int64_t>(145000000));
}

TEST(freqman_parses_every_optional_field) {
    freqman_entry e;
    CHECK(core::parse_freqman_entry(
        "a=430000000,b=440000000,s=25kHz,m=NFM,bw=12k5,d=All fields", e));

    CHECK_EQ(e.type, freqman_type::Range);
    CHECK_EQ(e.frequency_a, static_cast<int64_t>(430000000));
    CHECK_EQ(e.frequency_b, static_cast<int64_t>(440000000));
    CHECK_EQ(static_cast<int>(e.step), 11);         /* 25kHz */
    CHECK_EQ(static_cast<int>(e.modulation), 1);    /* NFM   */
    CHECK_EQ(static_cast<int>(e.bandwidth), 2);     /* 12k5  */
    CHECK_STR_EQ(e.description, "All fields");

    /* A ham entry carries a tone instead of a step. */
    freqman_entry ham;
    CHECK(core::parse_freqman_entry(
        "r=145600000,t=145000000,c=88.5,m=NFM,bw=11k,d=Ham relay", ham));
    CHECK_EQ(ham.type, freqman_type::HamRadio);
    CHECK_EQ(static_cast<int>(ham.tone), 9);
    CHECK_EQ(static_cast<int>(ham.modulation), 1);
    CHECK_EQ(static_cast<int>(ham.bandwidth), 1);   /* 11k */
    CHECK_STR_EQ(ham.description, "Ham relay");
}

TEST(freqman_bandwidth_needs_modulation_first) {
    /* Upstream reads fields left to right and only accepts `bw=` once a
     * modulation is known. A file with them the wrong way round loses the
     * bandwidth — matching that is what keeps files interchangeable. */
    freqman_entry late = parsed("f=100000000,bw=200k,m=WFM,d=x");
    CHECK_EQ(static_cast<int>(late.modulation), 2);
    CHECK_EQ(late.bandwidth, freqman_invalid_index);

    freqman_entry ordered = parsed("f=100000000,m=WFM,bw=200k,d=x");
    CHECK_EQ(static_cast<int>(ordered.modulation), 2);
    CHECK_EQ(static_cast<int>(ordered.bandwidth), 2);
}

TEST(freqman_description_is_trimmed_and_capped_at_30) {
    freqman_entry e = parsed("f=1000,d=  ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789  ");
    CHECK_STR_EQ(e.description, "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123");
    CHECK_EQ(e.description.size(), core::freqman_max_desc_size);
}

TEST(freqman_rejects_malformed_and_invalid_entries) {
    freqman_entry e;

    CHECK(!core::parse_freqman_entry("", e));
    CHECK(!core::parse_freqman_entry("# a comment", e));
    CHECK(!core::parse_freqman_entry("no fields at all", e));
    CHECK(!core::parse_freqman_entry("d=description only", e));

    /* Frequency A is required by every type. */
    CHECK(!core::parse_freqman_entry("f=0,d=x", e));
    CHECK(!core::parse_freqman_entry("f=not a number,d=x", e));

    /* Frequency B is required by the two-frequency types. */
    CHECK(!core::parse_freqman_entry("a=430000000,d=x", e));
    CHECK(!core::parse_freqman_entry("r=145600000,d=x", e));
    CHECK(!core::parse_freqman_entry("l=145600000,d=x", e));

    /* A range must run upwards. */
    CHECK(!core::parse_freqman_entry("a=440000000,b=430000000,d=x", e));
    CHECK(core::parse_freqman_entry("a=430000000,b=430000000,d=x", e));

    /* An unknown key is skipped, not fatal. */
    freqman_entry ok = parsed("f=437500000,zz=99,d=Still good");
    CHECK(core::is_valid(ok));
    CHECK_STR_EQ(ok.description, "Still good");

    /* An unknown modulation or step name leaves the field unset. */
    freqman_entry unknown = parsed("f=437500000,m=BOGUS,s=7kHz,d=x");
    CHECK(core::is_valid(unknown));
    CHECK_EQ(unknown.modulation, freqman_invalid_index);
    CHECK_EQ(unknown.step, freqman_invalid_index);
}

/* --- Serialisation ---------------------------------------------------- */

TEST(freqman_serialises_in_upstream_field_order) {
    /* The order matters: a PortaPack reads left to right and needs `m=` before
     * `bw=`. */
    const char* lines[] = {
        "f=437500000,d=Description",
        "a=430000000,b=440000000,s=25kHz,m=NFM,bw=12k5,d=All fields",
        "r=145600000,t=145000000,c=88.5,m=NFM,bw=11k,d=Ham relay",
        "l=145600000,t=145000000,m=NFM,d=Repeater",
        "f=100000000,m=WFM,bw=200k,d=Broadcast",
    };

    for (const char* line : lines) {
        freqman_entry e;
        CHECK(core::parse_freqman_entry(line, e));
        CHECK_STR_EQ(core::to_freqman_string(e), line);
    }
}

TEST(freqman_serialises_optional_fields_only_when_set) {
    freqman_entry e;
    e.type = freqman_type::Single;
    e.frequency_a = 433920000;
    CHECK_STR_EQ(core::to_freqman_string(e), "f=433920000");

    e.description = "ISM";
    CHECK_STR_EQ(core::to_freqman_string(e), "f=433920000,d=ISM");

    /* Bandwidth without a modulation cannot be written — there is no table to
     * name it from. */
    e.bandwidth = 2;
    CHECK_STR_EQ(core::to_freqman_string(e), "f=433920000,d=ISM");

    e.modulation = 1;  /* NFM */
    CHECK_STR_EQ(core::to_freqman_string(e), "f=433920000,m=NFM,bw=12k5,d=ISM");

    /* A step belongs to a range, and a tone to a ham entry; neither is written
     * for a single frequency. */
    e.step = 11;
    e.tone = 9;
    CHECK_STR_EQ(core::to_freqman_string(e), "f=433920000,m=NFM,bw=12k5,d=ISM");

    /* Raw round-trips its line; Unknown serialises to nothing. */
    freqman_entry raw;
    raw.type = freqman_type::Raw;
    raw.description = "anything at all";
    CHECK_STR_EQ(core::to_freqman_string(raw), "anything at all");

    CHECK_STR_EQ(core::to_freqman_string(freqman_entry{}), "");
}

TEST(freqman_pretty_string) {
    CHECK_STR_EQ(core::pretty_string(parsed("f=437500000,d=Description")),
                 " 437.5000M: Description");
    CHECK_STR_EQ(core::pretty_string(parsed("a=430000000,b=440000000,d=Range")),
                 "430.0M-440.0M: Range");
    CHECK_STR_EQ(core::pretty_string(parsed("r=145600000,t=145000000,d=Ham")),
                 "R:145.6M,T:145.0M: Ham");
    CHECK_STR_EQ(core::pretty_string(parsed("l=145600000,t=145000000,d=Repeater")),
                 "L:145.6M,T:145.0M: Repeater");

    freqman_entry raw;
    raw.type = freqman_type::Raw;
    raw.description = "junk line";
    CHECK_STR_EQ(core::pretty_string(raw), "junk line");

    CHECK_STR_EQ(core::pretty_string(freqman_entry{}), "UNK:");

    /* Too long for the field: cut and marked with '+'. */
    CHECK_STR_EQ(core::pretty_string(parsed("f=437500000,d=Description"), 10),
                 " 437.5000+");
}

TEST(freqman_entry_equality_follows_the_type) {
    auto a = parsed("a=430000000,b=440000000,s=25kHz,d=x");
    auto b = a;
    CHECK(a == b);

    /* Step is part of a range's identity. */
    b.step = core::freqman_find_step("50kHz");
    CHECK(a != b);

    /* ...but not a single frequency's. */
    auto s1 = parsed("f=437500000,d=x");
    auto s2 = s1;
    s2.step = core::freqman_find_step("50kHz");
    CHECK(s1 == s2);

    /* Tone is part of a ham entry's identity. */
    auto h1 = parsed("r=145600000,t=145000000,c=88.5,d=x");
    auto h2 = h1;
    h2.tone = core::freqman_parse_tone_key("100");
    CHECK(h1 != h2);

    /* Description and modulation always count. */
    auto d1 = parsed("f=437500000,d=one");
    auto d2 = parsed("f=437500000,d=two");
    CHECK(d1 != d2);
}

/* --- Files ------------------------------------------------------------ */

TEST(freqman_file_round_trips_through_save_and_load) {
    ScopedFile file{temp_path("mb200_test_list.TXT")};

    core::freqman_db original;
    original.push_back(std::make_unique<freqman_entry>(
        parsed("f=437500000,m=NFM,bw=12k5,d=Single")));
    original.push_back(std::make_unique<freqman_entry>(
        parsed("a=430000000,b=440000000,s=25kHz,m=NFM,bw=12k5,d=Range")));
    original.push_back(std::make_unique<freqman_entry>(
        parsed("r=145600000,t=145000000,c=88.5,m=NFM,bw=12k5,d=Ham")));
    original.push_back(std::make_unique<freqman_entry>(
        parsed("l=145600000,t=145000000,m=NFM,bw=12k5,d=Repeater")));

    CHECK(core::save_freqman_db(file.path, original));
    CHECK(file.exists());

    core::freqman_db reloaded;
    CHECK(core::parse_freqman_file(file.path, reloaded));

    CHECK_EQ(reloaded.size(), original.size());
    for (size_t i = 0; i < std::min(reloaded.size(), original.size()); ++i)
        CHECK(*reloaded[i] == *original[i]);

    /* And the bytes on disk are the format a PortaPack expects. */
    const auto text = file.read();
    CHECK(text.find("f=437500000,m=NFM,bw=12k5,d=Single\r\n") == 0);
    CHECK(text.find("r=145600000,t=145000000,c=88.5,m=NFM,bw=12k5,d=Ham") !=
          std::string::npos);
}

TEST(freqman_file_keeps_the_good_lines_when_some_are_bad) {
    ScopedFile file{temp_path("mb200_test_bad.TXT")};
    file.write(
        "f=437500000,d=Good one\r\n"
        "this line is not a freqman entry\r\n"
        "# a comment\r\n"
        "\r\n"
        "a=430000000,b=440000000,d=Good two\r\n"
        "f=0,d=Zero frequency\r\n"
        "a=440000000,b=430000000,d=Backwards range\r\n"
        "l=145600000,t=145000000,d=Good three\r\n");

    core::freqman_db db;
    CHECK(core::parse_freqman_file(file.path, db));

    CHECK_EQ(db.size(), static_cast<size_t>(3));
    if (db.size() == 3) {
        CHECK_STR_EQ(db[0]->description, "Good one");
        CHECK_EQ(db[0]->type, freqman_type::Single);
        CHECK_STR_EQ(db[1]->description, "Good two");
        CHECK_EQ(db[1]->type, freqman_type::Range);
        CHECK_STR_EQ(db[2]->description, "Good three");
        CHECK_EQ(db[2]->type, freqman_type::Repeater);
    }
}

TEST(freqman_file_reads_a_final_line_without_a_newline) {
    ScopedFile file{temp_path("mb200_test_nonl.TXT")};
    file.write("f=100000000,d=One\nf=200000000,d=Two");

    core::freqman_db db;
    CHECK(core::parse_freqman_file(file.path, db));
    CHECK_EQ(db.size(), static_cast<size_t>(2));
    if (db.size() == 2) CHECK_STR_EQ(db[1]->description, "Two");
}

TEST(freqman_file_inherits_modulation_from_the_previous_entry) {
    ScopedFile file{temp_path("mb200_test_inherit.TXT")};
    file.write(
        "f=100000000,m=WFM,bw=200k,d=First\r\n"
        "f=101000000,d=Second\r\n"
        "f=102000000,m=AM,d=Third\r\n"
        "f=103000000,d=Fourth\r\n");

    core::freqman_db db;
    CHECK(core::parse_freqman_file(file.path, db));
    CHECK_EQ(db.size(), static_cast<size_t>(4));
    if (db.size() != 4) return;

    CHECK_EQ(static_cast<int>(db[0]->modulation), 2);  /* WFM  */
    CHECK_EQ(static_cast<int>(db[0]->bandwidth), 2);   /* 200k */

    /* Inherited from the line above. */
    CHECK_EQ(static_cast<int>(db[1]->modulation), 2);
    CHECK_EQ(static_cast<int>(db[1]->bandwidth), 2);

    /* An explicit modulation with no bandwidth still inherits the bandwidth,
     * exactly as upstream does — the index then means something else, which is
     * an upstream wart, not ours to fix here. */
    CHECK_EQ(static_cast<int>(db[2]->modulation), 0);  /* AM */
    CHECK_EQ(static_cast<int>(db[2]->bandwidth), 2);

    CHECK_EQ(static_cast<int>(db[3]->modulation), 0);
}

TEST(freqman_load_options_filter_and_cap) {
    ScopedFile file{temp_path("mb200_test_opts.TXT")};
    file.write(
        "f=100000000,d=Single one\r\n"
        "a=430000000,b=440000000,d=A range\r\n"
        "r=145600000,t=145000000,d=A ham\r\n"
        "l=145600000,t=145000000,d=A repeater\r\n"
        "f=200000000,d=Single two\r\n");

    core::freqman_load_options only_freqs;
    only_freqs.load_ranges = false;
    only_freqs.load_hamradios = false;
    only_freqs.load_repeaters = false;

    core::freqman_db db;
    CHECK(core::parse_freqman_file(file.path, db, only_freqs));
    CHECK_EQ(db.size(), static_cast<size_t>(2));
    if (db.size() == 2) {
        CHECK_STR_EQ(db[0]->description, "Single one");
        CHECK_STR_EQ(db[1]->description, "Single two");
    }

    core::freqman_load_options capped;
    capped.max_entries = 3;
    core::freqman_db capped_db;
    CHECK(core::parse_freqman_file(file.path, capped_db, capped));
    CHECK_EQ(capped_db.size(), static_cast<size_t>(3));

    core::freqman_load_options unlimited;
    unlimited.max_entries = 0;
    core::freqman_db all;
    CHECK(core::parse_freqman_file(file.path, all, unlimited));
    CHECK_EQ(all.size(), static_cast<size_t>(5));
}

TEST(freqman_parse_of_a_missing_file_fails) {
    core::freqman_db db;
    CHECK(!core::parse_freqman_file(temp_path("mb200_no_such_list.TXT"), db));
}

/* --- FreqmanDB CRUD --------------------------------------------------- */

TEST(freqman_db_create_and_edit) {
    ScopedFile file{temp_path("mb200_test_crud.TXT")};

    core::FreqmanDB db;
    CHECK(!db.open(file.path));  /* not there yet */
    CHECK(db.open(file.path, true));
    CHECK(db.is_open());
    CHECK(db.empty());
    CHECK_EQ(db.entry_count(), 0u);
    CHECK(db.begin() == db.end());

    const auto single = parsed("f=437500000,m=NFM,d=Single");
    const auto range = parsed("a=430000000,b=440000000,s=25kHz,m=NFM,d=Range");
    const auto ham = parsed("r=145600000,t=145000000,c=88.5,m=NFM,d=Ham");

    db.append_entry(single);
    db.append_entry(range);
    CHECK_EQ(db.entry_count(), 2u);
    CHECK(db[0] == single);
    CHECK(db[1] == range);

    /* Insert at the front. */
    db.insert_entry(0, ham);
    CHECK_EQ(db.entry_count(), 3u);
    CHECK(db[0] == ham);
    CHECK(db[1] == single);

    /* Replace in the middle. */
    const auto replacement = parsed("f=88500000,m=WFM,d=Replaced");
    db.replace_entry(1, replacement);
    CHECK(db[1] == replacement);
    CHECK_EQ(db.entry_count(), 3u);

    /* Find by value, then delete by value. */
    auto it = db.find_entry(range);
    CHECK(it != db.end());
    CHECK_EQ(it.index(), 2u);
    CHECK(db.delete_entry(range));
    CHECK_EQ(db.entry_count(), 2u);
    CHECK(db.find_entry(range) == db.end());

    /* Deleting something that is not there is a no-op, not a crash. */
    CHECK(!db.delete_entry(range));

    /* Autosave means all of that is already on disk. */
    core::FreqmanDB reopened;
    CHECK(reopened.open(file.path));
    CHECK_EQ(reopened.entry_count(), 2u);
    CHECK(reopened[0] == ham);
    CHECK(reopened[1] == replacement);

    /* Iteration visits every entry in order. */
    size_t seen = 0;
    for (auto entry : reopened) {
        CHECK(core::is_valid(entry));
        ++seen;
    }
    CHECK_EQ(seen, static_cast<size_t>(2));
}

TEST(freqman_db_out_of_range_access_is_safe) {
    ScopedFile file{temp_path("mb200_test_oob.TXT")};

    core::FreqmanDB db;
    CHECK(db.open(file.path, true));

    CHECK_EQ(db[0].type, freqman_type::Unknown);
    CHECK_STR_EQ(db.line(5), "");

    db.replace_entry(3, parsed("f=1000000,d=x"));  /* nothing to replace */
    CHECK_EQ(db.entry_count(), 0u);

    db.delete_entry(3);
    CHECK_EQ(db.entry_count(), 0u);

    /* An index past the end appends rather than corrupting anything. */
    db.insert_entry(99, parsed("f=1000000,d=x"));
    CHECK_EQ(db.entry_count(), 1u);
}

TEST(freqman_db_raw_lines) {
    ScopedFile file{temp_path("mb200_test_raw.TXT")};
    file.write("f=437500000,d=Good\r\nnot an entry\r\n");

    core::FreqmanDB db;
    CHECK(db.open(file.path));
    CHECK_EQ(db.entry_count(), 2u);

    /* An editor wants to see the bad line so the user can fix it. */
    CHECK(db.read_raw());
    CHECK_EQ(db[1].type, freqman_type::Raw);
    CHECK_STR_EQ(db[1].description, "not an entry");
    CHECK_STR_EQ(db.line(1), "not an entry");

    /* A loader wants it gone. */
    db.set_read_raw(false);
    CHECK_EQ(db[1].type, freqman_type::Unknown);
}

TEST(freqman_db_batched_edits_with_autosave_off) {
    ScopedFile file{temp_path("mb200_test_batch.TXT")};

    core::FreqmanDB db;
    CHECK(db.open(file.path, true));
    db.set_autosave(false);

    db.append_entry(parsed("f=100000000,d=One"));
    db.append_entry(parsed("f=200000000,d=Two"));
    CHECK(db.dirty());

    /* Nothing written yet. */
    core::FreqmanDB peek;
    CHECK(peek.open(file.path));
    CHECK_EQ(peek.entry_count(), 0u);

    CHECK(db.save());
    CHECK(!db.dirty());

    core::FreqmanDB after;
    CHECK(after.open(file.path));
    CHECK_EQ(after.entry_count(), 2u);
}

/* --- Named lists ------------------------------------------------------ */

TEST(freqman_named_lists_live_in_the_freqman_directory) {
    /* PID: this stem names a real file in the shared FREQMAN dir. */
    const std::string stem = "__mb200_test_category_" + std::to_string(mb200test::test_pid());
    const auto path = core::get_freqman_path(stem);

    CHECK(path.find(stem) != std::string::npos);
    CHECK(path.size() > 4);
    CHECK_STR_EQ(path.substr(path.size() - 4), ".TXT");
    CHECK(path.find(core::freqman_directory()) == 0);

    /* Clean slate even if a previous run died mid-test. */
    core::delete_freqman_file(stem);

    CHECK(core::create_freqman_file(stem));

    auto names = core::get_freqman_files();
    CHECK(std::find(names.begin(), names.end(), stem) != names.end());
    /* The listing is sorted, which is what the category picker relies on. */
    CHECK(std::is_sorted(names.begin(), names.end()));

    core::freqman_db db;
    db.push_back(std::make_unique<freqman_entry>(parsed("f=433920000,d=ISM")));
    db.push_back(std::make_unique<freqman_entry>(parsed("f=446006250,d=PMR446 1")));
    CHECK(core::save_freqman_file(stem, db));

    core::freqman_db reloaded;
    CHECK(core::load_freqman_file(stem, reloaded));
    CHECK_EQ(reloaded.size(), static_cast<size_t>(2));
    if (reloaded.size() == 2) CHECK_STR_EQ(reloaded[1]->description, "PMR446 1");

    CHECK(core::delete_freqman_file(stem));
    names = core::get_freqman_files();
    CHECK(std::find(names.begin(), names.end(), stem) == names.end());

    /* Deleting twice reports failure rather than pretending. */
    CHECK(!core::delete_freqman_file(stem));
}
