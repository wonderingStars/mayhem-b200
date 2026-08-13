/*
 * mayhem-b200 — tests for three web portal panel providers:
 *
 *   src/remote/provider_subcar.cpp        SubCar (car key-fob RX)
 *   src/remote/provider_two_tone_rx.cpp   Two-Tone (QCII paging) RX
 *   src/remote/provider_weather.cpp       Weather/TPMS
 *
 * Each publishes its app's ui::RecentEntries container as a table panel. Two
 * things have to hold for every one of them. The browser and the 240x320 screen
 * must show the same columns with the same cell text, or an operator reading one
 * and then the other is being told two different stories. And a field a
 * transmitter has not sent must stay empty rather than being invented: a TPMS
 * format that carries no temperature must not read as 0 C.
 *
 * Wherever an entry can be produced by the app's own code it is — the SubCar
 * rows below come out of the real ProtoSuzuki decoder and the Weather rows out
 * of the real Schrader frame decoders — so nothing here re-derives what the
 * provider is supposed to be republishing.
 *
 * The last group drives each provider end to end through AppBridge
 * (request_launch -> drain -> service -> refresh -> panel_json), because that is
 * the only path that sets the bridge's current app id and reaches the provider
 * at all. It is what catches a wrong app id, which is the single most likely way
 * for a provider to silently do nothing.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_context.hpp"
#include "app_registry.hpp"
#include "audio_out.hpp"
#include "receiver_model.hpp"
#include "remote/app_bridge.hpp"
#include "remote/app_data.hpp"
#include "ui.hpp"
#include "ui_navigation.hpp"
#include "ui_recent_entries.hpp"
#include "ui_subcar.hpp"
#include "ui_two_tone_rx.hpp"
#include "ui_weather.hpp"
#include "usrp_radio.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using remote::TableData;

namespace remote {
/* Defined in the three provider .cpp files; see the comment in each for why
 * they are exposed rather than left file-local. */
TableData subcar_table_data(const app::subcar::RecentEntries& entries);
TableData two_tone_table_data(const app::TwoToneLogEntries& entries);
TableData weather_table_data(const app::tpms::RecentEntries& entries);
}  // namespace remote

/* ===========================================================================
 * SubCar
 * ===========================================================================*/

namespace {

/* The Suzuki pulse train, in the shape tests/test_subcar.cpp already feeds the
 * decoder: a 250 us alternating preamble of >300 pulses, a 500 us HIGH that both
 * ends the preamble and contributes the leading 1 bit, 63 pulse-width-coded data
 * bits (500 us HIGH = 1, 250 us = 0) and a 2 ms end gap. Strictly alternating,
 * because that is all a real pulse extractor can deliver. */
void feed_suzuki_frame(app::subcar::ProtoSuzuki& proto, uint64_t payload63) {
    proto.feed(true, 250);
    for (int i = 0; i < 305; i++) {
        proto.feed(false, 250);
        proto.feed(true, 250);
    }
    proto.feed(false, 250);
    proto.feed(true, 500);

    for (int i = 62; i >= 0; i--) {
        const bool bit = ((payload63 >> i) & 1ull) != 0ull;
        proto.feed(false, 250);
        proto.feed(true, bit ? 500u : 250u);
    }
    proto.feed(false, 2000);
}

/* Exactly what SubCarView::on_decoded() builds from a decoder callback, so the
 * entry under test is the one the app would have listed. */
app::subcar::RecentEntries decode_one_suzuki(uint64_t payload63) {
    app::subcar::RecentEntries recent;
    app::subcar::ProtoSuzuki proto;
    proto.set_callback([&recent](app::subcar::CarProtocol& p) {
        recent.push_front(app::subcar::RecentEntry{p.sensor_type, p.decode_data, p.decode_data2,
                                                   p.data_count_bit});
    });
    feed_suzuki_frame(proto, payload63);
    return recent;
}

}  // namespace

