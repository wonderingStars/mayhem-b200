/*
 * mayhem-b200 — tests for the EPIRB RX and ERT web portal panel providers
 * (src/remote/provider_epirb.cpp, src/remote/provider_ert.cpp).
 *
 * Two things are under test and they fail in different places:
 *
 *   1. The cell formatting. Every row asserted here is produced from a real
 *      epirb::Beacon or a real ert::Packet, so the strings the browser gets are
 *      checked against what the app's own on_draw paints on the 240x320 screen
 *      rather than against a hand-written expectation of it.
 *   2. That the provider fires at all. That half depends on the app id string
 *      matching the one src/apps/ui_*.cpp registers, and a wrong id fails
 *      silently — the portal just keeps showing the placeholder card. It is
 *      only reachable through AppBridge end to end: request_launch(), drain,
 *      NavigationView::service(), refresh(), panel_json().
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_context.hpp"
#include "audio_out.hpp"
#include "receiver_model.hpp"
#include "remote/app_bridge.hpp"
#include "remote/app_data.hpp"
#include "ui_epirb_rx.hpp"
#include "ui_ert.hpp"
#include "ui_navigation.hpp"
#include "ui_recent_entries.hpp"
#include "usrp_radio.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using remote::TableData;

/* Defined in src/remote/provider_epirb.cpp and src/remote/provider_ert.cpp;
 * see the comments there for why they are not in an anonymous namespace. */
namespace remote {
TableData epirb_table_data(const app::EpirbRecentEntries& entries);
TableData ert_table_data(const app::ert::RecentEntries& entries);
}  // namespace remote

