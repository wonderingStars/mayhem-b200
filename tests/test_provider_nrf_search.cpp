/*
 * mayhem-b200 — tests for the web portal panel providers of NRF RX and Search
 * (src/remote/provider_nrf_rx.cpp, src/remote/provider_search.cpp).
 *
 * Two halves, as with the APRS provider:
 *
 *   - The table adapters (remote::nrf_table_data / remote::search_table_data)
 *     are driven directly with entries built exactly as the apps build them,
 *     which pins the published columns and the per-cell formatting against the
 *     line each app's own on_draw paints.
 *   - The providers themselves are driven end to end through AppBridge —
 *     request_launch(), drain_launch_queue(), NavigationView::service(),
 *     refresh(), panel_json(). That is the only path that sets the bridge's
 *     current app id, it is the path the HTTP handler reads, and it is the one
 *     that catches a wrong app id: with the wrong id the provider is never
 *     reached, the panel comes back as "screen", and the portal silently keeps
 *     showing its placeholder card.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_context.hpp"
#include "audio_out.hpp"
#include "receiver_model.hpp"
#include "remote/app_bridge.hpp"
#include "string_format.hpp"
#include "remote/app_data.hpp"
#include "ui.hpp"
#include "ui_navigation.hpp"
#include "ui_nrf_rx.hpp"
#include "ui_recent_entries.hpp"
#include "ui_search.hpp"
#include "usrp_radio.hpp"

#include <cstdint>
#include <memory>
#include <string>

using remote::TableData;

/* Defined in the two provider .cpp files; see the comments there for why they
 * are exposed rather than left file-static. */
namespace remote {
TableData nrf_table_data(const app::NrfRecentEntries& entries);
TableData search_table_data(const app::SearchRecentEntries& entries);
}  // namespace remote

namespace {

/* --- NRF RX fixtures -------------------------------------------------------
 *
 * NrfRxView::on_packet() is private, so the entry it would build is built here
 * instead, field for field as ui_nrf_rx.cpp does it:
 *
 *     auto& entry = ui::on_packet(entries_, packet.address, 32);
 *     entry.count++;
 *     entry.payload_length = packet.payload_length;
 *     entry.pid            = packet.pid;
 *     entry.payload_hex    = packet.payload_string();
 *
 * The Payload cell therefore comes from the app's own formatter
 * (nrf::Packet::payload_string()), which is the part the provider has to keep
 * agreeing with. That the decoder produces such a Packet from real GFSK in the
 * first place is tests/test_nrf_rx.cpp's job, not this file's. */
app::nrf::Packet make_nrf_packet(uint64_t address, const uint8_t* payload,
                                 uint8_t length, uint8_t pid) {
    app::nrf::Packet p;
    p.address = address;
    p.payload_length = length;
    p.pid = pid;
    for (uint8_t i = 0; i < length; i++) p.payload[i] = payload[i];
    return p;
}

void feed_nrf(app::NrfRecentEntries& entries, const app::nrf::Packet& packet) {
    auto& entry = ui::on_packet(entries, packet.address, 32);
    entry.count++;
    entry.payload_length = packet.payload_length;
    entry.pid = packet.pid;
    entry.payload_hex = packet.payload_string();
}

/* The nRF24L01+ power-on default pipe address and an obvious payload, matching
 * tests/test_nrf_rx.cpp so the two files describe the same frame. */
constexpr uint64_t kNrfAddress = 0xE7E7E7E7E7ULL;
constexpr uint8_t kNrfPayload[4] = {0xDE, 0xAD, 0xBE, 0xEF};

/* --- Search fixtures -------------------------------------------------------
 *
 * SearchView::do_detection() is private; its two table-touching branches are
 * reproduced here exactly (ui_search.cpp):
 *
 *     Lock:    auto& entry = ui::on_packet(recent_, frequency);
 *              entry.set_time(now_hms());
 *     Release: auto& entry = ui::on_packet(recent_, frequency);
 *              entry.set_duration(outcome.duration);
 *
 * so `time` is an "HH:MM:SS" string from the host clock and `duration` is in
 * 100 ms units. */
void feed_search_lock(app::SearchRecentEntries& entries, uint64_t frequency,
                      const std::string& hms) {
    auto& entry = ui::on_packet(entries, frequency);
    entry.set_time(hms);
}

void feed_search_release(app::SearchRecentEntries& entries, uint64_t frequency,
                         uint32_t duration) {
    auto& entry = ui::on_packet(entries, frequency);
    entry.set_duration(duration);
}

}  // namespace

