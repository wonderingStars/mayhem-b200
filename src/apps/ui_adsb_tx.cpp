/*
 * mayhem-b200 — ADS-B / Mode S TX.
 *
 * See ui_adsb_tx.hpp for what was ported, from where, and what changed.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_adsb_tx.hpp"

#include "../dsp/demod_digital.hpp" /* dsp::ook_modulate */
#include "../dsp/protocol.hpp"      /* dsp::manchester_encode */
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "string_format.hpp"
#include "ui_alphanum.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

#include <cmath>
#include <cstdlib>

namespace app {
namespace adsb_tx {

namespace {
constexpr double kPi = 3.14159265358979323846;

/* Upstream's adsb_preamble (common/adsb_frame.hpp): the 8 us preamble, NOT
 * Manchester-coded — a chip is 500 ns, so this is 16 chips = 8 us. */
constexpr uint8_t kPreamble[kPreambleChips] = {1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0};

/* Unpacks `bit_count` bits from `data`, MSB first, one entry (0/1) per bit —
 * the shape dsp::manchester_encode() and dsp::ook_modulate() both take. */
std::vector<uint8_t> unpack_bits_msb(const uint8_t* data, size_t bit_count) {
    std::vector<uint8_t> bits(bit_count);
    for (size_t i = 0; i < bit_count; i++)
        bits[i] = static_cast<uint8_t>((data[i >> 3] >> (7 - (i & 7))) & 1u);
    return bits;
}
}  // namespace

void make_frame_adsb(adsb::AdsbFrame& frame, uint32_t icao_address) {
    frame.clear();
    frame.push_byte(static_cast<uint8_t>((adsb::DF_ADSB << 3) | 5));
    frame.push_byte(static_cast<uint8_t>((icao_address >> 16) & 0xFF));
    frame.push_byte(static_cast<uint8_t>((icao_address >> 8) & 0xFF));
    frame.push_byte(static_cast<uint8_t>(icao_address & 0xFF));
}

void encode_frame_id(adsb::AdsbFrame& frame, uint32_t icao_address, const std::string& callsign) {
    make_frame_adsb(frame, icao_address);
    frame.push_byte(static_cast<uint8_t>(adsb::TC_IDENT << 3));

    uint64_t callsign_coded = 0;
    for (size_t c = 0; c < 8; c++) {
        const char ch = (c < callsign.size()) ? callsign[c] : ' ';
        size_t s = 32; /* space, upstream's fallback for an invalid character */
        for (size_t k = 0; k < 64; k++) {
            if (ch == adsb::icao_id_lut[k]) {
                s = k;
                break;
            }
        }
        callsign_coded = (callsign_coded << 6) | s;
    }

    for (size_t c = 0; c < 6; c++)
        frame.push_byte(static_cast<uint8_t>((callsign_coded >> ((5 - c) * 8)) & 0xFF));

    frame.make_CRC();
}

void encode_frame_pos(adsb::AdsbFrame& frame, uint32_t icao_address, int32_t altitude,
                      double latitude, double longitude, uint32_t time_parity) {
    make_frame_adsb(frame, icao_address);
    frame.push_byte(static_cast<uint8_t>(adsb::TC_AIRBORNE_POS << 3));

    uint32_t altitude_coded = static_cast<uint32_t>((altitude + 1000) / 25);
    altitude_coded = ((altitude_coded & 0x7F0u) << 1) | 0x10u | (altitude_coded & 0x0Fu);
    frame.push_byte(static_cast<uint8_t>(altitude_coded >> 4));

    /* CPR encoding — the exact inverse of app::adsb::decode_frame_pos(), and
     * built from the same cpr_mod/cpr_NL/cpr_N this project's RX side uses,
     * so a pair of even/odd frames from this function decodes back to the
     * source lat/lon through app::adsb::decode_frame_pos() unchanged (see
     * tests/test_voradsbepirb_tx.cpp). */
    const double delta_lat = 360.0 / ((4.0 * adsb::NZ) - static_cast<double>(time_parity));
    const double yz =
        std::floor(adsb::CPR_MAX_VALUE * (adsb::cpr_mod(latitude, delta_lat) / delta_lat) + 0.5);
    const double rlat = delta_lat * ((yz / adsb::CPR_MAX_VALUE) + std::floor(latitude / delta_lat));

    double delta_lon;
    if ((adsb::cpr_NL(rlat) - static_cast<int>(time_parity)) > 0)
        delta_lon = 360.0 / adsb::cpr_N(rlat, static_cast<int>(time_parity));
    else
        delta_lon = 360.0;
    const double xz =
        std::floor(adsb::CPR_MAX_VALUE * (adsb::cpr_mod(longitude, delta_lon) / delta_lon) + 0.5);

    const uint32_t lat = static_cast<uint32_t>(adsb::cpr_mod(yz, adsb::CPR_MAX_VALUE));
    const uint32_t lon = static_cast<uint32_t>(adsb::cpr_mod(xz, adsb::CPR_MAX_VALUE));

    frame.push_byte(static_cast<uint8_t>((altitude_coded << 4) | (time_parity << 2) | (lat >> 15)));
    frame.push_byte(static_cast<uint8_t>(lat >> 7));
    frame.push_byte(static_cast<uint8_t>((lat << 1) | (lon >> 16)));
    frame.push_byte(static_cast<uint8_t>(lon >> 8));
    frame.push_byte(static_cast<uint8_t>(lon));

    frame.make_CRC();
}

void encode_frame_velo(adsb::AdsbFrame& frame, uint32_t icao_address, uint32_t speed,
                       double heading_deg, int32_t v_rate) {
    const double rad = heading_deg * kPi / 180.0;
    const int32_t velo_ew = static_cast<int32_t>(std::lround(std::sin(rad) * speed));
    const int32_t velo_ns = static_cast<int32_t>(std::lround(std::cos(rad) * speed));

    const uint32_t v_rate_coded_abs = static_cast<uint32_t>((std::abs(v_rate) / 64) + 1);
    const uint32_t velo_ew_abs = static_cast<uint32_t>(std::abs(velo_ew) + 1);
    const uint32_t velo_ns_abs = static_cast<uint32_t>(std::abs(velo_ns) + 1);

    make_frame_adsb(frame, icao_address);
    frame.push_byte(static_cast<uint8_t>((adsb::TC_AIRBORNE_VELO << 3) | 1));
    frame.push_byte(static_cast<uint8_t>(((velo_ew < 0 ? 1 : 0) << 2) | (velo_ew_abs >> 8)));
    frame.push_byte(static_cast<uint8_t>(velo_ew_abs));
    frame.push_byte(static_cast<uint8_t>(((velo_ns < 0 ? 1 : 0) << 7) | (velo_ns_abs >> 3)));
    frame.push_byte(static_cast<uint8_t>((velo_ns_abs << 5) | ((v_rate < 0 ? 1 : 0) << 3) |
                                         (v_rate_coded_abs >> 6)));
    frame.push_byte(static_cast<uint8_t>(v_rate_coded_abs << 2));
    frame.push_byte(0);

    frame.make_CRC();
}

void encode_frame_squawk(adsb::AdsbFrame& frame, uint16_t squawk) {
    constexpr uint8_t UM_field = 0b111101, FS = 0b010, DR = 0b00001;

    frame.clear();
    frame.push_byte(static_cast<uint8_t>((adsb::DF_EHS_SQUAWK << 3) | FS));
    frame.push_byte(static_cast<uint8_t>((DR << 3) | (UM_field >> 3)));

    const uint16_t squawk_coded = static_cast<uint16_t>(
        (((UM_field & 0b111u) << 13) | ((squawk << 9) & 0x1000u)) | ((squawk << 2) & 0x0800u) |
        ((squawk << 6) & 0x0400u) | ((squawk >> 1) & 0x0200u) | ((squawk << 3) & 0x0100u) |
        ((squawk >> 4) & 0x0080u) | ((squawk >> 1) & 0x0020u) | ((squawk << 4) & 0x0010u) |
        ((squawk >> 4) & 0x0008u) | ((squawk << 1) & 0x0004u) | ((squawk >> 7) & 0x0002u) |
        ((squawk >> 2) & 0x0001u));

    frame.push_byte(static_cast<uint8_t>(squawk_coded >> 8));
    frame.push_byte(static_cast<uint8_t>(squawk_coded));

    /* Fixed BDS 5,0 track-and-turn MB field, verbatim from upstream. */
    static constexpr uint8_t mb[7] = {0xF9, 0x36, 0x3D, 0x3B, 0xBF, 0x9C, 0xE9};
    for (uint8_t b : mb) frame.push_byte(b);

    frame.make_CRC();
}

std::vector<dsp::cfloat> adsb_frame_waveform(const adsb::AdsbFrame& frame, double sample_rate_hz,
                                             float amplitude) {
    const size_t data_bits = frame.is_long() ? 112 : 56;
    const auto databits = unpack_bits_msb(frame.get_raw_data(), data_bits);
    const auto payload_chips = dsp::manchester_encode(databits, 0);

    std::vector<uint8_t> chips;
    chips.reserve(kPreambleChips + payload_chips.size());
    chips.insert(chips.end(), std::begin(kPreamble), std::end(kPreamble));
    chips.insert(chips.end(), payload_chips.begin(), payload_chips.end());

    return dsp::ook_modulate(chips, static_cast<float>(sample_rate_hz),
                             static_cast<float>(kChipRateHz), amplitude);
}

}  // namespace adsb_tx

/* --- Tabs ---------------------------------------------------------------- */

ADSBPositionTab::ADSBPositionTab(ui::Rect parent_rect) : ui::OptionTabView{parent_rect} {
    set_type("position");
    add_children({&geopos_});
    geopos_.set_altitude(36000);
}

void ADSBPositionTab::collect_frames(uint32_t icao_address, std::vector<adsb::AdsbFrame>& out) {
    if (!is_enabled()) return;

    adsb::AdsbFrame frame{};
    adsb_tx::encode_frame_pos(frame, icao_address, geopos_.altitude(), geopos_.lat(),
                              geopos_.lon(), 0);
    out.push_back(frame);

    adsb_tx::encode_frame_pos(frame, icao_address, geopos_.altitude(), geopos_.lat(),
                              geopos_.lon(), 1);
    out.push_back(frame);
}

ADSBCallsignTab::ADSBCallsignTab(ui::Rect parent_rect) : ui::OptionTabView{parent_rect} {
    set_type("callsign");
    set_enabled(true);
    add_children({&labels_, &button_callsign_});
    button_callsign_.set_text(callsign_);

    button_callsign_.on_select = [this](ui::Button&) {
        auto* nav = globals().nav;
        if (!nav) return;
        ui::text_prompt(*nav, callsign_, 8, ENTER_KEYBOARD_MODE_ALPHA, [this](std::string& s) {
            button_callsign_.set_text(s);
        });
    };
}

void ADSBCallsignTab::collect_frames(uint32_t icao_address, std::vector<adsb::AdsbFrame>& out) {
    if (!is_enabled()) return;

    adsb::AdsbFrame frame{};
    adsb_tx::encode_frame_id(frame, icao_address, callsign_);
    out.push_back(frame);
}

ADSBSpeedTab::ADSBSpeedTab(ui::Rect parent_rect) : ui::OptionTabView{parent_rect} {
    set_type("speed");
    add_children({&labels_, &field_speed_, &field_heading_, &field_vrate_});
    field_speed_.set_value(400, false);
    field_heading_.set_value(0, false);
    field_vrate_.set_value(0, false);
}

void ADSBSpeedTab::collect_frames(uint32_t icao_address, std::vector<adsb::AdsbFrame>& out) {
    if (!is_enabled()) return;

    adsb::AdsbFrame frame{};
    adsb_tx::encode_frame_velo(frame, icao_address, static_cast<uint32_t>(field_speed_.value()),
                               field_heading_.value(), field_vrate_.value());
    out.push_back(frame);
}

ADSBSquawkTab::ADSBSquawkTab(ui::Rect parent_rect) : ui::OptionTabView{parent_rect} {
    set_type("squawk");
    add_children({&labels_, &field_squawk_});
}

void ADSBSquawkTab::collect_frames(uint32_t /*icao_address*/, std::vector<adsb::AdsbFrame>& out) {
    if (!is_enabled()) return;

    adsb::AdsbFrame frame{};
    adsb_tx::encode_frame_squawk(frame, static_cast<uint16_t>(field_squawk_.to_integer()));
    out.push_back(frame);
}

/* --- ADSBTxView ------------------------------------------------------------ */

namespace {
/* One frame every 50 ms, matching upstream's ADSBTXThread cycle. */
constexpr double kFramePeriodSeconds = 0.050;
}  // namespace

ADSBTxView::ADSBTxView() {
    add_children({&labels_, &sym_icao_, &field_freq_, &step_view_, &warning_, &tab_view_,
                  &tab_position_, &tab_callsign_, &tab_speed_, &tab_squawk_, &text_frame_,
                  &text_status_, &button_tx_});

    tab_view_.set_selected(0);

    sym_icao_.set_value(static_cast<uint64_t>(0xA00001));

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_freq_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                              static_cast<uint64_t>(caps.tx_freq.max));
    }
    field_freq_.set_value(1'090'000'000, false);
    field_freq_.on_change = [](uint64_t hz) {
        if (auto* tx = globals().transmitter) tx->set_target_frequency(hz);
    };

    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            request_start();
    };

    /* Show what will be sent before the operator ever presses Start. */
    generate_frames();
}

