/*
 * mayhem-b200 — tests for the web portal panel providers of AIS RX and BLE RX
 * (src/remote/provider_ais.cpp, src/remote/provider_ble_rx.cpp).
 *
 * Two things have to hold for each app. The browser and the 240x320 screen must
 * show the same columns with the same cell text, or an operator reading one and
 * then the other is being told two different stories. And a field a target has
 * not sent must stay empty rather than being invented: a vessel that has not
 * broadcast its name is not a blank-named ship, and an advertiser with no
 * AdvData has not sent a run of zero bytes.
 *
 * Every entry below is filled in the way the app fills it — the AIS ones by the
 * real AISRecentEntry::update() from a real ais::Packet that passed its own
 * length and FCS checks, the BLE ones from a real ble_rx::Packet the real
 * ble_rx::Decoder recovered from a synthesised GFSK burst. Nothing here
 * re-derives a decode.
 *
 * The end-to-end blocks at the bottom are the ones that catch a wrong app id:
 * everything above them calls the table adapter directly and would keep passing
 * while the portal served a placeholder card forever.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ais_app.hpp"
#include "app_context.hpp"
#include "app_registry.hpp"
#include "audio_out.hpp"
#include "demod_digital.hpp"
#include "receiver_model.hpp"
#include "remote/app_bridge.hpp"
#include "remote/app_data.hpp"
#include "ui.hpp"
#include "ui_ble_rx.hpp"
#include "ui_navigation.hpp"
#include "ui_recent_entries.hpp"
#include "usrp_radio.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using remote::PanelKind;
using remote::TableData;

namespace remote {
/* Defined in src/remote/provider_ais.cpp and src/remote/provider_ble_rx.cpp;
 * see the comments there for why they are exposed rather than left
 * file-local. */
TableData ais_table_data(const app::AISRecentEntries& entries);
TableData ble_table_data(const app::ble_rx::BleRecentEntries& entries);
}  // namespace remote

namespace {

/* Crude but sufficient: the panel bodies asserted on here are small and their
 * key order is fixed by AppBridge::panel_json(). */
bool panel_contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

/* --- AIS payload construction -----------------------------------------------
 *
 * ITU-R M.1371 numbers payload bits MSB-first across the octets, which is what
 * ais::Packet::read() returns, so a payload is built here as an MSB-first bit
 * array and packed MSB-first into octets. Same shape as the builder in
 * tests/test_ais.cpp, which is file-static there. */
class AisPayloadBuilder {
   public:
    void put(uint64_t value, size_t nbits) {
        for (size_t i = nbits; i > 0; --i)
            bits_.push_back(static_cast<uint8_t>((value >> (i - 1)) & 1));
    }

    /* Six-bit ASCII, '@'-padded to `chars` characters — the inverse of
     * ais::six_bit_to_ascii, and the padding ais::format::text() strips. */
    void put_text(const std::string& s, size_t chars) {
        for (size_t i = 0; i < chars; i++) {
            const uint8_t c = (i < s.size()) ? static_cast<uint8_t>(s[i]) : uint8_t{'@'};
            put(static_cast<uint64_t>(((c - 32) ^ 32) & 0x3F), 6);
        }
    }

    size_t bit_count() const { return bits_.size(); }

    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> out((bits_.size() + 7) / 8, 0);
        for (size_t i = 0; i < bits_.size(); i++)
            if (bits_[i]) out[i / 8] = static_cast<uint8_t>(out[i / 8] | (0x80u >> (i % 8)));
        return out;
    }

   private:
    std::vector<uint8_t> bits_{};
};

/* Message 1, position report: 168 bits. Carries no name and no call sign, which
 * is the whole point of it here. */