TEST(subcar_panel_publishes_the_columns_the_app_shows) {
    /* Names and order are SubCarView::columns_ (src/apps/ui_subcar.hpp):
     * {"Type", 0}, {"Bits", 4}, {"Age", 3}. They cannot be read back off a
     * running view — the RecentEntriesColumns reference inside the table and its
     * header is private — so this is what holds the provider's copy to the
     * app's. */
    app::subcar::RecentEntries entries;
    const TableData t = remote::subcar_table_data(entries);

    CHECK_EQ(t.columns.size(), size_t{3});
    CHECK_STR_EQ(t.columns[0], "Type");
    CHECK_STR_EQ(t.columns[1], "Bits");
    CHECK_STR_EQ(t.columns[2], "Age");
}

TEST(subcar_panel_with_nothing_decoded_yields_no_rows) {
    app::subcar::RecentEntries entries;
    const TableData t = remote::subcar_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{0});
    /* An empty table still describes itself, so the browser draws the header
     * row rather than an empty box. */
    CHECK_EQ(t.columns.size(), size_t{3});
}

TEST(subcar_panel_row_matches_the_line_the_app_draws) {
    /* The real ProtoSuzuki decodes this frame; the entry is then built the way
     * SubCarView::on_decoded() builds it. The Type column carries the protocol
     * name AND the low eight hex digits of the payload, because that is what the
     * app's on_draw paints into it. */
    const auto entries = decode_one_suzuki(0x0A0B0C0Dull);
    CHECK_EQ(entries.size(), size_t{1});
    /* The leading 1 bit the preamble exit contributes puts bit 63 high. */
    CHECK_EQ(entries.front().data, 0x800000000A0B0C0Dull);

    const TableData t = remote::subcar_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_EQ(t.rows[0].size(), size_t{3});
    CHECK_STR_EQ(t.rows[0][0], "Suzuki 0A0B0C0D");
    CHECK_STR_EQ(t.rows[0][1], "64");
    /* A frame decoded this instant really is zero seconds old — a genuine
     * counter at zero, not a stand-in. */
    CHECK_STR_EQ(t.rows[0][2], "0");
}

TEST(subcar_panel_uses_the_apps_own_text_for_an_unidentified_frame) {
    /* An entry nothing has filled in. Every SubCar cell is always present: the
     * app's own sensor_type_name() answers "Unknown" for FPC_Invalid, and Bits
     * and Age are counters whose zero is a real reading. There is no field here
     * that could be blank, so nothing is blanked. */
    app::subcar::RecentEntries entries;
    entries.emplace_back();

    const TableData t = remote::subcar_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "Unknown 00000000");
    CHECK_STR_EQ(t.rows[0][1], "0");
    CHECK_STR_EQ(t.rows[0][2], "0");
}

TEST(subcar_panel_publishes_the_ages_the_app_holds) {
    /* inc_age() is the app's own once-a-second tick (SubCarView::on_frame_sync),
     * and the Age column is published straight from it. */
    auto entries = decode_one_suzuki(0x00000001ull);
    entries.front().inc_age(7);

    CHECK_STR_EQ(remote::subcar_table_data(entries).rows[0][2], "7");
}

TEST(subcar_panel_lists_entries_in_the_apps_own_order) {
    /* SubCarView::on_decoded() push_front()s, so the most recently decoded frame
     * is the top row on the device. The provider walks the container in order,
     * which is what keeps the two agreeing. */
    app::subcar::RecentEntries entries;
    entries.push_front(app::subcar::RecentEntry{app::subcar::FPC_SUBARU, 0x1111ull, 0, 64});
    entries.push_front(app::subcar::RecentEntry{app::subcar::FPC_BMWV0, 0x2222ull, 0, 61});

    const TableData t = remote::subcar_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{2});
    CHECK_STR_EQ(t.rows[0][0], "BMW V0 00002222");
    CHECK_STR_EQ(t.rows[1][0], "Subaru 00001111");
}

/* ===========================================================================
 * Two-Tone RX
 * ===========================================================================*/