namespace {

/* Crude but sufficient: the panel bodies asserted on here are small and their
 * key order is fixed by AppBridge::panel_json(). */
bool json_has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/* --- EPIRB frame construction ---------------------------------------------
 *
 * Same shape as tests/test_epirb_rx.cpp's builder (a Standard Location PLB,
 * France, 43deg 45' 20" N / 1deg 30' 08" E), rebuilt here because that file's
 * helpers are static to its own translation unit. The BCH fields are filled by
 * a plain LFSR written from the generator polynomials, so this shares no code
 * with Beacon::compute_bch(). */

bool epirb_frame_bit(const uint8_t* frame, int bit) {
    return ((frame[(bit - 1) / 8] >> (7 - ((bit - 1) % 8))) & 1) != 0;
}

void epirb_set_bits(uint8_t* frame, int start, int end, uint64_t value) {
    const int n = end - start + 1;
    for (int i = 0; i < n; i++) {
        const int bit = start + i;
        const int byte = (bit - 1) / 8;
        const int off = 7 - ((bit - 1) % 8);
        if (((value >> (n - 1 - i)) & 1ULL) != 0)
            frame[byte] = static_cast<uint8_t>(frame[byte] | (1u << off));
        else
            frame[byte] = static_cast<uint8_t>(frame[byte] & ~(1u << off));
    }
}

uint32_t epirb_bch(const uint8_t* frame, int start, int end, uint32_t gen, int gen_bits) {
    const int deg = gen_bits - 1;
    const uint32_t mask = (1u << deg) - 1u;
    const uint32_t glow = gen & mask;
    uint32_t reg = 0;
    for (int b = start; b <= end; b++) {
        const bool feedback = (((reg >> (deg - 1)) & 1u) != 0) ^ epirb_frame_bit(frame, b);
        reg = (reg << 1) & mask;
        if (feedback) reg ^= glow;
    }
    return reg;
}

void epirb_finish_frame(uint8_t* frame) {
    epirb_set_bits(frame, 1, 24, app::epirb::kRealPreamble);
    epirb_set_bits(frame, 86, 106,
                   epirb_bch(frame, 25, 85, app::epirb::kBch21Polynomial,
                             app::epirb::kBch21PolyLength));
    epirb_set_bits(frame, 133, 144,
                   epirb_bch(frame, 107, 132, app::epirb::kBch12Polynomial,
                             app::epirb::kBch12PolyLength));
}

void epirb_build_plb_frame(uint8_t* frame) {
    for (size_t i = 0; i < app::epirb::kBeaconDataSize; i++) frame[i] = 0;

    epirb_set_bits(frame, 25, 25, 1);      /* long frame */
    epirb_set_bits(frame, 26, 26, 0);      /* location protocol */
    epirb_set_bits(frame, 27, 36, 227);    /* France */
    epirb_set_bits(frame, 37, 40, 0b0111); /* PLB serial location */
    epirb_set_bits(frame, 41, 50, 123);    /* C/S type approval */
    epirb_set_bits(frame, 51, 64, 4567);   /* serial */

    epirb_set_bits(frame, 65, 65, 0);      /* North */
    epirb_set_bits(frame, 66, 72, 43);     /* 43 deg */
    epirb_set_bits(frame, 73, 74, 3);      /* 45' */
    epirb_set_bits(frame, 75, 75, 0);      /* East */
    epirb_set_bits(frame, 76, 83, 1);      /* 1 deg */
    epirb_set_bits(frame, 84, 85, 2);      /* 30' */

    epirb_set_bits(frame, 107, 110, 0b1101);
    epirb_set_bits(frame, 111, 111, 1);
    epirb_set_bits(frame, 112, 112, 1);
    epirb_set_bits(frame, 113, 113, 1);
    epirb_set_bits(frame, 114, 118, 0);
    epirb_set_bits(frame, 119, 122, 5);    /* +20 s latitude offset */
    epirb_set_bits(frame, 123, 123, 1);
    epirb_set_bits(frame, 124, 128, 0);
    epirb_set_bits(frame, 129, 132, 2);    /* +8 s longitude offset */

    epirb_finish_frame(frame);
}

/* The three lines EpirbRxView::on_frame_bits() runs once a frame decodes
 * (src/apps/ui_epirb_rx.cpp), so the entries under test are the ones the app
 * would actually be holding. */
void epirb_record(app::EpirbRecentEntries& entries, const app::epirb::Beacon& beacon) {
    auto& entry = ui::on_packet(entries, beacon.hex_id, 32);
    entry.count++;
    entry.line = beacon.summary();
}

/* --- ERT packet construction ----------------------------------------------- */

void ert_set_field(std::vector<uint8_t>& bits, size_t start, size_t length, uint32_t value) {
    for (size_t i = 0; i < length; i++) {
        const uint32_t b = (value >> (length - 1 - i)) & 1u;
        if ((start + i) < bits.size()) bits[start + i] = static_cast<uint8_t>(b);
    }
}

/* An SCM payload: 75 data bits, the field offsets ert::Packet reads. */
app::ert::Packet make_scm(uint32_t id, uint32_t commodity, uint32_t consumption,
                          uint32_t physical_tamper, uint32_t encoder_tamper) {
    std::vector<uint8_t> bits(75, 0);
    ert_set_field(bits, 0, 2, id >> 24);
    ert_set_field(bits, 35, 24, id & 0xFFFFFFu);
    ert_set_field(bits, 3, 2, physical_tamper);
    ert_set_field(bits, 5, 4, commodity);
    ert_set_field(bits, 9, 2, encoder_tamper);
    ert_set_field(bits, 11, 24, consumption);
    return app::ert::Packet::from_bits(app::ert::PacketType::SCM, std::move(bits));
}

/* An SCM+ payload: 14 bytes, byte-aligned fields. */
app::ert::Packet make_scmplus(uint32_t id, uint32_t commodity, uint32_t consumption,
                              uint32_t tamper) {
    std::vector<uint8_t> bits(14 * 8, 0);
    ert_set_field(bits, 1 * 8 + 4, 4, commodity);
    ert_set_field(bits, 2 * 8, 32, id);
    ert_set_field(bits, 6 * 8, 32, consumption);
    ert_set_field(bits, 10 * 8, 16, tamper);
    return app::ert::Packet::from_bits(app::ert::PacketType::SCMPLUS, std::move(bits));
}

/* ErtView::on_frame()'s two lines (src/apps/ui_ert.cpp), so the entries under
 * test are keyed and filled exactly as the app keys and fills them. */
void ert_record(app::ert::RecentEntries& entries, const app::ert::Packet& packet) {
    auto& entry = ui::on_packet(entries, app::ert::Key{packet.id(), packet.commodity_type()});
    entry.update(packet);
}

/* --- End-to-end harness -----------------------------------------------------
 *
 * EpirbRxView's constructor dereferences globals().receiver, so a
 * ReceiverModel has to exist before the app registry's factory can build one.
 * Nothing here opens a device: on_show() calls receiver.start(), which fails at
 * start_rx() on a closed radio and returns false without spawning a DSP thread,
 * so both views come up with an empty list — which is exactly the state under
 * test. Tears the globals back down so later tests see them as they were. */
struct ProviderHarness {
    radio::UsrpRadio radio{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{radio, audio};
    ui::NavigationView nav{{0, 0, 240, 304}};

    radio::RadioDevice* saved_radio{app::globals().radio};
    radio::ReceiverModel* saved_receiver{app::globals().receiver};
    ui::NavigationView* saved_nav{app::globals().nav};

    ProviderHarness() {
        app::globals().radio = &radio;
        app::globals().receiver = &receiver;
        app::globals().nav = &nav;

        nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304})); /* root */
        nav.service();
    }

    ~ProviderHarness() {
        /* AppBridge is a process-global singleton, so a test that leaves it
         * believing epirb_rx or ert is open hands that state to every later
         * test in the binary — and to every -count= rerun. Clearing it needs
         * the nav still wired up, so it happens before the globals go back. */
        remote::AppBridge::instance().request_home();
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();

        app::globals().radio = saved_radio;
        app::globals().receiver = saved_receiver;
        app::globals().nav = saved_nav;
    }

    /* Puts an app on the stack the way the portal does. */
    void launch(const std::string& app_id) {
        remote::AppBridge::instance().request_launch(app_id);
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();
    }
};

}  // namespace