/* ===========================================================================
 * NRF RX — columns and cell formatting
 * ========================================================================= */

TEST(nrf_panel_publishes_the_columns_the_app_shows) {
    /* Names and order are NrfRxView::columns_ (src/apps/ui_nrf_rx.hpp):
     *   {{"Address", 12}, {"Len", 4}, {"Payload", 0}}
     * The widths describe a fixed-width screen and are not published. */
    app::NrfRecentEntries entries;
    const TableData t = remote::nrf_table_data(entries);

    CHECK_EQ(t.columns.size(), size_t{3});
    CHECK_STR_EQ(t.columns[0], "Address");
    CHECK_STR_EQ(t.columns[1], "Len");
    CHECK_STR_EQ(t.columns[2], "Payload");
}

TEST(nrf_panel_with_no_packets_yields_no_rows) {
    app::NrfRecentEntries entries;
    const TableData t = remote::nrf_table_data(entries);

    /* An empty container is not an error and must not fabricate a row. */
    CHECK_EQ(t.rows.size(), size_t{0});
    CHECK_EQ(t.columns.size(), size_t{3});
}

TEST(nrf_panel_row_matches_the_line_the_app_draws) {
    /* NrfRxView's on_draw paints
     *   to_string_hex(address, 10) + " " +
     *   to_string_dec_uint(payload_length, 3) + " " + payload_hex
     * so the three cells are those three fields, with the length's
     * right-justification and the trailing line padding dropped. */
    app::NrfRecentEntries entries;
    feed_nrf(entries, make_nrf_packet(kNrfAddress, kNrfPayload, 4, 2));

    const TableData t = remote::nrf_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    if (t.rows.empty()) return;

    CHECK_EQ(t.rows[0].size(), size_t{3});
    /* All ten hex digits are significant — an ESB address is five bytes. */
    CHECK_STR_EQ(t.rows[0][0], "E7E7E7E7E7");
    /* The app draws " 4" in a three-column field; that is right-justify. */
    CHECK_STR_EQ(t.rows[0][1], "4");
    /* Straight from nrf::Packet::payload_string(). */
    CHECK_STR_EQ(t.rows[0][2], "DE AD BE EF");
}

TEST(nrf_panel_leaves_a_zero_length_payload_blank) {
    /* An ESB frame may carry no payload at all (an ACK, for instance). The
     * length is a genuine counter reading zero and is published as "0"; the
     * payload is absent and stays absent, rather than becoming "00" or "--". */
    app::NrfRecentEntries entries;
    feed_nrf(entries, make_nrf_packet(0x0102030405ULL, nullptr, 0, 1));

    const TableData t = remote::nrf_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    if (t.rows.empty()) return;

    CHECK_STR_EQ(t.rows[0][0], "0102030405");
    CHECK_STR_EQ(t.rows[0][1], "0");
    CHECK_STR_EQ(t.rows[0][2], "");
}

TEST(nrf_panel_emits_empty_cells_for_an_entry_nothing_has_filled_in) {
    /* Defensive: an entry that exists but has had no packet applied to it has
     * no payload string. The cell must be empty rather than carrying invented
     * bytes. */
    app::NrfRecentEntries entries;
    entries.emplace_back(app::NrfRecentEntry::Key{0xE7E7E7E7E7ULL});

    const TableData t = remote::nrf_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    if (t.rows.empty()) return;

    CHECK_STR_EQ(t.rows[0][0], "E7E7E7E7E7");
    CHECK_STR_EQ(t.rows[0][1], "0");
    CHECK_STR_EQ(t.rows[0][2], "");
}

TEST(nrf_panel_lists_the_most_recently_heard_address_first) {
    /* ui::on_packet moves the matching entry to the front, which is the order
     * the device's table scrolls in; the published rows follow it. */
    app::NrfRecentEntries entries;
    feed_nrf(entries, make_nrf_packet(0xAABBCCDDEEULL, kNrfPayload, 4, 0));
    feed_nrf(entries, make_nrf_packet(kNrfAddress, kNrfPayload, 2, 1));

    const TableData t = remote::nrf_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{2});
    if (t.rows.size() < 2) return;

    CHECK_STR_EQ(t.rows[0][0], "E7E7E7E7E7");
    CHECK_STR_EQ(t.rows[1][0], "AABBCCDDEE");
}

/* ===========================================================================
 * Search — columns and cell formatting
 * ========================================================================= */