std::vector<uint8_t> ais_message_1(uint32_t mmsi) {
    AisPayloadBuilder b;
    b.put(1, 6);        /*   0 message id       */
    b.put(0, 2);        /*   6 repeat           */
    b.put(mmsi, 30);    /*   8 MMSI             */
    b.put(0, 4);        /*  38 nav status       */
    b.put(0, 8);        /*  42 rate of turn     */
    b.put(74, 10);      /*  50 speed over grnd  */
    b.put(1, 1);        /*  60 position acc.    */
    b.put(0, 28);       /*  61 longitude        */
    b.put(0, 27);       /*  89 latitude         */
    b.put(1234, 12);    /* 116 course over grnd */
    b.put(41, 9);       /* 128 true heading     */
    b.put(30, 6);       /* 137 UTC second       */
    b.put(0, 2);        /* 143 manoeuvre        */
    b.put(0, 3);        /* 145 spare            */
    b.put(0, 1);        /* 148 RAIM             */
    b.put(0x7FFFF, 19); /* 149 radio status     */
    CHECK_EQ(b.bit_count(), size_t{168});
    return b.bytes();
}

/* Message 5, static and voyage related data: 424 bits. */
std::vector<uint8_t> ais_message_5(uint32_t mmsi,
                                   const std::string& call_sign,
                                   const std::string& name) {
    AisPayloadBuilder b;
    b.put(5, 6);              /*   0 message id  */
    b.put(0, 2);              /*   6 repeat      */
    b.put(mmsi, 30);          /*   8 MMSI        */
    b.put(0, 2);              /*  38 AIS version */
    b.put(9134567, 30);       /*  40 IMO number  */
    b.put_text(call_sign, 7); /*  70 call sign   */
    b.put_text(name, 20);     /* 112 vessel name */
    b.put(70, 8);             /* 232 ship type   */
    b.put(90, 9);             /* 240 dim to bow  */
    b.put(30, 9);             /* 249 dim to stern*/
    b.put(10, 6);             /* 258 dim to port */
    b.put(10, 6);             /* 264 dim to stbd */
    b.put(1, 4);              /* 270 EPFD type   */
    b.put(5, 4);              /* 274 ETA month   */
    b.put(17, 5);             /* 278 ETA day     */
    b.put(9, 5);              /* 283 ETA hour    */
    b.put(30, 6);             /* 288 ETA minute  */
    b.put(64, 8);             /* 294 draught     */
    b.put_text("", 20);       /* 302 destination */
    b.put(0, 1);              /* 422 DTE         */
    b.put(0, 1);              /* 423 spare       */
    CHECK_EQ(b.bit_count(), size_t{424});
    return b.bytes();
}

ais::Packet ais_decode(const std::vector<uint8_t>& payload) {
    return ais::Packet::from_bits(ais::build_packet_bits(payload));
}

/* The app's own path from a frame to an entry: ui::on_packet() finds or creates
 * by MMSI, then AISRecentEntry::update() applies the fields — the two calls
 * AISAppView::on_packet() makes (ais_app.cpp). */
void ais_apply(app::AISRecentEntries& entries, const std::vector<uint8_t>& payload) {
    const ais::Packet packet = ais_decode(payload);
    /* Only a frame the app would have accepted is allowed through here. */
    CHECK(packet.is_valid());
    auto& entry = ui::on_packet(entries, packet.source_id());
    entry.update(packet);
}

}  // namespace

/* --- AIS: columns and cells ------------------------------------------------ */

TEST(ais_panel_publishes_the_columns_the_app_shows) {
    /* Names and order are AISAppView::columns_ (src/apps/ais_app.hpp):
     * {"MMSI", 9}, {"Name/Call", 0}. They cannot be read back off a running
     * view — RecentEntriesView resolves the zero-width column against the
     * screen width and only its private header and table hold the object — so
     * this is what holds the provider's copy to the app's. */
    app::AISRecentEntries entries;
    const TableData t = remote::ais_table_data(entries);

    CHECK_EQ(t.columns.size(), size_t{2});
    CHECK_STR_EQ(t.columns[0], "MMSI");
    CHECK_STR_EQ(t.columns[1], "Name/Call");
}

TEST(ais_panel_with_no_ships_heard_yields_no_rows) {
    app::AISRecentEntries entries;
    const TableData t = remote::ais_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{0});
    /* An empty table still describes itself, so the browser draws the header
     * row rather than an empty box. */
    CHECK_EQ(t.columns.size(), size_t{2});
}

