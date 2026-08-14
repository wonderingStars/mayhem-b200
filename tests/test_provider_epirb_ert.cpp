/*
 * mayhem-b200 — tests for the EPIRB RX and ERT web portal panel providers
 * (src/remote/provider_epirb.cpp, src/remote/provider_ert.cpp).
 *
 * Three things are under test and they fail in different places:
 *
 *   1. The cell formatting. Every row asserted here is produced from a real
 *      epirb::Beacon or a real ert::Packet, so the strings the browser gets are
 *      checked against what the app's own on_draw paints on the 240x320 screen
 *      rather than against a hand-written expectation of it.
 *   2. The EPIRB panel's MARKER GATE, which is the part of this file that can
 *      do harm. A distress beacon on a map is the point of the app; a distress
 *      beacon at a position it never transmitted sends a search to the wrong
 *      ocean, so every one of those tests builds a real 144-bit frame, checks
 *      what the decoder made of it, and then asserts what does and does not
 *      reach the map. The frames that must NOT be plotted outnumber the one
 *      that must.
 *   3. That the provider fires at all. That half depends on the app id string
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

using remote::GeoTableMarker;
using remote::TableData;

/* Defined in src/remote/provider_epirb.cpp and src/remote/provider_ert.cpp;
 * see the comments there for why they are not in an anonymous namespace. */