/* --- EPIRB RX: columns and cell formatting -------------------------------- */

TEST(epirb_panel_publishes_the_column_the_app_shows) {
    /* Name and count are EpirbRxView::columns_ (src/apps/ui_epirb_rx.hpp):
     * a single free-form line per beacon, no per-field columns. */
    app::EpirbRecentEntries entries;
    const TableData t = remote::epirb_table_data(entries);

    CHECK_EQ(t.columns.size(), size_t{1});
    CHECK_STR_EQ(t.columns[0], "Beacon");
}

TEST(epirb_panel_with_no_beacons_heard_yields_no_rows) {
    app::EpirbRecentEntries entries;
    const TableData t = remote::epirb_table_data(entries);

    CHECK_EQ(t.rows.size(), size_t{0});
    /* The columns still go out, so the browser draws an empty table rather
     * than nothing at all. */
    CHECK_EQ(t.columns.size(), size_t{1});
}

TEST(epirb_panel_row_matches_the_line_the_app_draws) {
    uint8_t frame[app::epirb::kBeaconDataSize];
    epirb_build_plb_frame(frame);

    app::epirb::Beacon beacon;
    beacon.set_frame(frame);
    CHECK(beacon.frame_valid());

    app::EpirbRecentEntries entries;
    epirb_record(entries, beacon);

    const TableData t = remote::epirb_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_EQ(t.rows[0].size(), size_t{1});

    /* Beacon::summary(): the first four hex-ID characters, the beacon type, the
     * Maidenhead locator and the frame's OK/KO verdict. Byte for byte what the
     * device paints, less the STR_COLOR_* escapes around the verdict. */
    CHECK_STR_EQ(t.rows[0][0], "1C6E-PLB-JN03ss[OK]");
}

TEST(epirb_panel_cells_carry_no_terminal_colour_escapes) {
    uint8_t frame[app::epirb::kBeaconDataSize];
    epirb_build_plb_frame(frame);

    app::epirb::Beacon beacon;
    beacon.set_frame(frame);

    app::EpirbRecentEntries entries;
    epirb_record(entries, beacon);

    /* The app's own line does carry them — that is what makes the strip in the
     * provider load-bearing rather than defensive. */
    CHECK(entries.front().line.find('\x1B') != std::string::npos);

    const TableData t = remote::epirb_table_data(entries);
    CHECK(t.rows[0][0].find('\x1B') == std::string::npos);
    /* And nothing else of the escape survives either: the palette byte that
     * follows 0x1B is 0x0A/0x0C/0x0E/0x0F, all of which would show up as
     * control characters in the JSON. */
    for (char c : t.rows[0][0]) CHECK(static_cast<unsigned char>(c) >= 0x20);
}

TEST(epirb_panel_leaves_a_beacon_with_no_position_blank) {
    /* Longitude degrees of 255 is C/S's "no position encoded" default, and
     * Location::is_unknown() reads it as such. The app then draws six blanks
     * where the locator goes; a beacon heard without a fix must not appear at
     * a fabricated grid square (and emphatically not at AA00aa / 0N 0E). */
    uint8_t frame[app::epirb::kBeaconDataSize];
    epirb_build_plb_frame(frame);
    epirb_set_bits(frame, 76, 83, 255);
    epirb_finish_frame(frame);

    app::epirb::Beacon beacon;
    beacon.set_frame(frame);
    CHECK(beacon.frame_valid());
    CHECK(beacon.location.is_unknown());

    app::EpirbRecentEntries entries;
    epirb_record(entries, beacon);

    const TableData t = remote::epirb_table_data(entries);
    CHECK_STR_EQ(t.rows[0][0], "1C6E-PLB-      [OK]");
}