namespace {

/* TwoToneRxView::log_pair() composes its line from two two_tone::format_tone()
 * calls joined by a space. It is private and only ever runs with a live radio
 * feeding on_frame_sync(), so the composition is mirrored here — with the app's
 * own formatter, and against the same tone pair tests/test_two_tone_rx.cpp gets
 * out of the real sequencer ("855.5Hz 1000ms 1153.4Hz 3000ms"). The provider
 * republishes entry.line verbatim, so what this pins is the shape of a row, not
 * a second copy of the formatting. */
app::TwoToneLogEntry make_logged_pair(uint32_t serial,
                                      uint32_t t1_hz,
                                      uint32_t t1_ms,
                                      uint32_t t2_hz,
                                      uint32_t t2_ms) {
    app::TwoToneLogEntry entry{serial};
    entry.line = app::two_tone::format_tone(t1_hz, t1_ms) + " " +
                 app::two_tone::format_tone(t2_hz, t2_ms);
    return entry;
}

}  // namespace

TEST(two_tone_panel_publishes_the_column_the_app_shows) {
    /* Name and order are TwoToneRxView::log_columns_
     * (src/apps/ui_two_tone_rx.hpp): a single {"A tone / B tone", 0}. */
    app::TwoToneLogEntries entries;
    const TableData t = remote::two_tone_table_data(entries);

    CHECK_EQ(t.columns.size(), size_t{1});
    CHECK_STR_EQ(t.columns[0], "A tone / B tone");
}

TEST(two_tone_panel_with_nothing_logged_yields_no_rows) {
    app::TwoToneLogEntries entries;
    const TableData t = remote::two_tone_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{0});
    CHECK_EQ(t.columns.size(), size_t{1});
}

TEST(two_tone_panel_row_matches_the_line_the_app_draws) {
    app::TwoToneLogEntries entries;
    entries.push_back(make_logged_pair(1, 856, 1000, 1154, 3000));

    const TableData t = remote::two_tone_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_EQ(t.rows[0].size(), size_t{1});
    /* Both tones snapped to their Motorola table entries by the app's own
     * format_tone(); the provider adds and removes nothing. */
    CHECK_STR_EQ(t.rows[0][0], "855.5Hz 1000ms 1153.4Hz 3000ms");
}

TEST(two_tone_panel_leaves_an_unfilled_entry_blank) {
    /* An entry that exists but whose pair was never filled in. Empty, not
     * "0Hz 0ms" — a log line with no tones in it is not a detection. */
    app::TwoToneLogEntries entries;
    entries.emplace_back(app::TwoToneLogEntry::Key{4});

    const TableData t = remote::two_tone_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "");
}

TEST(two_tone_panel_lists_entries_in_the_apps_own_order) {
    /* log_pair() push_back()s so that newer pairs appear BELOW older ones —
     * upstream's ordering, and the opposite of every other app here. The
     * provider must not quietly reverse it. */
    app::TwoToneLogEntries entries;
    entries.push_back(make_logged_pair(1, 856, 1000, 1154, 3000));
    entries.push_back(make_logged_pair(2, 1154, 1000, 856, 3000));

    const TableData t = remote::two_tone_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{2});
    CHECK_STR_EQ(t.rows[0][0], "855.5Hz 1000ms 1153.4Hz 3000ms");
    CHECK_STR_EQ(t.rows[1][0], "1153.4Hz 1000ms 855.5Hz 3000ms");
}

/* ===========================================================================
 * Weather / TPMS
 * ===========================================================================*/