ADSBTxView::~ADSBTxView() {
    if (transmitting_) stop_tx();
}

void ADSBTxView::focus() {
    tab_view_.focus();
}

void ADSBTxView::generate_frames() {
    const uint32_t icao_address = static_cast<uint32_t>(sym_icao_.to_integer());

    frames_.clear();
    tab_position_.collect_frames(icao_address, frames_);
    tab_callsign_.collect_frames(icao_address, frames_);
    tab_speed_.collect_frames(icao_address, frames_);
    tab_squawk_.collect_frames(icao_address, frames_);

    text_frame_.set(to_string_dec_uint(frames_.size()) + " frame(s) armed.");
}

void ADSBTxView::request_start() {
    if (tx_acknowledged_) {
        start_tx();
        return;
    }
    ui::display_modal(
        "TX WARNING",
        "ADS-B TX injects a false\naircraft into real air traffic\nsurveillance. Illegal almost\neverywhere. TX only into a\nshielded dummy load.",
        ui::YESNO, [this](bool choice) {
            if (choice) {
                tx_acknowledged_ = true;
                start_tx();
            }
        });
}

void ADSBTxView::start_tx() {
    auto* tx = globals().transmitter;
    if (!tx) {
        text_status_.set(STR_COLOR_RED "No transmitter wired.");
        return;
    }

    generate_frames();
    if (frames_.empty()) {
        text_status_.set(STR_COLOR_YELLOW "No frame types enabled.");
        return;
    }

    const double fs = adsb_tx::kDefaultSampleRateHz;
    const size_t slot_samples = static_cast<size_t>(std::llround(kFramePeriodSeconds * fs));

    tx_waveform_.clear();
    tx_waveform_.reserve(frames_.size() * slot_samples);
    for (const auto& frame : frames_) {
        const auto burst = adsb_tx::adsb_frame_waveform(frame, fs);
        const size_t pad = (burst.size() < slot_samples) ? (slot_samples - burst.size()) : 0;
        tx_waveform_.insert(tx_waveform_.end(), burst.begin(), burst.end());
        tx_waveform_.insert(tx_waveform_.end(), pad, dsp::cfloat{0.0f, 0.0f});
    }
    tx_pos_.store(0);

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(fs);
    tx->set_target_frequency(field_freq_.value());
    tx->set_iq_source([this](dsp::cfloat* out, size_t n) -> size_t {
        if (tx_waveform_.empty()) return 0;
        size_t pos = tx_pos_.load();
        for (size_t i = 0; i < n; i++) {
            out[i] = tx_waveform_[pos];
            pos++;
            if (pos >= tx_waveform_.size()) pos = 0;
        }
        tx_pos_.store(pos);
        return n;
    });

    if (!tx->start()) {
        text_status_.set(STR_COLOR_YELLOW "TX start failed (needs B200).");
        tx->set_iq_source(nullptr);
        return;
    }

    transmitting_ = true;
    button_tx_.set_text("Stop");
    text_status_.set(STR_COLOR_GREEN "TX " + to_string_dec_uint(frames_.size()) + " frame(s)");
}

void ADSBTxView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    transmitting_ = false;
    button_tx_.set_text("Start");
    text_status_.set("Idle");
}

void ADSBTxView::on_frame_sync() {
    View::on_frame_sync();

    /* Keep the "what will be sent" preview live as the operator edits tabs,
     * without wiring an on_change callback into every field. Skipped while
     * transmitting so it cannot race the frames start_tx() already armed. */
    if (!transmitting_ && (++preview_counter_ % 30) == 0) generate_frames();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_adsbtx{{"adsbtx", "ADS-B TX", app::Category::Transmit,
                                 ui::Color::green(), &ui::bitmap_icon_adsb,
                                 [] { return std::make_unique<app::ADSBTxView>(); }}};
}  // namespace