TEST(ais_panel_row_matches_the_line_the_app_draws) {
    app::AISRecentEntries entries;
    ais_apply(entries, ais_message_5(244660320u, "PBRV", "EVER GIVEN"));

    const TableData t = remote::ais_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_EQ(t.rows[0].size(), size_t{2});
    CHECK_STR_EQ(t.rows[0][0], "244660320");
    /* The app prefers the vessel name over the call sign, with the '@' padding
     * of the unused six-bit characters stripped. */
    CHECK_STR_EQ(t.rows[0][1], "EVER GIVEN");
}

TEST(ais_panel_zero_pads_the_mmsi_the_way_the_app_does) {
    /* ais::format::mmsi() is always nine characters, and it is what both the
     * list and the detail page use. Not screen alignment: an MMSI is a nine
     * digit identity and its leading zeros are part of it. */
    app::AISRecentEntries entries;
    ais_apply(entries, ais_message_1(1234567u));

    const TableData t = remote::ais_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "001234567");
}

TEST(ais_panel_leaves_a_ship_that_has_sent_no_name_or_call_sign_blank) {
    /* A position report carries neither. Many transponders send the message 5
     * that does only once every six minutes, so this is the ordinary state of a
     * freshly heard vessel — and it must read as "not sent yet", not as a ship
     * whose name is empty. */
    app::AISRecentEntries entries;
    ais_apply(entries, ais_message_1(232003812u));

    const TableData t = remote::ais_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "232003812");
    CHECK_STR_EQ(t.rows[0][1], "");
}

TEST(ais_panel_falls_back_to_the_call_sign_only_when_no_name_field_arrived) {
    /* The app tests the RAW stored name, and a message 5 stores twenty '@'
     * characters for an unused name field — non-empty, so the fallback does NOT
     * fire and the device shows an empty cell. Reproduced exactly: testing the
     * formatted name instead would put a call sign in the browser that the
     * screen does not show. */
    app::AISRecentEntries entries;
    ais_apply(entries, ais_message_5(211234560u, "DK1AB", ""));

    const TableData t = remote::ais_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][1], "");

    /* Message 24 part B stores only a call sign and never touches the name, so
     * there the fallback is what the device draws. */
    app::AISRecentEntries call_only;
    auto& e = call_only.emplace_back(app::AISRecentEntry::Key{211234560u});
    e.call_sign = "DK1AB";
    CHECK_STR_EQ(remote::ais_table_data(call_only).rows[0][1], "DK1AB");
}

TEST(ais_panel_emits_empty_cells_for_an_entry_nothing_has_filled_in) {
    /* An entry that exists but has had no packet applied to it: the name/call
     * cell has to come out empty rather than carrying a stand-in. The MMSI is
     * the entry's own key and so is real by construction. */
    app::AISRecentEntries entries;
    entries.emplace_back(app::AISRecentEntry::Key{366123456u});

    const TableData t = remote::ais_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "366123456");
    CHECK_STR_EQ(t.rows[0][1], "");
}

TEST(ais_panel_lists_the_most_recently_heard_ship_first) {
    app::AISRecentEntries entries;
    ais_apply(entries, ais_message_5(244660320u, "PBRV", "EVER GIVEN"));
    ais_apply(entries, ais_message_5(232003812u, "MDXY3", "QUEEN MARY 2"));

    const TableData t = remote::ais_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{2});
    CHECK_STR_EQ(t.rows[0][0], "232003812");
    CHECK_STR_EQ(t.rows[1][0], "244660320");
}

TEST(ais_panel_keeps_one_row_per_mmsi_across_repeat_frames) {
    /* The position report arrives first and names nothing; the static frame
     * then fills the same entry in rather than making a second one. */
    app::AISRecentEntries entries;
    ais_apply(entries, ais_message_1(244660320u));
    CHECK_STR_EQ(remote::ais_table_data(entries).rows[0][1], "");

    ais_apply(entries, ais_message_5(244660320u, "PBRV", "EVER GIVEN"));
    const TableData t = remote::ais_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][1], "EVER GIVEN");
}

