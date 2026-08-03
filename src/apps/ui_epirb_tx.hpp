/*
 * mayhem-b200 — 406 MHz COSPAS-SARSAT distress beacon TX (EPIRB/ELT/PLB).
 *
 * Ported from firmware/application/external/epirb_tx/ (beacon.hpp's
 * generate_beacon(), location.hpp's decimal<->DMS conversion, ui_epirb_tx.*)
 * and firmware/baseband/proc_epirb.* for the on-air format.
 *
 * This port carries over the FIRST GENERATION BEACON (FGB) frame only —
 * upstream's manual generator (the scope this task asks for: "the
 * beacon-ID/position fields"). The newer Second Generation Beacon (SGB,
 * T.018) format upstream also added is not ported; a B200 test setup gains
 * nothing from it that FGB does not already exercise, and it would double
 * the surface area with no protocol lesson left to demonstrate.
 *
 * Frame layout (1-based bit numbers, matching C/S T.001 and this project's
 * ui_epirb_rx.hpp's Beacon::get_bits()):
 *
 *      1 ..  15   bit sync, all ones
 *     16 ..  24   frame sync: 000101111 normal, 011010000 self-test
 *          25     format flag: 1 = long frame (144 bits)
 *          26     protocol flag: 1 = user protocol, 0 = location protocol
 *     27 ..  36   country code (maritime identification digits)
 *     37 ..  40   protocol code
 *     41 ..  85   identification data / position (protocol-dependent)
 *     86 .. 106   BCH-1, 21-bit check over bits 25..85
 *    107 .. 132   supplementary data (position offsets)
 *    133 .. 144   BCH-2, 12-bit check over bits 107..132
 *
 * BCH-1 generator: x^21+x^18+x^17+x^15+x^14+x^12+x^11+x^8+x^7+x^6+x^5+x+1
 * BCH-2 generator: x^12+x^10+x^8+x^5+x^4+x^3+1
 *
 * Both polynomials, and the compute_bch() bit-shift-register algorithm that
 * evaluates them, are reused directly from this project's EPIRB RX
 * (app::epirb::kBch21Polynomial / kBch12Polynomial, ui_epirb_rx.hpp) rather
 * than re-derived, so a generated frame is checked for correctness the same
 * way this project's receiver would check it: build the frame here, decode
 * it with app::epirb::Beacon::set_frame(), and frame_valid() must be true —
 * see tests/test_voradsbepirb_tx.cpp.
 *
 * ON AIR: a burst is 160 ms of unmodulated carrier, then the 144 data bits
 * biphase-L (Manchester) encoded at 400 bps (800 chips/s) and phase-shift
 * keyed +/-1.1 rad about the carrier, then 100 ms more of carrier — the same
 * numbers this project's EPIRB RX demodulator (ui_epirb_rx.hpp epirb::
 * Demodulator) expects on the way in.
 *
 * LEGALITY: transmitting on 406 MHz triggers a real COSPAS-SARSAT distress
 * alert routed to search-and-rescue authorities. Doing so without genuine
 * distress is a serious crime in every jurisdiction that has ratified the
 * treaty. This view defaults to the self-test frame flag, shows a
 * persistent on-screen warning, and requires an explicit acknowledgement
 * before the first transmission; it never starts on its own. No hardware is
 * attached during development, so actual radiation is unverified — say so.
 *
 * Copyright (C) 2024 EPIRB Receiver Implementation
 * Copyright (C) 2026 Frederic BORRY - ADRASEC 31 (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_EPIRB_TX_H__
#define __MB200_UI_EPIRB_TX_H__

#include "../dsp/demod.hpp" /* dsp::cfloat */
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_geomap.hpp"
#include "ui_widget.hpp"
#include "ui_epirb_rx.hpp" /* app::epirb::{kBch21Polynomial, kBch12Polynomial, Beacon} */

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace app {
namespace epirb_tx {

enum class BeaconType : uint8_t { EPIRB = 0, ELT = 1, PLB = 2 };
enum class BeaconProtocol : uint8_t { USER = 0, STANDARD = 1, NATIONAL = 2 };

/* Sexagesimal position, the fields generate_beacon() actually consumes. */
struct Location {
    bool south{false};
    int16_t lat_deg{0};
    int8_t lat_min{0};
    int8_t lat_sec{0};
    bool west{false};
    int16_t long_deg{0};
    int8_t long_min{0};
    int8_t long_sec{0};
};

struct BeaconParams {
    BeaconType type{BeaconType::EPIRB};
    BeaconProtocol protocol{BeaconProtocol::STANDARD};
    uint32_t country{227}; /* maritime identification digits, e.g. 227 = France */
    bool is_test{true};
    bool is_internal{true};
    bool has_121_5{true};
    Location location{};
};

/* Upstream's location.hpp decimal_to_dms(): decimal degrees -> sign +
 * degrees/minutes/seconds, rounding the seconds and carrying into minutes
 * and degrees on a 60-second or 60-minute rollover. */
void decimal_to_dms(double value, bool& negative, int16_t& deg, int8_t& min, int8_t& sec);

/* Builds the 18-byte (144-bit) FGB frame per C/S T.001, including both BCH
 * checks. Byte-for-byte port of beacon.hpp's generate_beacon(); always
 * returns 18. */
size_t generate_beacon(std::array<uint8_t, 18>& frame, const BeaconParams& params);

/* --- Modulation ------------------------------------------------------------
 *
 * 160 ms carrier, then the 144 bits biphase-L (Manchester, sense 0) encoded
 * to 288 chips at 800 chips/s and phase-keyed +/-1.1 rad, then 100 ms more
 * of carrier — C/S T.001's numbers, matching what
 * ui_epirb_rx.hpp's epirb::Demodulator expects on receive. */
constexpr double kChipRateHz = 800.0;
constexpr double kPhaseDeviationRad = 1.1;
constexpr double kPreCarrierSeconds = 0.160;
constexpr double kPostCarrierSeconds = 0.100;
constexpr double kDefaultSampleRateHz = 48000.0;

std::vector<dsp::cfloat> epirb_frame_waveform(const std::array<uint8_t, 18>& frame,
                                              double sample_rate_hz = kDefaultSampleRateHz);

}  // namespace epirb_tx

class EpirbTxView : public ui::View {
   public:
    EpirbTxView();
    ~EpirbTxView() override;

    EpirbTxView(const EpirbTxView&) = delete;
    EpirbTxView& operator=(const EpirbTxView&) = delete;

    std::string title() const override { return "EPIRB TX"; }

    void focus() override;
    void on_frame_sync() override;

   private:
    void update_frame();
    void request_start();
    void start_tx();
    void stop_tx();

    epirb_tx::BeaconParams params_{};
    std::array<uint8_t, 18> frame_{};

    bool transmitting_{false};
    bool tx_acknowledged_{false};

    std::vector<dsp::cfloat> tx_waveform_{};
    std::atomic<size_t> tx_pos_{0};

    ui::Labels labels_{
        {{0, 0}, "Type", ui::Color::light_grey()},
        {{120, 0}, "Protocol", ui::Color::light_grey()},
        {{0, 20}, "Country", ui::Color::light_grey()},
    };

    ui::OptionsField field_type_{
        {40, 0}, 7, {{"EPIRB", 0}, {"ELT", 1}, {"PLB", 2}}};
    ui::OptionsField field_protocol_{
        {184, 0}, 4, {{"Usr", 0}, {"Std", 1}, {"Nat", 2}}};
    ui::NumberField field_country_{{64, 20}, 3, {0, 799}, 1, '0'};

    ui::Checkbox check_test_{{0, 38}, 10, "Self-test", true};
    ui::Checkbox check_internal_{{120, 38}, 10, "Internal", true};
    ui::Checkbox check_121_5_{{0, 56}, 20, "121.5/243 homing device", true};

    /* Altitude isn't part of the FGB frame; the field is left in place
     * (unused by generate_beacon()) rather than fighting the shared widget
     * to remove it. */
    ui::GeoPos geopos_{{0, 78}, ui::GeoPos::METERS, ui::GeoPos::HIDDEN};

    ui::Labels freq_label_{{{0, 128}, "Freq", ui::Color::light_grey()}};
    ui::FrequencyField field_freq_{{40, 128}};
    ui::FrequencyStepView step_view_{{148, 128}, field_freq_};

    ui::Labels warning_{
        {{0, 150}, "406 MHz triggers a REAL SAR", ui::Color::red()},
        {{0, 166}, "distress alert. Crime unless", ui::Color::red()},
        {{0, 182}, "genuinely testing (test flag).", ui::Color::red()},
    };

    ui::Text text_frame1_{{0, 204, 240, 16}, "-"};
    ui::Text text_frame2_{{0, 220, 240, 16}, "-"};
    ui::Text text_valid_{{0, 236, 240, 16}, "-"};

    ui::Text text_status_{{0, 260, 240, 16}, "Idle"};

    ui::Button button_tx_{{0, 280, 240, 22}, "Start"};
};

}  // namespace app

#endif /*__MB200_UI_EPIRB_TX_H__*/
