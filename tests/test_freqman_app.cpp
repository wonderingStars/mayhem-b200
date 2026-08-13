/*
 * mayhem-b200 — Frequency Manager UI logic tests.
 *
 * The manager view (src/apps/ui_freqman.*) is UI glue over core::FreqmanDB, so
 * what is testable without a display is the *data path* the view drives: the
 * exact FreqmanDB operations behind Add / Edit / Delete / Reorder, the default
 * entry the Add button builds, the frequency the Tune button hands to the
 * receiver, the option-table lookups the editor populates its OptionsFields
 * from, and the pretty_string the list renders.
 *
 * Expected values are derived from the firmware's freqman_db.cpp (option tables,
 * field order, pretty_string form) — not from this port's output — so a list the
 * manager writes still loads on a PortaPack. Distinct from test_freqman.cpp,
 * which covers the parser/serialiser directly.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include <process.h> /* _getpid: see kStem */

#include "file_path.hpp"
#include "freqman_db.hpp"
#include "string_format.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

using core::freqman_entry;
using core::freqman_invalid_index;
using core::freqman_type;
using Index = core::FreqmanDB::Index;

namespace {

freqman_entry parsed(const char* line) {
    freqman_entry e;
    core::parse_freqman_entry(line, e);
    return e;
}

/* The default entry FrequencyManagerView::on_add_entry() builds. */
freqman_entry make_default_entry(uint32_t n) {
    freqman_entry e{};
    e.type = freqman_type::Single;
    e.frequency_a = 100'000'000;
    e.description = std::string{"Entry "} + to_string_dec_uint(n);
    return e;
}

/* The neighbour swap FrequencyManagerView::on_move_entry() performs. */
void swap_entries(core::FreqmanDB& db, size_t i, size_t j) {
    const auto a = db[static_cast<Index>(i)];
    const auto b = db[static_cast<Index>(j)];
    db.replace_entry(static_cast<Index>(i), b);
    db.replace_entry(static_cast<Index>(j), a);
}

/* A list name the tests own; wiped before and after so a crashed run can't
 * poison the next. PID in the stem because this list lives in the REAL shared
 * FREQMAN directory (<Documents>/mayhem-b200/FREQMAN) -- a fixed stem is the
 * same file for every concurrently-running suite (one per git worktree is
 * normal here), and each ScopedList deletes it, so one process wiped
 * another's live fixture mid-test. Observed 2026-08-13 in a two-process
 * verification run: both suites failed in these tests simultaneously with
 * each seeing the other's entry counts. */
static const std::string kStem = "__mb200_freqman_app_test_" + std::to_string(_getpid());

struct ScopedList {
    ScopedList() { core::delete_freqman_file(kStem); }
    ~ScopedList() { core::delete_freqman_file(kStem); }
    ScopedList(const ScopedList&) = delete;
    ScopedList& operator=(const ScopedList&) = delete;
};

}  // namespace

/* --- Add: default entry the button creates ---------------------------- */

TEST(freqman_app_default_entry_is_valid_and_serialises) {
    const auto e = make_default_entry(0);
    CHECK_EQ(e.type, freqman_type::Single);
    CHECK_EQ(e.frequency_a, static_cast<int64_t>(100'000'000));
    CHECK(core::is_valid(e));
    /* No modulation/bandwidth set, so only f= and d= are written. */
    CHECK_STR_EQ(core::to_freqman_string(e), "f=100000000,d=Entry 0");
    CHECK_STR_EQ(make_default_entry(7).description, "Entry 7");
}

/* --- Add: insert-below-selection index semantics ---------------------- */

TEST(freqman_app_add_inserts_below_selection) {
    ScopedList guard;

    core::FreqmanDB db;
    CHECK(db.open_list(kStem, /*create=*/true));
    CHECK(db.is_open());
    CHECK(db.empty());

    db.append_entry(parsed("f=100000000,d=A"));
    db.append_entry(parsed("f=200000000,d=B"));
    db.append_entry(parsed("f=300000000,d=C"));
    CHECK_EQ(db.entry_count(), 3u);

    /* Selection on index 1 (B); Add inserts at current_index()+1 == 2. */
    const size_t selected = 1;
    db.insert_entry(static_cast<Index>(selected + 1), make_default_entry(3));

    CHECK_EQ(db.entry_count(), 4u);
    CHECK_STR_EQ(db[0].description, "A");
    CHECK_STR_EQ(db[1].description, "B");
    CHECK_STR_EQ(db[2].description, "Entry 3");  /* landed right after B */
    CHECK_STR_EQ(db[3].description, "C");
}

/* --- Edit: replace persists ------------------------------------------- */