TEST(search_panel_publishes_the_columns_the_app_shows) {
    /* Names and order are SearchView::columns_ (src/apps/ui_search.hpp):
     *   {{"Frequency", 0}, {"Time", 8}, {"Duration", 8}} */
    app::SearchRecentEntries entries;
    const TableData t = remote::search_table_data(entries);

    CHECK_EQ(t.columns.size(), size_t{3});
    CHECK_STR_EQ(t.columns[0], "Frequency");
    CHECK_STR_EQ(t.columns[1], "Time");
    CHECK_STR_EQ(t.columns[2], "Duration");
}

TEST(search_panel_with_no_hits_yields_no_rows) {
    app::SearchRecentEntries entries;
    const TableData t = remote::search_table_data(entries);

    CHECK_EQ(t.rows.size(), size_t{0});
    CHECK_EQ(t.columns.size(), size_t{3});
}

TEST(search_panel_row_matches_the_line_the_app_draws) {
    app::SearchRecentEntries entries;
    feed_search_lock(entries, 433'920'000ULL, "12:34:56");
    feed_search_release(entries, 433'920'000ULL, 37);

    const TableData t = remote::search_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    if (t.rows.empty()) return;

    CHECK_EQ(t.rows[0].size(), size_t{3});
    /* to_string_short_freq(433'920'000) is " 433.9200"; the leading space is
     * its four-column right-justification of the MHz part. */
    CHECK_STR_EQ(t.rows[0][0], "433.9200");
    CHECK_STR_EQ(t.rows[0][1], "12:34:56");
    /* search::format_duration(37) — the app's own formatter, tenths of a
     * second below a minute. */
    CHECK_STR_EQ(t.rows[0][2], "3.7s");
}

TEST(search_panel_strips_only_the_frequency_right_justification) {
    /* Below 1 GHz to_string_short_freq pads the MHz part out to four columns.
     * The four decimals are the resolution the app resolved the hit to and are
     * kept; the pad is alignment and is dropped. */
    app::SearchRecentEntries entries;
    feed_search_lock(entries, 100'000'000ULL, "00:00:01");

    const TableData t = remote::search_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    if (t.rows.empty()) return;

    CHECK_STR_EQ(t.rows[0][0], "100.0000");
    CHECK_STR_EQ(to_string_short_freq(100'000'000ULL), " 100.0000");
}

TEST(search_panel_uses_the_apps_own_duration_format_past_a_minute) {
    /* format_duration switches to "MmSSs" at 60 s (600 tenths). */
    app::SearchRecentEntries entries;
    feed_search_lock(entries, 145'500'000ULL, "08:00:00");
    feed_search_release(entries, 145'500'000ULL, 725);

    const TableData t = remote::search_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    if (t.rows.empty()) return;

    CHECK_STR_EQ(t.rows[0][2], "1m12s");
}

TEST(search_panel_leaves_an_entry_with_no_time_blank) {
    /* An entry can reach the list without ever having had set_time() called:
     * do_detection()'s Release branch calls ui::on_packet() too, and re-creates
     * the row if the Lock row was evicted from the 64-entry list. The Time cell
     * stays empty rather than being given an invented timestamp. Duration IS
     * published at zero — it is a counter the detector advances every ~100 ms
     * while a hit stays locked, so zero means "no tenth elapsed yet". */
    app::SearchRecentEntries entries;
    entries.emplace_back(app::SearchRecentEntry::Key{868'300'000ULL});

    const TableData t = remote::search_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    if (t.rows.empty()) return;

    CHECK_STR_EQ(t.rows[0][0], "868.3000");
    CHECK_STR_EQ(t.rows[0][1], "");
    CHECK_STR_EQ(t.rows[0][2], "0.0s");
}

TEST(search_panel_lists_the_most_recent_hit_first) {
    app::SearchRecentEntries entries;
    feed_search_lock(entries, 100'000'000ULL, "10:00:00");
    feed_search_lock(entries, 433'920'000ULL, "10:00:05");

    const TableData t = remote::search_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{2});
    if (t.rows.size() < 2) return;

    CHECK_STR_EQ(t.rows[0][0], "433.9200");
    CHECK_STR_EQ(t.rows[1][0], "100.0000");
}

/* ===========================================================================
 * The providers themselves, end to end through AppBridge
 * ========================================================================= */