namespace {

using Bits = std::vector<uint8_t>;

/* Appends the low `count` bits of `value`, most significant first — the order
 * app::tpms::read_field() reads them back in. Same helper
 * tests/test_tpms_ert.cpp builds its frames with. */
void put_bits(Bits& bits, uint64_t value, size_t count) {
    for (size_t i = count; i > 0; i--) {
        bits.push_back(static_cast<uint8_t>((value >> (i - 1)) & 1ull));
    }
}

/* OOK 8k192 Schrader: 37 symbols, [0:3) flags, [3:27) ID, [27:35) pressure,
 * [35:37) a checksum chosen so bit 0 plus the eighteen 2-bit groups starting at
 * bit 1 (the checksum field included) has 3 in its low two bits. This format
 * carries NO temperature, which is what makes it the honest case for an absent
 * field. */
Bits make_schrader_8k192(uint32_t flags3, uint32_t id24, uint8_t pressure_raw) {
    Bits bits;
    put_bits(bits, flags3 & 0x7u, 3);
    put_bits(bits, id24 & 0xffffffu, 24);
    put_bits(bits, pressure_raw, 8);
    put_bits(bits, 0, 2); /* placeholder for the checksum */

    uint32_t sum = bits[0];
    for (size_t i = 1; i < 35; i += 2) {
        sum += app::tpms::read_field(bits, i, 2);
    }
    const uint32_t checksum = (3u - (sum & 3u)) & 3u;

    bits[35] = static_cast<uint8_t>((checksum >> 1) & 1u);
    bits[36] = static_cast<uint8_t>(checksum & 1u);
    return bits;
}

/* OOK 8k4 Schrader (GMC_96): 76 symbols, [0:20) rest of the system ID,
 * [20:52) ID, [52:60) pressure, [60:68) temperature, [68:76) the 8-bit sum of
 * the nine bytes beginning with the assumed 0x4 nibble. Pressure AND
 * temperature, but no flags. */
Bits make_schrader_8k4(uint32_t system_id20,
                       uint32_t id32,
                       uint8_t pressure_raw,
                       uint8_t temp_raw) {
    Bits bits;
    put_bits(bits, system_id20 & 0xfffffu, 20);
    put_bits(bits, id32, 32);
    put_bits(bits, pressure_raw, 8);
    put_bits(bits, temp_raw, 8);
    put_bits(bits, 0, 8); /* placeholder for the checksum */

    uint8_t sum = static_cast<uint8_t>((0x4u << 4) | app::tpms::read_field(bits, 0, 4));
    for (size_t i = 4; i < 68; i += 8) {
        sum = static_cast<uint8_t>(sum + app::tpms::read_field(bits, i, 8));
    }
    for (size_t k = 0; k < 8; k++) {
        bits[68 + k] = static_cast<uint8_t>((sum >> (7 - k)) & 1u);
    }
    return bits;
}

/* Exactly what WeatherView::on_frame() does with a decoded reading, so the
 * entry under test is the one the app would have listed. */
void apply_reading(app::tpms::RecentEntries& recent,
                   app::tpms::SignalType signal_type,
                   const Bits& bits) {
    const auto reading = app::tpms::decode_reading(signal_type, bits);
    CHECK(reading.valid());
    if (!reading.valid()) return;

    auto& entry = ui::on_packet(recent, app::tpms::RecentEntry::Key{reading.type, reading.id});
    entry.update(reading);
    entry.signal_type = signal_type;
}

}  // namespace

TEST(weather_panel_publishes_the_columns_the_app_shows) {
    /* Names and order are WeatherView::columns_ (src/apps/ui_weather.hpp):
     * {"Tp",2}, {"ID",0}, {"Pres",4}, {"Temp",4}, {"Cnt",3}, {"Fl",2}. */
    app::tpms::RecentEntries entries;
    const TableData t = remote::weather_table_data(entries);

    CHECK_EQ(t.columns.size(), size_t{6});
    CHECK_STR_EQ(t.columns[0], "Tp");
    CHECK_STR_EQ(t.columns[1], "ID");
    CHECK_STR_EQ(t.columns[2], "Pres");
    CHECK_STR_EQ(t.columns[3], "Temp");
    CHECK_STR_EQ(t.columns[4], "Cnt");
    CHECK_STR_EQ(t.columns[5], "Fl");
}

TEST(weather_panel_with_nothing_received_yields_no_rows) {
    app::tpms::RecentEntries entries;
    const TableData t = remote::weather_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{0});
    CHECK_EQ(t.columns.size(), size_t{6});
}

TEST(weather_panel_row_matches_the_line_the_app_draws) {
    /* A real OOK 8k4 (GMC_96) frame through the app's own decoder: pressure
     * 100 * 11 / 4 = 275 kPa, temperature 100 - 61 = 39 C. */
    app::tpms::RecentEntries entries;
    apply_reading(entries, app::tpms::SignalType::Ook8k4Schrader,
                  make_schrader_8k4(0x12345u, 0xDEADBEEFu, 100, 100));

    const TableData t = remote::weather_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_EQ(t.rows[0].size(), size_t{6});
    /* Tp is the numeric Reading::Type the app's own column shows: GMC_96 = 5. */
    CHECK_STR_EQ(t.rows[0][0], "5");
    CHECK_STR_EQ(t.rows[0][1], "DEADBEEF");
    CHECK_STR_EQ(t.rows[0][2], "275");
    CHECK_STR_EQ(t.rows[0][3], "39");
    CHECK_STR_EQ(t.rows[0][4], "1");
    /* This format carries no flags, so the cell is empty rather than "00". */
    CHECK_STR_EQ(t.rows[0][5], "");
}