TEST(epirb_panel_emits_an_empty_cell_for_an_entry_nothing_has_filled_in) {
    /* An entry whose key exists but whose summary line was never written. The
     * cell has to be empty rather than any stand-in that reads like a decode. */
    app::EpirbRecentEntries entries;
    entries.emplace_back(app::EpirbRecentEntry::Key{"1C6E3DA3AEFFBFF"});

    const TableData t = remote::epirb_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "");
}

/* --- ERT: columns and cell formatting -------------------------------------- */

TEST(ert_panel_publishes_the_columns_the_app_shows) {
    /* Names and order are ErtView::columns_ (src/apps/ui_ert.hpp). */
    app::ert::RecentEntries entries;
    const TableData t = remote::ert_table_data(entries);

    CHECK_EQ(t.columns.size(), size_t{5});
    CHECK_STR_EQ(t.columns[0], "ID");
    CHECK_STR_EQ(t.columns[1], "Ty");
    CHECK_STR_EQ(t.columns[2], "Consumpt");
    CHECK_STR_EQ(t.columns[3], "Tamp");
    CHECK_STR_EQ(t.columns[4], "Ct");
}

TEST(ert_panel_with_no_meters_heard_yields_no_rows) {
    app::ert::RecentEntries entries;
    const TableData t = remote::ert_table_data(entries);

    CHECK_EQ(t.rows.size(), size_t{0});
    CHECK_EQ(t.columns.size(), size_t{5});
}

TEST(ert_panel_row_matches_the_line_the_app_draws_for_an_scm_meter) {
    app::ert::RecentEntries entries;
    const app::ert::Packet p = make_scm(12345678, 7, 1234, 2, 1);

    /* The packet has to decode the way the app reads it, or the row below is
     * asserting against the test's own arithmetic rather than the app's. */
    CHECK_EQ(p.id(), app::ert::ID{12345678});
    CHECK_EQ(p.commodity_type(), app::ert::CommodityType{7});
    CHECK_EQ(p.consumption(), app::ert::Consumption{1234});

    ert_record(entries, p);

    const TableData t = remote::ert_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_EQ(t.rows[0].size(), size_t{5});

    CHECK_STR_EQ(t.rows[0][0], "12345678");
    CHECK_STR_EQ(t.rows[0][1], "7");
    CHECK_STR_EQ(t.rows[0][2], "1234");
    /* SCM splits the tamper count: physical/encoder, as
     * ert::format_tamper_flags_scm() renders it. */
    CHECK_STR_EQ(t.rows[0][3], "2/1");
    CHECK_STR_EQ(t.rows[0][4], "1");
}

TEST(ert_panel_keeps_the_apps_hex_tamper_word_for_a_non_scm_meter) {
    app::ert::RecentEntries entries;
    ert_record(entries, make_scmplus(4001234567u, 12, 98765, 0x00A5));

    const TableData t = remote::ert_table_data(entries);
    CHECK_STR_EQ(t.rows[0][0], "4001234567");
    CHECK_STR_EQ(t.rows[0][1], "12");
    CHECK_STR_EQ(t.rows[0][2], "98765");
    /* Four hex digits, leading zeros and all: that padding is how the app
     * renders a 16-bit word, not screen alignment, so it stays on the wire. */
    CHECK_STR_EQ(t.rows[0][3], "00A5");
    CHECK_STR_EQ(t.rows[0][4], "1");
}

TEST(ert_panel_counts_receptions_and_keeps_the_apps_own_ceiling) {
    /* ErtView's on_draw shows "++" past 99 receptions rather than a wider
     * number; that is a real limit on what the app will show, not alignment. */
    app::ert::RecentEntries entries;
    const app::ert::Packet p = make_scm(42, 4, 7, 0, 0);

    for (int i = 0; i < 99; i++) ert_record(entries, p);
    CHECK_STR_EQ(remote::ert_table_data(entries).rows[0][4], "99");

    ert_record(entries, p);
    CHECK_STR_EQ(remote::ert_table_data(entries).rows[0][4], "++");
}