TEST(freqman_app_edit_replaces_entry_in_place) {
    ScopedList guard;

    core::FreqmanDB db;
    CHECK(db.open_list(kStem, true));
    db.append_entry(parsed("f=100000000,d=One"));
    db.append_entry(parsed("f=200000000,m=NFM,d=Two"));

    const auto edited = parsed("a=430000000,b=440000000,s=25kHz,m=NFM,bw=12k5,d=Edited");
    db.replace_entry(1, edited);

    CHECK_EQ(db.entry_count(), 2u);
    CHECK(db[1] == edited);
    CHECK_EQ(db[1].type, freqman_type::Range);

    /* Autosave means the edit is already on disk. */
    core::freqman_db reloaded;
    CHECK(core::load_freqman_file(kStem, reloaded));
    CHECK_EQ(reloaded.size(), 2u);
    if (reloaded.size() == 2) CHECK(*reloaded[1] == edited);
}

/* --- Delete ----------------------------------------------------------- */

TEST(freqman_app_delete_removes_selected_entry) {
    ScopedList guard;

    core::FreqmanDB db;
    CHECK(db.open_list(kStem, true));
    db.append_entry(parsed("f=100000000,d=A"));
    db.append_entry(parsed("f=200000000,d=B"));
    db.append_entry(parsed("f=300000000,d=C"));

    db.delete_entry(1);  /* delete B */
    CHECK_EQ(db.entry_count(), 2u);
    CHECK_STR_EQ(db[0].description, "A");
    CHECK_STR_EQ(db[1].description, "C");
}

/* --- Reorder: neighbour swap ------------------------------------------ */

TEST(freqman_app_reorder_swaps_neighbours_and_persists) {
    ScopedList guard;

    core::FreqmanDB db;
    CHECK(db.open_list(kStem, true));
    db.append_entry(parsed("f=100000000,d=A"));
    db.append_entry(parsed("f=200000000,d=B"));
    db.append_entry(parsed("f=300000000,d=C"));

    /* Move C (index 2) up one -> swap with B (index 1). */
    swap_entries(db, 2, 1);
    CHECK_STR_EQ(db[0].description, "A");
    CHECK_STR_EQ(db[1].description, "C");
    CHECK_STR_EQ(db[2].description, "B");

    /* Move A (index 0) down one -> swap with C (index 1). */
    swap_entries(db, 0, 1);
    CHECK_STR_EQ(db[0].description, "C");
    CHECK_STR_EQ(db[1].description, "A");
    CHECK_STR_EQ(db[2].description, "B");

    /* The new order is on disk, unchanged by reload. */
    core::freqman_db reloaded;
    CHECK(core::load_freqman_file(kStem, reloaded));
    CHECK_EQ(reloaded.size(), 3u);
    if (reloaded.size() == 3) {
        CHECK_STR_EQ(reloaded[0]->description, "C");
        CHECK_STR_EQ(reloaded[1]->description, "A");
        CHECK_STR_EQ(reloaded[2]->description, "B");
    }
}

/* --- The full round trip the task asks for ---------------------------- */

TEST(freqman_app_create_add_save_reload_round_trip) {
    ScopedList guard;

    /* Not present until created. */
    auto before = core::get_freqman_files();
    CHECK(std::find(before.begin(), before.end(), kStem) == before.end());

    core::FreqmanDB db;
    CHECK(db.open_list(kStem, /*create=*/true));  // what change_category / New Lst does

    const freqman_entry entries[] = {
        parsed("f=433920000,m=NFM,bw=12k5,d=ISM"),
        parsed("a=430000000,b=440000000,s=25kHz,m=NFM,bw=12k5,d=70cm"),
        parsed("r=145600000,t=145000000,c=88.5,m=NFM,bw=12k5,d=Relay"),
        parsed("l=145600000,t=145000000,m=NFM,bw=12k5,d=Repeater"),
    };
    for (const auto& e : entries)
        db.append_entry(e);
    CHECK_EQ(db.entry_count(), 4u);

    /* The list now shows up in the category picker, sorted. */
    auto listed = core::get_freqman_files();
    CHECK(std::find(listed.begin(), listed.end(), kStem) != listed.end());
    CHECK(std::is_sorted(listed.begin(), listed.end()));

    /* Reload through the same path a freshly-opened manager would. */
    core::freqman_db reloaded;
    CHECK(core::load_freqman_file(kStem, reloaded));
    CHECK_EQ(reloaded.size(), 4u);
    for (size_t i = 0; i < reloaded.size() && i < 4; ++i)
        CHECK(*reloaded[i] == entries[i]);

    /* And a second FreqmanDB opened on the same list sees the same entries. */
    core::FreqmanDB reopened;
    CHECK(reopened.open_list(kStem, false));
    CHECK_EQ(reopened.entry_count(), 4u);
    CHECK(reopened[0] == entries[0]);
    CHECK(reopened[3] == entries[3]);
}

/* --- Tune: which frequency the button hands to the receiver ----------- */