/* --- BLE: columns and cells ------------------------------------------------ */

namespace {

namespace ble = app::ble_rx;

std::vector<uint8_t> ble_bytes_to_bits_lsb(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> bits;
    bits.reserve(bytes.size() * 8);
    for (uint8_t b : bytes)
        for (int k = 0; k < 8; k++) bits.push_back(static_cast<uint8_t>((b >> k) & 1));
    return bits;
}

/* A synthesised advertising burst on channel 37, built with the same CRC and
 * whitening the decoder verifies — the same construction tests/test_ble_rx.cpp
 * proves the decoder against, so the packet the decoder hands back here is a
 * genuine decode rather than a hand-set struct.
 *
 * `pdu` is the on-air PDU: header {type/flags, length}, then the six AdvA
 * octets least-significant first, then AdvData. */
std::vector<dsp::cfloat> ble_burst(const std::vector<uint8_t>& pdu) {
    const uint32_t crc = ble::crc24(pdu.data(), pdu.size(), ble::crc_init_reorder(ble::kCrcInit));

    std::vector<uint8_t> full = pdu;
    full.push_back(static_cast<uint8_t>(crc & 0xFF));
    full.push_back(static_cast<uint8_t>((crc >> 8) & 0xFF));
    full.push_back(static_cast<uint8_t>((crc >> 16) & 0xFF));

    const auto whitened = ble::whiten(ble_bytes_to_bits_lsb(full), 37);

    /* Preamble 0xAA then access address D6 BE 89 8E, on-air (LSB-first) order. */
    auto bits = ble_bytes_to_bits_lsb(std::vector<uint8_t>{0xAA, 0xD6, 0xBE, 0x89, 0x8E});
    bits.insert(bits.end(), whitened.begin(), whitened.end());
    for (int i = 0; i < 4; i++) bits.push_back(0); /* trailing readable symbols */

    return dsp::fsk_modulate(bits, 4'000'000.0f, 1'000'000.0f, 250'000.0f, 0.5f);
}

ble::Packet ble_decode(const std::vector<uint8_t>& pdu) {
    const auto iq = ble_burst(pdu);
    CHECK(!iq.empty());

    ble::Decoder decoder;
    decoder.configure(4, 37);

    ble::Packet got{};
    int hits = 0;
    decoder.set_on_packet([&](const ble::Packet& p) {
        got = p;
        hits++;
    });
    decoder.process(iq.data(), iq.size());

    /* Anything else and the fixture is wrong, not the provider. */
    CHECK_EQ(hits, 1);
    return got;
}

/* The app's own path from a decoded packet to an entry, as
 * BleRxView::on_packet() writes it (ui_ble_rx.cpp). That method is private and
 * is only ever driven by the sample tap, so this is the closest reachable
 * spelling of it; the four assignments are the whole of it. */
void ble_apply(ble::BleRecentEntries& entries, const ble::Packet& packet) {
    auto& entry = ui::on_packet(entries, packet.mac_key(), 32);
    entry.count++;
    entry.type = packet.type;
    entry.payload_len = packet.payload_len;
    entry.data_hex = packet.data_string();
}

/* ADV_IND from AA:BB:CC:DD:EE:FF carrying the three-octet "Flags: LE General
 * Discoverable" AD structure. */
std::vector<uint8_t> ble_pdu_with_data() {
    return {0x40, 0x09, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x02, 0x01, 0x1A};
}

/* The same advertiser with an empty payload: length 6 is the six AdvA octets
 * and no AdvData at all. */
std::vector<uint8_t> ble_pdu_without_data() {
    return {0x42, 0x06, 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA};
}

}  // namespace

TEST(ble_panel_publishes_the_columns_the_app_shows) {
    /* Names and order are BleRxView::columns_ (src/apps/ui_ble_rx.hpp):
     * {"MAC", 13}, {"T", 2}, {"Data", 0}. The object is a private member the
     * table only holds a reference to, so it cannot be read back off a running
     * view; this is what holds the provider's copy to the app's. */
    ble::BleRecentEntries entries;
    const TableData t = remote::ble_table_data(entries);

    CHECK_EQ(t.columns.size(), size_t{3});
    CHECK_STR_EQ(t.columns[0], "MAC");
    CHECK_STR_EQ(t.columns[1], "T");
    CHECK_STR_EQ(t.columns[2], "Data");
}

TEST(ble_panel_with_no_advertisers_heard_yields_no_rows) {
    ble::BleRecentEntries entries;
    const TableData t = remote::ble_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{0});
    CHECK_EQ(t.columns.size(), size_t{3});
}

