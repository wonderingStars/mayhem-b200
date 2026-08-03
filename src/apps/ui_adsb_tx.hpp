/*
 * mayhem-b200 — ADS-B / Mode S TX.
 *
 * Ported from firmware/application/external/adsbtx/ (ui_adsb_tx.*),
 * firmware/common/adsb.* / adsb_frame.* (the frame encoders and CRC-24) and
 * firmware/baseband/proc_adsbtx.* (the PPM keyer).
 *
 * A Mode S extended squitter (DF17) frame is 112 bits: 5-bit Downlink Format
 * (17) + 3-bit CA + 24-bit ICAO address + 56-bit ME payload + 24-bit CRC. The
 * CRC-24 generator, the CPR position algorithm, the callsign 6-bit alphabet
 * and the ICAO-address framing are all shared with this project's ADS-B
 * receiver (ui_adsb_rx.hpp's `app::adsb` namespace) rather than re-derived —
 * this is the "cross-check the polynomial against the RX decoder" the
 * porting brief asks for: encode and decode literally run the same
 * dsp::Crc<24> instance with the same polynomial (app::adsb::kModeSPolynomial
 * = 0xFFF409, upstream's 0x1205FFF in reversed-offset form), so a frame built
 * here always has check_CRC() == 0 by construction, and tests/test_adsb.cpp
 * already proves that engine against upstream's independent CRC algorithm.
 *
 * On air, Mode S is pulse-position modulated at 1 Mbit/s: an 8 us preamble
 * (pulses at 0, 1.0, 3.5, 4.5 us, NOT Manchester-coded) followed by the 112
 * payload bits, each Manchester/PPM-coded 1 us wide (high->low = 1,
 * low->high = 0) — dsp::manchester_encode(bits, sense 0) is exactly that
 * mapping, and it is the same convention this project's RX side decodes
 * (ui_adsb_rx.cpp's AdsbDemod::process_one: "high then low is a 1"). This
 * port therefore Manchester-encodes the payload and feeds the whole preamble
 * + payload chip stream through dsp::ook_modulate() at 4 Msps (2 samples per
 * 500 ns chip) rather than reproducing proc_adsbtx.cpp's "crude AM" spinning
 * 4-point constellation, which was only an int8 fixed-point trick for the
 * M0 DAC and not part of the on-air format — the same simplification this
 * project's AmModulator already makes for ordinary AM (see dsp/modulate.hpp).
 *
 * LEGALITY: transmitting ADS-B on 1090 MHz is illegal almost everywhere —
 * it injects false targets into real air-traffic surveillance. This view
 * shows a persistent on-screen warning and requires an explicit
 * acknowledgement before the first transmission; it never starts on its own.
 * No hardware is attached during development, so actual radiation is
 * unverified — say so.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_ADSB_TX_H__
#define __MB200_UI_ADSB_TX_H__

#include "../dsp/demod.hpp" /* dsp::cfloat */
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_geomap.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"
#include "ui_adsb_rx.hpp" /* app::adsb::AdsbFrame, mode_s_crc, CPR helpers, icao_id_lut */

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace app {

/* =========================================================================
 * The protocol layer, free of any UI. Ported from common/adsb.cpp's
 * encode_frame_*() functions, byte for byte, onto app::adsb::AdsbFrame.
 * ========================================================================= */
namespace adsb_tx {

/* DF=17, CA=5, ICAO address — the common header every DF17 frame starts
 * with (upstream's make_frame_adsb()). */
void make_frame_adsb(adsb::AdsbFrame& frame, uint32_t icao_address);

/* TC=4 aircraft identification / category message: an 8-character callsign
 * from the 6-bit alphabet in app::adsb::icao_id_lut. Characters outside the
 * alphabet, and any position past the end of a short callsign, encode as
 * space — upstream indexes a caller-padded 8-character buffer unconditionally
 * and this is the same behaviour without requiring the caller to pad. */
void encode_frame_id(adsb::AdsbFrame& frame, uint32_t icao_address, const std::string& callsign);

/* TC=11 airborne position: one call per parity (time_parity 0 = even,
 * 1 = odd). Two frames, one of each parity, are what an RX needs to combine
 * via app::adsb::decode_frame_pos() into a lat/lon fix — see
 * tests/test_voradsbepirb_tx.cpp for that exact round trip. `altitude` is
 * feet, `latitude`/`longitude` decimal degrees. */
void encode_frame_pos(adsb::AdsbFrame& frame, uint32_t icao_address, int32_t altitude,
                      double latitude, double longitude, uint32_t time_parity);

/* TC=19 airborne velocity, subtype 1 (subsonic ground speed): `speed` in
 * knots, `heading_deg` a compass bearing (0 = north, clockwise), `v_rate` in
 * ft/min (positive = climb). */
void encode_frame_velo(adsb::AdsbFrame& frame, uint32_t icao_address, uint32_t speed,
                       double heading_deg, int32_t v_rate);

/* DF=21 Comm-B altitude/identity reply carrying a 4-digit octal squawk code
 * (e.g. 7700) plus a fixed BDS 5,0 track-and-turn MB field, matching
 * upstream's encode_frame_squawk() exactly (including its hard-coded MB
 * payload). */
void encode_frame_squawk(adsb::AdsbFrame& frame, uint16_t squawk);

/* --- Modulation ------------------------------------------------------------
 *
 * 1 Mbit/s Mode S PPM at 4 Msps (2 samples per 500 ns chip), matching
 * upstream's baseband_thread rate exactly. The preamble (16 chips, not
 * Manchester-coded) plus the Manchester-coded 112-bit payload is 240 chips =
 * 480 samples, the same length upstream's ADSBTXProcessor produces per
 * frame (bit_pos wraps at 240 << 1). Envelope amplitude 1.0 for a chip 1,
 * 0.0 for a chip 0 — real on/off keying rather than upstream's spinning
 * 4-point constellation (see the file header). */
constexpr double kChipRateHz = 2'000'000.0;
constexpr double kDefaultSampleRateHz = 4'000'000.0;
constexpr size_t kPreambleChips = 16;
constexpr size_t kPayloadBits = 112;
constexpr size_t kChipsPerFrame = kPreambleChips + (kPayloadBits * 2);

std::vector<dsp::cfloat> adsb_frame_waveform(const adsb::AdsbFrame& frame,
                                             double sample_rate_hz = kDefaultSampleRateHz,
                                             float amplitude = 1.0f);

}  // namespace adsb_tx

/* =========================================================================
 * Views
 * ========================================================================= */

class ADSBPositionTab : public ui::OptionTabView {
   public:
    ADSBPositionTab(ui::Rect parent_rect);

    void collect_frames(uint32_t icao_address, std::vector<adsb::AdsbFrame>& out);

   private:
    ui::GeoPos geopos_{{0, 2 * 16}, ui::GeoPos::FEET, ui::GeoPos::HIDDEN};
};

class ADSBCallsignTab : public ui::OptionTabView {
   public:
    ADSBCallsignTab(ui::Rect parent_rect);

    void collect_frames(uint32_t icao_address, std::vector<adsb::AdsbFrame>& out);

   private:
    std::string callsign_{"TEST1234"};

    ui::Labels labels_{{{2 * 8, 2 * 16}, "Callsign:", ui::Color::light_grey()}};
    ui::Button button_callsign_{{11 * 8, 2 * 16 - 4, 12 * 8, 24}, "TEST1234"};
};

class ADSBSpeedTab : public ui::OptionTabView {
   public:
    ADSBSpeedTab(ui::Rect parent_rect);

    void collect_frames(uint32_t icao_address, std::vector<adsb::AdsbFrame>& out);

   private:
    ui::Labels labels_{
        {{1 * 8, 2 * 16}, "Speed(kt):", ui::Color::light_grey()},
        {{1 * 8, 4 * 16}, "Heading:", ui::Color::light_grey()},
        {{1 * 8, 6 * 16}, "V/S(ft/min):", ui::Color::light_grey()},
    };
    ui::NumberField field_speed_{{13 * 8, 2 * 16}, 3, {0, 999}, 5, ' '};
    ui::NumberField field_heading_{{13 * 8, 4 * 16}, 3, {0, 359}, 1, ' ', true};
    ui::NumberField field_vrate_{{13 * 8, 6 * 16}, 5, {-4096, 4096}, 64, ' '};
};

class ADSBSquawkTab : public ui::OptionTabView {
   public:
    ADSBSquawkTab(ui::Rect parent_rect);

    void collect_frames(uint32_t icao_address, std::vector<adsb::AdsbFrame>& out);

   private:
    ui::Labels labels_{{{2 * 8, 2 * 16}, "Squawk:", ui::Color::light_grey()}};
    ui::SymField field_squawk_{{10 * 8, 2 * 16}, 4, ui::SymField::Type::Oct};
};

class ADSBTxView : public ui::View {
   public:
    ADSBTxView();
    ~ADSBTxView() override;

    ADSBTxView(const ADSBTxView&) = delete;
    ADSBTxView& operator=(const ADSBTxView&) = delete;

    std::string title() const override { return "ADS-B TX"; }

    void focus() override;
    void on_frame_sync() override;

   private:
    void generate_frames();
    void request_start();
    void start_tx();
    void stop_tx();

    bool transmitting_{false};
    bool tx_acknowledged_{false};
    uint32_t preview_counter_{0};

    std::vector<adsb::AdsbFrame> frames_{};
    std::vector<dsp::cfloat> tx_waveform_{};
    std::atomic<size_t> tx_pos_{0};

    ui::Labels labels_{
        {{2 * 8, 0}, "ICAO24:", ui::Color::light_grey()},
    };

    ui::SymField sym_icao_{{10 * 8, 0}, 6, ui::SymField::Type::Hex};

    ui::FrequencyField field_freq_{{2 * 8, 16}};
    ui::FrequencyStepView step_view_{{2 * 8 + 11 * 8, 16}, field_freq_};

    ui::Labels warning_{
        {{0, 56}, "TX injects a fake aircraft.", ui::Color::red()},
        {{0, 72}, "Illegal almost everywhere.", ui::Color::red()},
        {{0, 88}, "No hardware: RF unverified.", ui::Color::grey()},
    };

    ui::Rect tab_content_rect_{0, 104, 240, 140};
    ADSBPositionTab tab_position_{tab_content_rect_};
    ADSBCallsignTab tab_callsign_{tab_content_rect_};
    ADSBSpeedTab tab_speed_{tab_content_rect_};
    ADSBSquawkTab tab_squawk_{tab_content_rect_};

    ui::TabView tab_view_{
        {"Position", ui::Color::cyan(), &tab_position_},
        {"Callsign", ui::Color::green(), &tab_callsign_},
        {"Speed", ui::Color::yellow(), &tab_speed_},
        {"Squawk", ui::Color::orange(), &tab_squawk_},
    };

    ui::Text text_frame_{{0, 248, 240, 16}, "-"};
    ui::Text text_status_{{0, 264, 240, 16}, "Idle"};

    ui::Button button_tx_{{0, 282, 240, 20}, "Start"};
};

}  // namespace app

#endif /*__MB200_UI_ADSB_TX_H__*/