TEST(freqman_app_tune_uses_primary_frequency_per_type) {
    /* on_tune_entry() calls receiver->set_target_frequency(frequency_a). For
     * every type frequency_a is the primary (f / a / r / l) frequency. */
    CHECK_EQ(parsed("f=437500000,d=x").frequency_a, static_cast<int64_t>(437'500'000));
    CHECK_EQ(parsed("a=430000000,b=440000000,d=x").frequency_a, static_cast<int64_t>(430'000'000));
    CHECK_EQ(parsed("r=145600000,t=145000000,d=x").frequency_a, static_cast<int64_t>(145'600'000));
    CHECK_EQ(parsed("l=145600000,t=145000000,d=x").frequency_a, static_cast<int64_t>(145'600'000));

    /* The confirmation string is trimmed to_string_short_freq. */
    CHECK_STR_EQ(trim(to_string_short_freq(437'500'000)), "437.5000");
}

/* --- Display: what the list widget renders ---------------------------- */

TEST(freqman_app_list_display_formatting) {
    /* pretty_string forms, straight from upstream freqman_db.cpp. */
    CHECK_STR_EQ(core::pretty_string(parsed("f=437500000,d=Description")),
                 " 437.5000M: Description");
    CHECK_STR_EQ(core::pretty_string(parsed("a=430000000,b=440000000,d=Range")),
                 "430.0M-440.0M: Range");
    CHECK_STR_EQ(core::pretty_string(parsed("r=145600000,t=145000000,d=Ham")),
                 "R:145.6M,T:145.0M: Ham");
    CHECK_STR_EQ(core::pretty_string(parsed("l=145600000,t=145000000,d=Repeater")),
                 "L:145.6M,T:145.0M: Repeater");

    /* An unparseable line is shown raw (the list colours it differently). */
    freqman_entry raw;
    raw.type = freqman_type::Raw;
    raw.description = "junk line";
    CHECK_STR_EQ(core::pretty_string(raw), "junk line");

    /* Truncation past the field width ends in '+', the list's overflow marker. */
    CHECK_STR_EQ(core::pretty_string(parsed("f=437500000,d=Description"), 10),
                 " 437.5000+");
}

/* --- Editor: option tables the OptionsFields are built from ------------ */

TEST(freqman_app_editor_modulation_options) {
    /* populate_modulation_options(): {"None",-1} then every modulation by index.
     * Each name must round-trip back to its index, since the value written to
     * the OptionsField IS the index stored in the entry. */
    static_assert(core::freqman_modulation_count == 6, "6 modulations, per upstream");
    for (size_t i = 0; i < core::freqman_modulation_count; ++i) {
        const auto name = core::freqman_entry_get_modulation_string(
            static_cast<core::freqman_index_t>(i));
        CHECK(!name.empty());
        CHECK_EQ(static_cast<size_t>(core::freqman_find_modulation(name)), i);
    }
    /* "None" maps to the invalid index, which the editor stores as -1. */
    CHECK(!core::is_valid(freqman_invalid_index));
}

TEST(freqman_app_editor_bandwidth_options_depend_on_modulation) {
    /* populate_bandwidth_options() lists the current modulation's table. */
    const auto nfm = core::freqman_find_modulation("NFM");
    CHECK(core::is_valid(nfm));
    const size_t n = core::freqman_bandwidth_count(nfm);
    CHECK(n > 0);
    for (size_t i = 0; i < n; ++i) {
        const auto name = core::freqman_entry_get_bandwidth_string(
            nfm, static_cast<core::freqman_index_t>(i));
        CHECK(!name.empty());
        CHECK_EQ(static_cast<size_t>(core::freqman_find_bandwidth(nfm, name)), i);
    }
    /* Concrete anchor from the NFM table. */
    CHECK_STR_EQ(core::freqman_entry_get_bandwidth_string(nfm, 2), "12k5");
}

TEST(freqman_app_editor_step_and_tone_options) {
    /* Step short-names round-trip and carry a positive Hz value. */
    const size_t steps = core::freqman_step_count();
    CHECK(steps > 0);
    for (size_t i = 0; i < steps; ++i) {
        const auto name = core::freqman_entry_get_step_string_short(
            static_cast<core::freqman_index_t>(i));
        CHECK(!name.empty());
        CHECK_EQ(static_cast<size_t>(core::freqman_find_step(name)), i);
        CHECK(core::freqman_entry_get_step_value(static_cast<core::freqman_index_t>(i)) > 0);
    }
    CHECK_EQ(static_cast<int>(core::freqman_find_step("25kHz")), 11);

    /* Tone table: index 9 is CTCSS 88.5 Hz ("8 YB"). */
    CHECK(core::freqman_tone_key_count() > 9);
    CHECK_STR_EQ(core::freqman_tone_key_value_string(9), "88.5");
    CHECK_STR_EQ(core::freqman_tone_key_name(9), "8 YB");
}