TEST(ble_panel_row_matches_the_line_the_app_draws) {
    const ble::Packet packet = ble_decode(ble_pdu_with_data());
    ble::BleRecentEntries entries;
    ble_apply(entries, packet);

    const TableData t = remote::ble_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_EQ(t.rows[0].size(), size_t{3});

    /* Pinned twice: against the literal the device paints, and against the
     * app's own Packet::mac_string()/data_string(), so a change to either
     * formatter shows up here rather than silently splitting the browser from
     * the screen. */
    CHECK_STR_EQ(t.rows[0][0], "AA:BB:CC:DD:EE:FF");
    CHECK_STR_EQ(t.rows[0][0], packet.mac_string());
    /* The numeric ADV_PDU_TYPE, which is what the app's on_draw shows. */
    CHECK_STR_EQ(t.rows[0][1], "0");
    CHECK_STR_EQ(t.rows[0][2], "02 01 1A");
    CHECK_STR_EQ(t.rows[0][2], packet.data_string());
}

TEST(ble_panel_leaves_an_advertiser_that_sent_no_payload_blank) {
    /* ADV_NONCONN_IND with a six octet payload: the AdvA and nothing else. The
     * Data cell must be empty, not a run of zero bytes. */
    const ble::Packet packet = ble_decode(ble_pdu_without_data());
    CHECK_EQ(packet.payload_len, uint8_t{6});
    CHECK_EQ(packet.data.size(), size_t{0});

    ble::BleRecentEntries entries;
    ble_apply(entries, packet);

    const TableData t = remote::ble_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "AA:BB:CC:DD:EE:FF");
    CHECK_STR_EQ(t.rows[0][1], "2"); /* ADV_NONCONN_IND */
    CHECK_STR_EQ(t.rows[0][2], "");
}

TEST(ble_panel_emits_an_empty_data_cell_for_an_entry_nothing_has_filled_in) {
    /* An entry that exists but has had no packet applied to it. The MAC is the
     * entry's own key and so is real by construction; the payload hex is the
     * only thing that can be absent, and it comes out empty. */
    ble::BleRecentEntries entries;
    entries.emplace_back(ble::BleRecentEntry::Key{0xAABBCCDDEEFFULL});

    const TableData t = remote::ble_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "AA:BB:CC:DD:EE:FF");
    CHECK_STR_EQ(t.rows[0][2], "");
}

TEST(ble_panel_keeps_one_row_per_advertiser_across_repeat_frames) {
    const ble::Packet with_data = ble_decode(ble_pdu_with_data());
    const ble::Packet without_data = ble_decode(ble_pdu_without_data());
    /* Same advertiser, so one row — and the newer frame's payload wins, which
     * is what the app's on_packet does. */
    CHECK_EQ(with_data.mac_key(), without_data.mac_key());

    ble::BleRecentEntries entries;
    ble_apply(entries, with_data);
    ble_apply(entries, without_data);

    const TableData t = remote::ble_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][1], "2");
    CHECK_STR_EQ(t.rows[0][2], "");
}

/* --- The providers themselves ------------------------------------------------
 *
 * Everything above drives the table adapters directly, which pins the columns
 * and the cell formatting but never runs ais_panel()/ble_panel() — so the part
 * of each provider that has to find the app in the first place, and the app id
 * it registers under, were unexercised. A wrong id is the single most likely
 * way this silently does nothing: the provider never fires, the portal keeps
 * serving its placeholder card, and every assertion above still passes.
 *
 * These go through AppBridge end to end — request_launch(), drain, service(),
 * refresh(), panel_json() — because that is the only path that sets the
 * bridge's current app id, and it is the path the HTTP handler reads. */