TEST(weather_panel_leaves_an_unreported_temperature_blank) {
    /* OOK 8k192 Schrader has no temperature field at all. The device leaves that
     * cell blank and so does the panel: an unreported temperature is not 0 C.
     * Pressure is 192 * 4 / 3 = 256 kPa, and the flags byte is the app's own
     * (flags << 4) | checksum. */
    app::tpms::RecentEntries entries;
    apply_reading(entries, app::tpms::SignalType::Ook8k192Schrader,
                  make_schrader_8k192(0b101u, 0xABCDEFu, 192));

    const TableData t = remote::weather_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    /* Schrader = 4. */
    CHECK_STR_EQ(t.rows[0][0], "4");
    CHECK_STR_EQ(t.rows[0][1], "00ABCDEF");
    CHECK_STR_EQ(t.rows[0][2], "256");
    CHECK_STR_EQ(t.rows[0][3], "");
    CHECK_STR_EQ(t.rows[0][4], "1");
    CHECK_STR_EQ(t.rows[0][5], "53");
}

TEST(weather_panel_publishes_kilopascal_and_celsius) {
    /* The operator's unit selection lives in two private OptionsFields with no
     * accessor, so the panel publishes what the entry natively stores and what
     * the app itself starts in: kPa and Celsius. 33 * 11 / 4 = 90 kPa (about 13
     * PSI, which is what the same entry would read as on a device switched to
     * PSI), and 40 - 61 = -21 C, which must keep its sign. */
    app::tpms::RecentEntries entries;
    apply_reading(entries, app::tpms::SignalType::Ook8k4Schrader,
                  make_schrader_8k4(0x00001u, 0x00000042u, 33, 40));

    const TableData t = remote::weather_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][2], "90");
    CHECK_STR_EQ(t.rows[0][3], "-21");
}

TEST(weather_panel_emits_empty_cells_for_an_entry_nothing_has_filled_in) {
    /* An entry that exists but has had no reading applied to it: pressure,
     * temperature and flags all come out empty rather than carrying a stand-in.
     * The reception count is a counter, so 0 is a real value and is published as
     * one. */
    app::tpms::RecentEntries entries;
    entries.emplace_back(app::tpms::RecentEntry::Key{app::tpms::Reading::Type::None, 0u});

    const TableData t = remote::weather_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][0], "0");
    CHECK_STR_EQ(t.rows[0][1], "00000000");
    CHECK_STR_EQ(t.rows[0][2], "");
    CHECK_STR_EQ(t.rows[0][3], "");
    CHECK_STR_EQ(t.rows[0][4], "0");
    CHECK_STR_EQ(t.rows[0][5], "");
}

TEST(weather_panel_keeps_the_apps_own_reception_count_ceiling) {
    /* WeatherView's on_draw shows "+++" past 999 rather than a wider number,
     * because the column is three characters. That ceiling is a real limit on
     * what the app reports and is kept; the right-justification around it is
     * screen layout and is dropped. */
    app::tpms::RecentEntries entries;
    auto& e = entries.emplace_back(
        app::tpms::RecentEntry::Key{app::tpms::Reading::Type::FLM_72, 0x1234u});

    e.received_count = 999;
    CHECK_STR_EQ(remote::weather_table_data(entries).rows[0][4], "999");

    e.received_count = 1000;
    CHECK_STR_EQ(remote::weather_table_data(entries).rows[0][4], "+++");
}

TEST(weather_panel_tracks_the_apps_own_counters_across_repeat_frames) {
    app::tpms::RecentEntries entries;
    const auto frame = make_schrader_8k4(0x12345u, 0xDEADBEEFu, 100, 100);
    apply_reading(entries, app::tpms::SignalType::Ook8k4Schrader, frame);
    apply_reading(entries, app::tpms::SignalType::Ook8k4Schrader, frame);

    const TableData t = remote::weather_table_data(entries);
    /* Same type and id, so one sensor rather than two. */
    CHECK_EQ(t.rows.size(), size_t{1});
    CHECK_STR_EQ(t.rows[0][4], "2");
}