namespace remote {
TableData epirb_table_data(const app::EpirbRecentEntries& entries);
std::vector<GeoTableMarker> epirb_geo_markers(const app::EpirbRecentEntries& entries,
                                              size_t max_markers);
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

/* The two lines EpirbRxView::on_frame_bits() runs to fill its table
 * (src/apps/ui_epirb_rx.cpp), so the CELLS under test are the ones the app
 * would be holding. Note that it deliberately does NOT reproduce what the app
 * does with the position: that is the gate the map rests on, and the tests for
 * it drive the app's own on_frame_bits() rather than a copy of it — a copy
 * would go on passing after the gate was deleted from the app. */
void epirb_record(app::EpirbRecentEntries& entries, const app::epirb::Beacon& beacon) {
    auto& entry = ui::on_packet(entries, beacon.hex_id, 32);
    entry.count++;
    entry.line = beacon.summary();
}

/* The 144 frame bits, MSB first per byte, in the order the demodulator hands
 * them to EpirbRxView::on_frame_bits() (Beacon::set_frame_bits() reassembles
 * exactly this). */
std::vector<uint8_t> epirb_frame_bits(const uint8_t* frame) {
    std::vector<uint8_t> bits;
    bits.reserve(app::epirb::kBeaconDataSize * 8);
    for (size_t i = 0; i < app::epirb::kBeaconDataSize; i++)
        for (int b = 7; b >= 0; b--) bits.push_back((frame[i] >> b) & 1u);
    return bits;
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

/* Opens EPIRB RX and hands back the live view — the same object the provider
 * will find. Every marker test starts here rather than assembling entries by
 * hand, so what it asserts about a position is the app's own decision. */
app::EpirbRxView* open_epirb(ProviderHarness& h) {
    h.launch("epirb_rx");
    for (size_t i = 0; i < h.nav.depth(); i++)
        if (auto* v = dynamic_cast<app::EpirbRxView*>(h.nav.at_depth(i))) return v;
    return nullptr;
}

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

/* --- EPIRB RX: the marker gate ----------------------------------------------
 *
 * One frame in this section earns a marker. Everything else is a way of NOT
 * earning one, because that is where the harm is: a beacon plotted where it is
 * not is worse than a beacon not plotted at all, and the row — locator and all
 * — reaches the operator either way. */

TEST(epirb_panel_plots_a_beacon_whose_frame_carried_a_position) {
    /* The same Standard Location PLB the row tests use: France, 43deg 45' N +
     * a PDF-2 offset of 20 seconds, 1deg 30' E + 8 seconds. Its Maidenhead
     * locator is already pinned above as JN03ss, so the position under test is
     * the one the app itself reports. */
    uint8_t frame[app::epirb::kBeaconDataSize];
    epirb_build_plb_frame(frame);

    app::epirb::Beacon beacon;
    beacon.set_frame(frame);
    CHECK(beacon.frame_valid());
    CHECK(beacon.location.is_valid());

    ProviderHarness h;
    app::EpirbRxView* view = open_epirb(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;
    view->on_frame_bits(epirb_frame_bits(frame));

    const auto markers = remote::epirb_geo_markers(view->entries(), 200);
    CHECK_EQ(markers.size(), size_t{1});
    if (markers.empty()) return;

    /* 43 + 45/60 + 20/3600 and 1 + 30/60 + 8/3600, which is what the app's own
     * Angle conversion produces from those fields. */
    CHECK_NEAR(markers[0].lat, 43.755555, 1e-5);
    CHECK_NEAR(markers[0].lon, 1.502222, 1e-5);
    /* The entry's key, whose first four characters open the table's own cell
     * ("1C6E-PLB-JN03ss[OK]"), so the marker and its row name one beacon. */
    CHECK_STR_EQ(markers[0].label, "1C6E3DA3AEFFBFF");
    CHECK_STR_EQ(markers[0].kind, "beacon");
    /* A 406 MHz frame carries no course. Absent, not 0 — which map.js would
     * draw as an arrow pointing due north. */
    CHECK(!markers[0].heading_deg.has_value());
}

TEST(epirb_panel_keeps_a_beacon_with_no_encoded_position_off_the_map) {
    /* The longitude sentinel, the same frame the blank-locator row test uses.
     * Several protocols carry no position by design; this is what they look
     * like, and the beacon belongs in the table and nowhere else. */
    uint8_t frame[app::epirb::kBeaconDataSize];
    epirb_build_plb_frame(frame);
    epirb_set_bits(frame, 76, 83, 255);
    epirb_finish_frame(frame);

    app::epirb::Beacon beacon;
    beacon.set_frame(frame);
    CHECK(beacon.frame_valid());
    CHECK(beacon.location.is_unknown());
    CHECK(!beacon.location.is_valid());

    ProviderHarness h;
    app::EpirbRxView* view = open_epirb(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;
    view->on_frame_bits(epirb_frame_bits(frame));

    /* Still in the table... */
    CHECK_EQ(remote::epirb_table_data(view->entries()).rows.size(), size_t{1});
    /* ...and not on the map, and emphatically not at 0N 0E. */
    CHECK_EQ(remote::epirb_geo_markers(view->entries(), 200).size(), size_t{0});
    CHECK(!view->entries().front().has_position);
}

TEST(epirb_panel_refuses_the_latitude_sentinel_that_is_unknown_cannot_see) {
    /* THE trap this gate exists for. Latitude is a seven-bit field in every
     * protocol, so its "not available" default is 127 and never the 255
     * Location::is_unknown() looks for — a frame carrying it alongside a
     * plausible longitude passes upstream's test and decodes as 127 degrees
     * north. On the device that is a garbage detail line; on a map it is a
     * fabricated distress position. */
    uint8_t frame[app::epirb::kBeaconDataSize];
    epirb_build_plb_frame(frame);
    epirb_set_bits(frame, 66, 72, 127); /* latitude degrees, standard location */
    epirb_finish_frame(frame);

    app::epirb::Beacon beacon;
    beacon.set_frame(frame);
    CHECK(beacon.frame_valid());
    /* The trap is real: the app's older test says this beacon has a position. */
    CHECK(!beacon.location.is_unknown());
    CHECK_EQ(beacon.location.latitude.degrees, 127L);
    /* And the gate that matters says it does not. */
    CHECK(!beacon.location.is_valid());

    ProviderHarness h;
    app::EpirbRxView* view = open_epirb(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;
    view->on_frame_bits(epirb_frame_bits(frame));

    CHECK_EQ(remote::epirb_geo_markers(view->entries(), 200).size(), size_t{0});
}

TEST(epirb_panel_refuses_a_latitude_no_beacon_can_be_at) {
    /* Not a sentinel — 100 is simply a number the field can hold — so this is
     * the range check on its own, which is also what catches a PDF-2 offset
     * that borrowed an angle out of range. */
    uint8_t frame[app::epirb::kBeaconDataSize];
    epirb_build_plb_frame(frame);
    epirb_set_bits(frame, 66, 72, 100);
    epirb_finish_frame(frame);

    app::epirb::Beacon beacon;
    beacon.set_frame(frame);
    CHECK(beacon.frame_valid());
    CHECK(!beacon.location.is_unknown());
    CHECK_EQ(beacon.location.latitude.degrees, 100L);
    CHECK(!beacon.location.is_valid());

    ProviderHarness h;
    app::EpirbRxView* view = open_epirb(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;
    view->on_frame_bits(epirb_frame_bits(frame));

    CHECK_EQ(remote::epirb_geo_markers(view->entries(), 200).size(), size_t{0});
}

TEST(epirb_panel_refuses_the_exact_origin) {
    /* Every position field and both PDF-2 offsets zeroed. C/S encodes "not
     * available" as all-ones and never as zero, so this decodes as a genuine
     * 0N 0E — which the decoder is free to report (is_valid() is about the
     * protocol, and the beacon really did send zeros) and which the provider
     * refuses anyway. 700 km south of Accra is where a field read as zeros
     * lands, not where an emergency beacon is. */
    uint8_t frame[app::epirb::kBeaconDataSize];
    epirb_build_plb_frame(frame);
    epirb_set_bits(frame, 65, 65, 0);   /* N */
    epirb_set_bits(frame, 66, 72, 0);   /* 0 deg */
    epirb_set_bits(frame, 73, 74, 0);   /* 0' */
    epirb_set_bits(frame, 75, 75, 0);   /* E */
    epirb_set_bits(frame, 76, 83, 0);   /* 0 deg */
    epirb_set_bits(frame, 84, 85, 0);   /* 0' */
    epirb_set_bits(frame, 119, 122, 0); /* no latitude seconds offset */
    epirb_set_bits(frame, 129, 132, 0); /* no longitude seconds offset */
    epirb_finish_frame(frame);

    app::epirb::Beacon beacon;
    beacon.set_frame(frame);
    CHECK(beacon.frame_valid());
    /* The decoder says this is a position, and it is right to. */
    CHECK(beacon.location.is_valid());
    CHECK_NEAR(beacon.location.latitude.degrees_decimal(), 0.0, 1e-6);
    CHECK_NEAR(beacon.location.longitude.degrees_decimal(), 0.0, 1e-6);

    ProviderHarness h;
    app::EpirbRxView* view = open_epirb(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;
    view->on_frame_bits(epirb_frame_bits(frame));

    /* So the entry carries it — the layer above is what withholds the marker. */
    CHECK(view->entries().front().has_position);
    CHECK_EQ(remote::epirb_table_data(view->entries()).rows.size(), size_t{1});
    CHECK_EQ(remote::epirb_geo_markers(view->entries(), 200).size(), size_t{0});
}

TEST(epirb_panel_will_not_plot_a_position_out_of_a_frame_that_failed_its_bch) {
    /* The position bits (41..85, and the offsets in 107..132) are exactly what
     * BCH-1 and BCH-2 protect, so a frame that fails them has an unverified
     * position — and the app shows it as KO. Plotting it would put a search on
     * a coordinate no beacon transmitted. */
    uint8_t frame[app::epirb::kBeaconDataSize];
    epirb_build_plb_frame(frame);
    /* TWO flipped bits inside the latitude field, and the BCHs are not
     * recomputed. One would not do it: Beacon::simple_correction() walks bits
     * 25..85 looking for the single flip that satisfies BCH-1 and accepts the
     * frame with bch1_corrected set — the app's own decision, and one this gate
     * deliberately honours (a corrected frame is a verified frame). Two errors
     * are past what a 21-bit BCH can repair. */
    frame[8] ^= 0x09;

    app::epirb::Beacon beacon;
    beacon.set_frame(frame);
    CHECK(!beacon.frame_valid());
    /* The location itself still parses — this is the BCH gate doing the work,
     * not the sentinel one. */
    CHECK(beacon.location.is_valid());

    ProviderHarness h;
    app::EpirbRxView* view = open_epirb(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;
    view->on_frame_bits(epirb_frame_bits(frame));

    CHECK(!view->entries().front().has_position);
    /* The operator still sees the beacon, marked KO, exactly as on the device. */
    const TableData t = remote::epirb_table_data(view->entries());
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK(t.rows[0][0].find("[KO]") != std::string::npos);
    CHECK_EQ(remote::epirb_geo_markers(view->entries(), 200).size(), size_t{0});
}

TEST(epirb_panel_does_not_withdraw_a_verified_position_on_a_later_positionless_frame) {
    /* Same beacon, two bursts: one that verified a position and one that
     * carried none. The hex ID masks the position bits out, so both land on one
     * entry — and the marker has to survive. Withdrawing a distress position
     * because a later burst was corrupt or came from a protocol that omits it
     * is exactly the wrong failure direction. */
    uint8_t with_pos[app::epirb::kBeaconDataSize];
    epirb_build_plb_frame(with_pos);

    uint8_t no_pos[app::epirb::kBeaconDataSize];
    epirb_build_plb_frame(no_pos);
    epirb_set_bits(no_pos, 76, 83, 255);
    epirb_finish_frame(no_pos);

    app::epirb::Beacon a;
    a.set_frame(with_pos);
    app::epirb::Beacon b;
    b.set_frame(no_pos);
    CHECK_STR_EQ(a.hex_id, b.hex_id); /* one entry, or this test proves nothing */

    ProviderHarness h;
    app::EpirbRxView* view = open_epirb(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;
    view->on_frame_bits(epirb_frame_bits(with_pos));
    view->on_frame_bits(epirb_frame_bits(no_pos));

    const TableData t = remote::epirb_table_data(view->entries());
    CHECK_EQ(t.rows.size(), size_t{1});
    /* The newer frame's line wins, as the app writes it... */
    CHECK_STR_EQ(t.rows[0][0], "1C6E-PLB-      [OK]");
    /* ...while the position that verified once stays on the map. */
    const auto markers = remote::epirb_geo_markers(view->entries(), 200);
    CHECK_EQ(markers.size(), size_t{1});
    if (!markers.empty()) CHECK_NEAR(markers[0].lat, 43.755555, 1e-5);
}

TEST(epirb_panel_publishes_no_markers_when_nothing_has_been_heard) {
    app::EpirbRecentEntries entries;
    CHECK_EQ(remote::epirb_geo_markers(entries, 200).size(), size_t{0});

    /* And an entry nothing filled in is not a position either. */
    entries.emplace_back(app::EpirbRecentEntry::Key{"1C6E3DA3AEFFBFF"});
    CHECK_EQ(remote::epirb_geo_markers(entries, 200).size(), size_t{0});
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

TEST(epirb_panel_provider_publishes_a_geotable_when_the_app_is_open) {
    ProviderHarness h;
    /* The id has to be the one src/apps/ui_epirb_rx.cpp registers, or the
     * bridge never reaches the provider at all and the portal silently keeps
     * showing the placeholder card. */
    h.launch("epirb_rx");
    CHECK_EQ(h.nav.depth(), size_t{2});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(json_has(panel, "\"app_id\":\"epirb_rx\""));
    /* Upgraded from "table" to "geotable": the table half is unchanged and the
     * beacon markers are a strict addition beside it, so the columns and rows
     * assertions below still hold verbatim. */
    CHECK(json_has(panel, "\"panel_kind\":\"geotable\""));
    CHECK(json_has(panel, "\"columns\":[\"Beacon\"]"));
    /* No device, so no beacons: an empty rows array, not a fabricated row... */
    CHECK(json_has(panel, "\"rows\":[]"));
    /* ...and an empty markers array, which is the same claim about the map. */
    CHECK(json_has(panel, "\"map\":{\"markers\":[]}"));
}

TEST(epirb_panel_provider_publishes_a_decoded_beacon_through_the_bridge) {
    ProviderHarness h;
    app::EpirbRxView* view = open_epirb(h);
    CHECK(view != nullptr);
    if (view == nullptr) return;

    uint8_t frame[app::epirb::kBeaconDataSize];
    epirb_build_plb_frame(frame);
    view->on_frame_bits(epirb_frame_bits(frame));

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    /* The whole hop, not just the adapter: serialized under the keys PANELS.md's
     * geotable contract names, and reachable at GET /api/panel. */
    CHECK(json_has(panel, "\"panel_kind\":\"geotable\""));
    CHECK(json_has(panel, "1C6E-PLB-JN03ss[OK]"));
    CHECK(json_has(panel, "\"kind\":\"beacon\""));
    CHECK(json_has(panel, "\"lat\":43.75555"));
    CHECK(!json_has(panel, "\"markers\":[]"));
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

    CHECK(json_has(panel, "\"panel_kind\":\"geotable\""));
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