namespace {

/* Both views bind globals() in their constructors or on_show(), so a
 * ReceiverModel has to exist before the app registry's factory can build one.
 * Nothing here opens a device: on_show() calls receiver.start(), which fails at
 * start_rx() on a closed radio and returns false without spawning a DSP thread,
 * so each view comes up with an empty list — which is exactly the state under
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

}  // namespace

TEST(ais_panel_provider_publishes_a_table_when_the_app_is_open) {
    ProviderHarness h;
    /* The id has to be the one src/apps/ais_app.cpp registers ("ais"), or the
     * bridge never reaches the provider at all and the depth stays 1. */
    h.launch("ais");
    CHECK_EQ(h.nav.depth(), size_t{2});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"app_id\":\"ais\""));
    CHECK(panel_contains(panel, "\"panel_kind\":\"table\""));
    CHECK(panel_contains(panel, "\"columns\":[\"MMSI\",\"Name/Call\"]"));
    /* No device, so no ships: an empty rows array, not a fabricated row. */
    CHECK(panel_contains(panel, "\"rows\":[]"));
}

TEST(ais_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    ProviderHarness h;
    h.launch("ais");

    /* AISRecentEntryDetailView pushes a GeoMapView when a ship's "See on map"
     * button is pressed (ais_app.cpp). Any pushed view reproduces the
     * condition: the provider is handed that view, and AISAppView is only
     * reachable by walking down. */
    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    /* Still the ship list. Without NavigationView::at_depth() this would be a
     * PanelKind::Screen and the browser would go blank for as long as the
     * operator left the map open. */
    CHECK(panel_contains(panel, "\"panel_kind\":\"table\""));
    CHECK(panel_contains(panel, "\"columns\":[\"MMSI\",\"Name/Call\"]"));
}

TEST(ais_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    ProviderHarness h;
    h.launch("ais");

    /* Popped on the device rather than through request_home(), so the bridge
     * still believes ais is current while AISAppView has gone. The provider has
     * to report that rather than serve an empty table that reads as "AIS RX is
     * running and hearing nothing". */
    h.nav.pop_to_root();
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{1});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"panel_kind\":\"screen\""));
    CHECK(panel_contains(panel, "AIS RX is not the open app."));
    CHECK(!panel_contains(panel, "\"columns\""));
}

TEST(ble_panel_provider_publishes_a_table_when_the_app_is_open) {
    ProviderHarness h;
    /* The id has to be the one src/apps/ui_ble_rx.cpp registers ("blerx"). */
    h.launch("blerx");
    CHECK_EQ(h.nav.depth(), size_t{2});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"app_id\":\"blerx\""));
    CHECK(panel_contains(panel, "\"panel_kind\":\"table\""));
    CHECK(panel_contains(panel, "\"columns\":[\"MAC\",\"T\",\"Data\"]"));
    /* No device, so nothing heard: an empty rows array, not a fabricated row. */
    CHECK(panel_contains(panel, "\"rows\":[]"));
}

TEST(ble_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    ProviderHarness h;
    h.launch("blerx");

    /* BleRxView pushes nothing of its own today, but the provider must not
     * depend on that: anything on top of it has to leave the panel intact. */
    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"panel_kind\":\"table\""));
    CHECK(panel_contains(panel, "\"columns\":[\"MAC\",\"T\",\"Data\"]"));
}

TEST(ble_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    ProviderHarness h;
    h.launch("blerx");

    h.nav.pop_to_root();
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{1});

    remote::AppBridge::instance().refresh();
    const std::string panel = remote::AppBridge::instance().panel_json();

    CHECK(panel_contains(panel, "\"panel_kind\":\"screen\""));
    CHECK(panel_contains(panel, "BLE RX is not the open app."));
    CHECK(!panel_contains(panel, "\"columns\""));
}