TEST(weather_panel_lists_the_most_recently_heard_sensor_first) {
    app::tpms::RecentEntries entries;
    apply_reading(entries, app::tpms::SignalType::Ook8k4Schrader,
                  make_schrader_8k4(0x12345u, 0x11111111u, 100, 100));
    apply_reading(entries, app::tpms::SignalType::Ook8k192Schrader,
                  make_schrader_8k192(0b001u, 0x222222u, 120));

    const TableData t = remote::weather_table_data(entries);
    CHECK_EQ(t.rows.size(), size_t{2});
    CHECK_STR_EQ(t.rows[0][1], "00222222");
    CHECK_STR_EQ(t.rows[1][1], "11111111");
}

/* ===========================================================================
 * The providers themselves
 *
 * Everything above drives the three *_table_data() helpers directly, which pins
 * the columns and the cell formatting but never runs a provider — so the half
 * that has to FIND the app in the first place was unexercised. That is the half
 * that fails in the operator's hands rather than in a unit: the provider is
 * handed nav->top(), and the moment someone on the device opens a detail page
 * the top of the stack is not the app's root view.
 *
 * These go through AppBridge end to end, because that is the only path that sets
 * the bridge's current app id, and it is the path the HTTP handler reads.
 * ===========================================================================*/

namespace {

/* Every one of these views binds app::globals() in its constructor (SubCar and
 * 2-Tone dereference globals().receiver outright), so the globals have to exist
 * before the app registry's factory can build one. Nothing here opens a device:
 * on_show() calls receiver.start(), which fails at start_rx() on a closed radio
 * and returns false without spawning a DSP thread, so each view comes up with an
 * empty table — which is exactly the state under test. Tears the globals back
 * down so later tests see them as they were. */
struct PanelHarness {
    radio::UsrpRadio radio{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{radio, audio};
    ui::NavigationView nav{{0, 0, 240, 304}};

    radio::RadioDevice* saved_radio{app::globals().radio};
    radio::ReceiverModel* saved_receiver{app::globals().receiver};
    ui::NavigationView* saved_nav{app::globals().nav};

    PanelHarness() {
        app::globals().radio = &radio;
        app::globals().receiver = &receiver;
        app::globals().nav = &nav;

        nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304})); /* root */
        nav.service();
    }

    ~PanelHarness() {
        /* AppBridge is a process-global singleton, so a test that leaves it
         * believing one of these apps is open hands that state to every later
         * test in the binary — and to every -count= rerun. Clearing it needs the
         * nav still wired up, so it happens before the globals are put back. */
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
bool json_has(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

std::string refreshed_panel_json() {
    remote::AppBridge::instance().refresh();
    return remote::AppBridge::instance().panel_json();
}

}  // namespace

TEST(subcar_panel_provider_publishes_a_table_when_the_app_is_open) {
    PanelHarness h;
    /* The id has to be the one src/apps/ui_subcar.cpp registers, or the bridge
     * never reaches the provider at all and the portal keeps showing its
     * placeholder card with nothing anywhere reporting an error. */
    h.launch("subcarrx");
    CHECK_EQ(h.nav.depth(), size_t{2});

    const std::string panel = refreshed_panel_json();
    CHECK(json_has(panel, "\"app_id\":\"subcarrx\""));
    CHECK(json_has(panel, "\"panel_kind\":\"table\""));
    CHECK(json_has(panel, "\"columns\":[\"Type\",\"Bits\",\"Age\"]"));
    /* No device, so nothing decoded: an empty rows array, not a fabricated
     * row. */
    CHECK(json_has(panel, "\"rows\":[]"));
}

TEST(subcar_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    PanelHarness h;
    h.launch("subcarrx");

    /* SubCarView pushes a SubCarDetailView when a frame is selected
     * (ui_subcar.cpp). Any pushed view reproduces the condition: the provider is
     * handed that view, and SubCarView is only reachable by walking down. */
    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    const std::string panel = refreshed_panel_json();
    CHECK(json_has(panel, "\"panel_kind\":\"table\""));
    CHECK(json_has(panel, "\"columns\":[\"Type\",\"Bits\",\"Age\"]"));
}

TEST(subcar_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    PanelHarness h;
    h.launch("subcarrx");

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

    const std::string panel = refreshed_panel_json();
    CHECK(json_has(panel, "\"panel_kind\":\"screen\""));
    CHECK(json_has(panel, "Home -- no app is open."));
    /* The stale id is what this change fixed; pin it, not just the text. */
    CHECK(json_has(panel, "\"app_id\":\"\""));
    /* Emphatically not an empty table, which would be indistinguishable from a
     * running receiver that has decoded nothing. */
    CHECK(!json_has(panel, "\"columns\""));
}

TEST(two_tone_panel_provider_publishes_a_table_when_the_app_is_open) {
    PanelHarness h;
    /* The id has to be the one src/apps/ui_two_tone_rx.cpp registers. */
    h.launch("two_tone_rx");
    CHECK_EQ(h.nav.depth(), size_t{2});

    const std::string panel = refreshed_panel_json();
    CHECK(json_has(panel, "\"app_id\":\"two_tone_rx\""));
    CHECK(json_has(panel, "\"panel_kind\":\"table\""));
    CHECK(json_has(panel, "\"columns\":[\"A tone / B tone\"]"));
    CHECK(json_has(panel, "\"rows\":[]"));
}

TEST(two_tone_panel_provider_survives_a_pushed_view) {
    PanelHarness h;
    h.launch("two_tone_rx");

    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();

    const std::string panel = refreshed_panel_json();
    CHECK(json_has(panel, "\"panel_kind\":\"table\""));
    CHECK(json_has(panel, "\"columns\":[\"A tone / B tone\"]"));
}

TEST(two_tone_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    PanelHarness h;
    h.launch("two_tone_rx");

    h.nav.pop_to_root();
    h.nav.service();

    const std::string panel = refreshed_panel_json();
    CHECK(json_has(panel, "\"panel_kind\":\"screen\""));
    CHECK(json_has(panel, "Home -- no app is open."));
    /* The stale id is what this change fixed; pin it, not just the text. */
    CHECK(json_has(panel, "\"app_id\":\"\""));
    CHECK(!json_has(panel, "\"columns\""));
}

TEST(weather_panel_provider_publishes_a_table_when_the_app_is_open) {
    PanelHarness h;
    /* The id has to be the one src/apps/ui_weather.cpp registers. */
    h.launch("weather");
    CHECK_EQ(h.nav.depth(), size_t{2});

    const std::string panel = refreshed_panel_json();
    CHECK(json_has(panel, "\"app_id\":\"weather\""));
    CHECK(json_has(panel, "\"panel_kind\":\"table\""));
    CHECK(json_has(panel, "\"columns\":[\"Tp\",\"ID\",\"Pres\",\"Temp\",\"Cnt\",\"Fl\"]"));
    CHECK(json_has(panel, "\"rows\":[]"));
}

TEST(weather_panel_provider_survives_the_operator_drilling_into_a_sub_view) {
    PanelHarness h;
    h.launch("weather");

    /* WeatherView pushes a TpmsDetailView when a sensor is selected
     * (ui_weather.cpp). */
    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    const std::string panel = refreshed_panel_json();
    CHECK(json_has(panel, "\"panel_kind\":\"table\""));
    CHECK(json_has(panel, "\"columns\":[\"Tp\",\"ID\",\"Pres\",\"Temp\",\"Cnt\",\"Fl\"]"));
}

TEST(weather_panel_provider_says_so_honestly_when_the_app_is_not_on_the_stack) {
    PanelHarness h;
    h.launch("weather");

    h.nav.pop_to_root();
    h.nav.service();

    const std::string panel = refreshed_panel_json();
    CHECK(json_has(panel, "\"panel_kind\":\"screen\""));
    CHECK(json_has(panel, "Home -- no app is open."));
    /* The stale id is what this change fixed; pin it, not just the text. */
    CHECK(json_has(panel, "\"app_id\":\"\""));
    CHECK(!json_has(panel, "\"columns\""));
}