TEST(ert_panel_emits_empty_cells_for_a_meter_no_packet_has_filled_in) {
    /* A keyed entry that no packet has updated: its consumption and tamper
     * fields are default-constructed zeros, not readings, and publishing them
     * would show a meter reading of 0 and a clean tamper word for a meter
     * nothing has been heard from. */
    app::ert::RecentEntries entries;
    entries.emplace_back(app::ert::Key{555, 7});

    const TableData t = remote::ert_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "555");
    CHECK_STR_EQ(t.rows[0][1], "7");
    CHECK_STR_EQ(t.rows[0][2], "");
    CHECK_STR_EQ(t.rows[0][3], "");
    /* The count is a genuine counter and a real zero, so it is published. */
    CHECK_STR_EQ(t.rows[0][4], "0");
}

/* --- The providers themselves, end to end ---------------------------------- */

TEST(epirb_panel_provider_publishes_a_table_when_the_app_is_open) {
    ProviderHarness h;
    /* The id has to be the one src/apps/ui_epirb_rx.cpp registers, or the
     * bridge never reaches the provider at all and the portal silently keeps
     * showing the placeholder card. */
    h.launch("epirb_rx");
    CHECK_EQ(h.nav.depth(), size_t{2});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(json_has(panel, "\"app_id\":\"epirb_rx\""));
    CHECK(json_has(panel, "\"panel_kind\":\"table\""));
    CHECK(json_has(panel, "\"columns\":[\"Beacon\"]"));
    /* No device, so no beacons: an empty rows array, not a fabricated row. */
    CHECK(json_has(panel, "\"rows\":[]"));
}

TEST(epirb_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    ProviderHarness h;
    h.launch("epirb_rx");

    /* Anything the operator pushes on the device — a step menu, a file picker
     * — reproduces the condition: the provider is handed that view, and
     * EpirbRxView is only reachable by walking the stack down. */
    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(json_has(panel, "\"panel_kind\":\"table\""));
    CHECK(json_has(panel, "\"columns\":[\"Beacon\"]"));
}

TEST(epirb_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    ProviderHarness h;
    h.launch("epirb_rx");

    /* Popped on the device rather than through request_home() -- the path a
     * remote key press takes, which never goes near the launch queue.
     * AppBridge::refresh() derives the current app from the navigation stack,
     * so with the view gone the truthful answer is Home: not this app's data,
     * and not this app's name over an empty version of it. Before that
     * derivation the bridge went on believing the app was current and the
     * provider's own "... is not the open app." guard is what answered here.
     * That guard is still in the provider; the bridge simply no longer asks a
     * provider about an app that is not on the stack. */
    h.nav.pop_to_root();
    h.nav.service();

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(json_has(panel, "\"panel_kind\":\"screen\""));
    CHECK(json_has(panel, "Home -- no app is open."));
    /* The stale id is what this change fixed; pin it, not just the text. */
    CHECK(json_has(panel, "\"app_id\":\"\""));
    /* Emphatically not an empty table, which would be indistinguishable from
     * a running receiver that has heard nothing. */
    CHECK(!json_has(panel, "\"columns\""));
}

TEST(ert_panel_provider_publishes_a_table_when_the_app_is_open) {
    ProviderHarness h;
    /* The id has to be the one src/apps/ui_ert.cpp registers. */
    h.launch("ert");
    CHECK_EQ(h.nav.depth(), size_t{2});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(json_has(panel, "\"app_id\":\"ert\""));
    CHECK(json_has(panel, "\"panel_kind\":\"table\""));
    CHECK(json_has(panel, "\"columns\":[\"ID\",\"Ty\",\"Consumpt\",\"Tamp\",\"Ct\"]"));
    CHECK(json_has(panel, "\"rows\":[]"));
}

TEST(ert_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    ProviderHarness h;
    h.launch("ert");

    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(json_has(panel, "\"panel_kind\":\"table\""));
    CHECK(json_has(panel, "\"columns\":[\"ID\",\"Ty\",\"Consumpt\",\"Tamp\",\"Ct\"]"));
}

TEST(ert_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    ProviderHarness h;
    h.launch("ert");

    h.nav.pop_to_root();
    h.nav.service();

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(json_has(panel, "\"panel_kind\":\"screen\""));
    CHECK(json_has(panel, "Home -- no app is open."));
    /* The stale id is what this change fixed; pin it, not just the text. */
    CHECK(json_has(panel, "\"app_id\":\"\""));
    CHECK(!json_has(panel, "\"columns\""));
}