namespace {

/* Both views bind globals().receiver in their constructors, so a ReceiverModel
 * has to exist before the app registry's factory can build one. Nothing here
 * opens a device: SearchView::on_show() calls receiver.start(), which fails at
 * start_rx() on a closed radio and returns false without spawning a DSP thread,
 * so each view comes up with an empty results list — which is exactly the state
 * under test. Tears the globals back down so later tests see them as they
 * were. */
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
         * believing one of these apps is open hands that state to every later
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

/* Crude but sufficient: the panel bodies asserted on here are small and their
 * key order is fixed by AppBridge::panel_json(). */
bool panel_contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

TEST(nrf_panel_provider_publishes_a_table_when_the_app_is_open) {
    ProviderHarness h;
    /* The id has to be the one src/apps/ui_nrf_rx.cpp registers ("nrf_rx"), or
     * the bridge never reaches the provider at all and the portal keeps its
     * placeholder card. */
    h.launch("nrf_rx");
    CHECK_EQ(h.nav.depth(), size_t{2});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"app_id\":\"nrf_rx\""));
    CHECK(panel_contains(panel, "\"panel_kind\":\"table\""));
    CHECK(panel_contains(panel, "\"columns\":[\"Address\",\"Len\",\"Payload\"]"));
    /* No device, so nothing decoded: an empty rows array, not a fabricated
     * row. */
    CHECK(panel_contains(panel, "\"rows\":[]"));
}

TEST(nrf_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    ProviderHarness h;
    h.launch("nrf_rx");

    /* Any view pushed on top reproduces the condition the provider has to cope
     * with: it is handed that view, and NrfRxView is only reachable by walking
     * NavigationView::at_depth() down the stack. */
    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"panel_kind\":\"table\""));
    CHECK(panel_contains(panel, "\"columns\":[\"Address\",\"Len\",\"Payload\"]"));
}

TEST(nrf_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    ProviderHarness h;
    h.launch("nrf_rx");

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
    CHECK_EQ(h.nav.depth(), size_t{1});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"panel_kind\":\"screen\""));
    CHECK(panel_contains(panel, "Home -- no app is open."));
    /* The stale id is what this change fixed; pin it, not just the text. */
    CHECK(panel_contains(panel, "\"app_id\":\"\""));
    CHECK(!panel_contains(panel, "\"columns\""));
}

TEST(search_panel_provider_publishes_a_table_when_the_app_is_open) {
    ProviderHarness h;
    /* The id has to be the one src/apps/ui_search.cpp registers ("search"). */
    h.launch("search");
    CHECK_EQ(h.nav.depth(), size_t{2});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"app_id\":\"search\""));
    CHECK(panel_contains(panel, "\"panel_kind\":\"table\""));
    CHECK(panel_contains(panel, "\"columns\":[\"Frequency\",\"Time\",\"Duration\"]"));
    CHECK(panel_contains(panel, "\"rows\":[]"));
}

TEST(search_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    ProviderHarness h;
    h.launch("search");

    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"panel_kind\":\"table\""));
    CHECK(panel_contains(panel, "\"columns\":[\"Frequency\",\"Time\",\"Duration\"]"));
}

TEST(search_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    ProviderHarness h;
    h.launch("search");

    h.nav.pop_to_root();
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{1});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"panel_kind\":\"screen\""));
    CHECK(panel_contains(panel, "Home -- no app is open."));
    /* The stale id is what this change fixed; pin it, not just the text. */
    CHECK(panel_contains(panel, "\"app_id\":\"\""));
    CHECK(!panel_contains(panel, "\"columns\""));
}

/* The published data is a live read of the app's own container, not a snapshot
 * taken at launch: entries added to the view after the panel was first served
 * appear on the next refresh(). Driven through the adapter because neither
 * view exposes a way to inject a decoded packet — which is the point of the
 * accessor being const. */
TEST(nrf_and_search_panels_are_read_live_from_the_apps_containers) {
    app::NrfRecentEntries nrf;
    CHECK_EQ(remote::nrf_table_data(nrf).rows.size(), size_t{0});
    feed_nrf(nrf, make_nrf_packet(kNrfAddress, kNrfPayload, 4, 0));
    CHECK_EQ(remote::nrf_table_data(nrf).rows.size(), size_t{1});

    app::SearchRecentEntries search;
    CHECK_EQ(remote::search_table_data(search).rows.size(), size_t{0});
    feed_search_lock(search, 433'920'000ULL, "12:00:00");
    CHECK_EQ(remote::search_table_data(search).rows.size(), size_t{1});
}
