/*
 * mayhem-b200 — 406 MHz COSPAS-SARSAT distress beacon TX.
 *
 * See ui_epirb_tx.hpp for what was ported, from where, and what changed.
 *
 * Copyright (C) 2024 EPIRB Receiver Implementation
 * Copyright (C) 2026 Frederic BORRY - ADRASEC 31 (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_epirb_tx.hpp"

#include "../dsp/protocol.hpp" /* dsp::manchester_encode */
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "string_format.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

#include <cmath>
#include <cstring>

namespace app {
namespace epirb_tx {

namespace {

/* --- Bit-level helpers, byte-for-byte from beacon.hpp -------------------- */

void set_bit(uint8_t* buf, int bit, bool v) {
    const int byte_idx = bit >> 3;
    const int off = 7 - (bit & 7);
    if (v)
        buf[byte_idx] = static_cast<uint8_t>(buf[byte_idx] | (1u << off));
    else
        buf[byte_idx] = static_cast<uint8_t>(buf[byte_idx] & ~(1u << off));
}

void push_bits(uint8_t* buf, int& pos, uint64_t v, int n) {
    for (int i = n - 1; i >= 0; i--) set_bit(buf, pos++, ((v >> i) & 1u) != 0);
}

/* 1-based inclusive bit range, MSB first — the same convention as
 * app::epirb::Beacon::get_bits() (ui_epirb_rx.hpp), so compute_bch() below
 * evaluates identically to the RX side's compute_bch1()/compute_bch2(). */
uint64_t get_bits(const uint8_t* data, int start_bit, int end_bit) {
    uint64_t result = 0;
    start_bit--;
    const int num_bits = end_bit - start_bit;
    if (num_bits <= 0) return 0;

    size_t byte_index = static_cast<size_t>(start_bit) / 8;
    uint8_t b = data[byte_index];
    int bit_offset = 7 - (start_bit % 8);

    for (int i = 0; i < num_bits; ++i) {
        result <<= 1;
        result |= (b >> bit_offset) & 1u;
        if (--bit_offset < 0) {
            ++byte_index;
            b = data[byte_index];
            bit_offset = 7;
        }
    }
    return result;
}

uint64_t compute_bch(const uint8_t* frame, int start_bit, int end_bit, uint32_t poly,
                     int poly_length) {
    const int data_length = end_bit - start_bit + 1;
    const int total_length = data_length + poly_length - 1;
    uint64_t result = get_bits(frame, start_bit, start_bit + poly_length - 1);
    for (int i = poly_length; i <= total_length; i++) {
        const bool first_bit = (result >> (poly_length - 1)) != 0;
        if (first_bit) result ^= poly;
        if (i < total_length) {
            result <<= 1;
            if (i < data_length) result |= get_bits(frame, start_bit + i, start_bit + i);
        }
    }
    return result;
}

std::vector<uint8_t> unpack_bits_msb(const uint8_t* data, size_t bit_count) {
    std::vector<uint8_t> bits(bit_count);
    for (size_t i = 0; i < bit_count; i++)
        bits[i] = static_cast<uint8_t>((data[i >> 3] >> (7 - (i & 7))) & 1u);
    return bits;
}

}  // namespace

void decimal_to_dms(double value, bool& negative, int16_t& deg, int8_t& min, int8_t& sec) {
    negative = value < 0.0;
    value = std::fabs(value);

    deg = static_cast<int16_t>(value);
    const double m = (value - deg) * 60.0;
    min = static_cast<int8_t>(m);
    const double s = (m - min) * 60.0;
    sec = static_cast<int8_t>(s + 0.5);

    if (sec == 60) {
        sec = 0;
        min++;
    }
    if (min == 60) {
        min = 0;
        deg++;
    }
}

size_t generate_beacon(std::array<uint8_t, 18>& frame, const BeaconParams& params) {
    frame.fill(0);
    uint8_t* buf = frame.data();
    int pos = 0;
    uint32_t deg, min, sec;

    for (int i = 0; i < 15; i++) push_bits(buf, pos, 1, 1); /* bit sync */

    push_bits(buf, pos, params.is_test ? 0b011010000u : 0b000101111u, 9); /* frame sync */

    const int pdf1_start = pos;
    push_bits(buf, pos, 1, 1); /* format flag: long */
    const bool is_user = (params.protocol == BeaconProtocol::USER);
    const bool is_standard = (params.protocol == BeaconProtocol::STANDARD);
    push_bits(buf, pos, is_user ? 1u : 0u, 1); /* protocol flag */
    push_bits(buf, pos, params.country, 10);

    switch (params.type) {
        case BeaconType::EPIRB:
            if (is_user)
                push_bits(buf, pos, 0b010u, 3);
            else if (is_standard)
                push_bits(buf, pos, 0b0010u, 4);
            else
                push_bits(buf, pos, 0b1010u, 4);
            break;
        case BeaconType::PLB:
            if (is_user)
                push_bits(buf, pos, 0b011u, 3);
            else if (is_standard)
                push_bits(buf, pos, 0b0111u, 4);
            else
                push_bits(buf, pos, 0b1011u, 4);
            break;
        default:
        case BeaconType::ELT:
            if (is_user)
                push_bits(buf, pos, 0b001u, 3);
            else if (is_standard)
                push_bits(buf, pos, 0b0011u, 4);
            else
                push_bits(buf, pos, 0b1000u, 4);
            break;
    }

    while (pos < pdf1_start + 61) push_bits(buf, pos, 0, 1);

    if (is_user) {
        if (params.type == BeaconType::PLB) {
            set_bit(buf, 40 - 1, true);
            set_bit(buf, 41 - 1, true);
            set_bit(buf, 42 - 1, false);
        }
        set_bit(buf, 85 - 1, params.has_121_5);
    } else if (is_standard) {
        set_bit(buf, 65 - 1, params.location.south);
        set_bit(buf, 75 - 1, params.location.west);
        pos = 66 - 1;
        deg = static_cast<uint32_t>((params.location.lat_deg << 2) + (params.location.lat_min / 15));
        push_bits(buf, pos, deg, 9);
        pos = 76 - 1;
        deg = static_cast<uint32_t>((params.location.long_deg << 2) + (params.location.long_min / 15));
        push_bits(buf, pos, deg, 10);
        pos = pdf1_start + 61;
    } else {
        set_bit(buf, 59 - 1, params.location.south);
        set_bit(buf, 72 - 1, params.location.west);
        pos = 60 - 1;
        push_bits(buf, pos, static_cast<uint32_t>(params.location.lat_deg), 7);
        min = static_cast<uint32_t>(params.location.lat_min / 2);
        push_bits(buf, pos, min, 5);
        pos = 73 - 1;
        push_bits(buf, pos, static_cast<uint32_t>(params.location.long_deg), 8);
        min = static_cast<uint32_t>(params.location.long_min / 2);
        push_bits(buf, pos, min, 5);
        pos = pdf1_start + 61;
    }

    const uint64_t bch1 = compute_bch(buf, 25, 85, epirb::kBch21Polynomial, epirb::kBch21PolyLength);
    push_bits(buf, pos, bch1, 21);

    const int pdf2_start = pos;
    if (is_user) {
        push_bits(buf, pos, params.is_internal ? 1u : 0u, 1);
        push_bits(buf, pos, params.location.south ? 1u : 0u, 1);
        push_bits(buf, pos, static_cast<uint32_t>(params.location.lat_deg), 7);
        min = static_cast<uint32_t>(params.location.lat_min / 4);
        push_bits(buf, pos, min, 4);
        push_bits(buf, pos, params.location.west ? 1u : 0u, 1);
        push_bits(buf, pos, static_cast<uint32_t>(params.location.long_deg), 8);
        min = static_cast<uint32_t>(params.location.long_min / 4);
        push_bits(buf, pos, min, 4);
    } else if (is_standard) {
        push_bits(buf, pos, 0b1101u, 4);
        push_bits(buf, pos, params.is_internal ? 1u : 0u, 1);
        push_bits(buf, pos, params.has_121_5 ? 1u : 0u, 1);
        push_bits(buf, pos, 1u, 1);
        min = static_cast<uint32_t>(params.location.lat_min % 15);
        push_bits(buf, pos, min, 5);
        sec = static_cast<uint32_t>(params.location.lat_sec / 4);
        push_bits(buf, pos, sec, 4);
        push_bits(buf, pos, 1u, 1);
        min = static_cast<uint32_t>(params.location.long_min % 15);
        push_bits(buf, pos, min, 5);
        sec = static_cast<uint32_t>(params.location.long_sec / 4);
        push_bits(buf, pos, sec, 4);
    } else {
        push_bits(buf, pos, 0b1101u, 4);
        push_bits(buf, pos, params.is_internal ? 1u : 0u, 1);
        push_bits(buf, pos, params.has_121_5 ? 1u : 0u, 1);
        push_bits(buf, pos, 1u, 1);
        push_bits(buf, pos, static_cast<uint32_t>(params.location.lat_min % 2), 2);
        sec = static_cast<uint32_t>(params.location.lat_sec / 4);
        push_bits(buf, pos, sec, 4);
        push_bits(buf, pos, 1u, 1);
        push_bits(buf, pos, static_cast<uint32_t>(params.location.long_min % 2), 2);
        sec = static_cast<uint32_t>(params.location.long_sec / 4);
        push_bits(buf, pos, sec, 4);
    }

    while (pos < pdf2_start + 26) push_bits(buf, pos, 0, 1);

    const uint64_t bch2 = compute_bch(buf, 107, 132, epirb::kBch12Polynomial, epirb::kBch12PolyLength);
    push_bits(buf, pos, bch2, 12);

    return 18;
}

std::vector<dsp::cfloat> epirb_frame_waveform(const std::array<uint8_t, 18>& frame,
                                              double sample_rate_hz) {
    const auto databits = unpack_bits_msb(frame.data(), 144);
    const auto chips = dsp::manchester_encode(databits, 0); /* 288 chips */

    size_t samples_per_chip = static_cast<size_t>(std::llround(sample_rate_hz / kChipRateHz));
    if (samples_per_chip < 1) samples_per_chip = 1;
    const size_t pre_samples =
        static_cast<size_t>(std::llround(kPreCarrierSeconds * sample_rate_hz));
    const size_t post_samples =
        static_cast<size_t>(std::llround(kPostCarrierSeconds * sample_rate_hz));

    std::vector<dsp::cfloat> out;
    out.reserve(pre_samples + (chips.size() * samples_per_chip) + post_samples);

    out.insert(out.end(), pre_samples, dsp::cfloat{1.0f, 0.0f});

    for (uint8_t chip : chips) {
        const double phase = chip ? kPhaseDeviationRad : -kPhaseDeviationRad;
        const dsp::cfloat s{static_cast<float>(std::cos(phase)), static_cast<float>(std::sin(phase))};
        out.insert(out.end(), samples_per_chip, s);
    }

    out.insert(out.end(), post_samples, dsp::cfloat{1.0f, 0.0f});
    return out;
}

}  // namespace epirb_tx

/* --- View -------------------------------------------------------------- */

EpirbTxView::EpirbTxView() {
    add_children({&labels_, &field_type_, &field_protocol_, &field_country_, &check_test_,
                  &check_internal_, &check_121_5_, &geopos_, &freq_label_, &field_freq_,
                  &step_view_, &warning_, &text_frame1_, &text_frame2_, &text_valid_,
                  &text_status_, &button_tx_});

    field_type_.set_selected_index(0, false);
    field_protocol_.set_selected_index(1, false); /* Standard, matches params_ default */
    field_country_.set_value(static_cast<int32_t>(params_.country), false);

    check_test_.set_value(params_.is_test);
    check_internal_.set_value(params_.is_internal);
    check_121_5_.set_value(params_.has_121_5);

    geopos_.set_altitude(0);
    geopos_.set_lat(48.0f);
    geopos_.set_lon(2.0f);

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_freq_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                              static_cast<uint64_t>(caps.tx_freq.max));
    }
    /* 406.037 MHz — one of the C/S T.001 test/allocation channels. */
    field_freq_.set_value(406'037'000, false);
    field_freq_.on_change = [](uint64_t hz) {
        if (auto* tx = globals().transmitter) tx->set_target_frequency(hz);
    };

    field_type_.on_change = [this](size_t, int32_t v) {
        params_.type = static_cast<epirb_tx::BeaconType>(v);
        update_frame();
    };
    field_protocol_.on_change = [this](size_t, int32_t v) {
        params_.protocol = static_cast<epirb_tx::BeaconProtocol>(v);
        update_frame();
    };
    field_country_.on_change = [this](int32_t v) {
        params_.country = static_cast<uint32_t>(v);
        update_frame();
    };
    check_test_.on_select = [this](ui::Checkbox&, bool v) {
        params_.is_test = v;
        update_frame();
    };
    check_internal_.on_select = [this](ui::Checkbox&, bool v) {
        params_.is_internal = v;
        update_frame();
    };
    check_121_5_.on_select = [this](ui::Checkbox&, bool v) {
        params_.has_121_5 = v;
        update_frame();
    };
    geopos_.on_change = [this](int32_t, float lat, float lon, int32_t) {
        epirb_tx::decimal_to_dms(lat, params_.location.south, params_.location.lat_deg,
                                 params_.location.lat_min, params_.location.lat_sec);
        epirb_tx::decimal_to_dms(lon, params_.location.west, params_.location.long_deg,
                                 params_.location.long_min, params_.location.long_sec);
        update_frame();
    };

    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            request_start();
    };

    update_frame();
}

EpirbTxView::~EpirbTxView() {
    if (transmitting_) stop_tx();
}

void EpirbTxView::focus() {
    field_type_.focus();
}

void EpirbTxView::update_frame() {
    epirb_tx::generate_beacon(frame_, params_);

    text_frame1_.set(to_string_hex_array(frame_.data(), 9));
    text_frame2_.set(to_string_hex_array(frame_.data() + 9, 9));

    /* Decode with this project's own RX-side Beacon parser as a live
     * sanity check — if this ever reads "KO" the frame this view is about
     * to transmit does not check out against the receiver this project
     * ships. */
    epirb::Beacon decoded;
    decoded.set_frame(frame_.data());
    text_valid_.set(decoded.frame_valid() ? STR_COLOR_GREEN "BCH OK" : STR_COLOR_RED "BCH FAIL");
}

void EpirbTxView::request_start() {
    if (tx_acknowledged_) {
        start_tx();
        return;
    }
    ui::display_modal(
        "TX WARNING",
        "406 MHz transmission triggers\na REAL distress alert to SAR\nauthorities. Serious crime\nunless genuinely testing.",
        ui::YESNO, [this](bool choice) {
            if (choice) {
                tx_acknowledged_ = true;
                start_tx();
            }
        });
}

void EpirbTxView::start_tx() {
    auto* tx = globals().transmitter;
    if (!tx) {
        text_status_.set(STR_COLOR_RED "No transmitter wired.");
        return;
    }

    update_frame();

    const double fs = epirb_tx::kDefaultSampleRateHz;
    tx_waveform_ = epirb_tx::epirb_frame_waveform(frame_, fs);
    tx_pos_.store(0);

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(fs);
    tx->set_target_frequency(field_freq_.value());
    tx->set_iq_source([this](dsp::cfloat* out, size_t n) -> size_t {
        const size_t pos = tx_pos_.load();
        const size_t remaining = (pos < tx_waveform_.size()) ? (tx_waveform_.size() - pos) : 0;
        const size_t take = (n < remaining) ? n : remaining;
        for (size_t i = 0; i < take; i++) out[i] = tx_waveform_[pos + i];
        tx_pos_.store(pos + take);
        return take;
    });

    if (!tx->start()) {
        text_status_.set(STR_COLOR_YELLOW "TX start failed (needs B200).");
        tx->set_iq_source(nullptr);
        return;
    }

    transmitting_ = true;
    button_tx_.set_text("Stop");
    text_status_.set(STR_COLOR_GREEN "TX burst sending...");
}

void EpirbTxView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    transmitting_ = false;
    button_tx_.set_text("Start");
    text_status_.set("Idle");
}

void EpirbTxView::on_frame_sync() {
    View::on_frame_sync();

    if (transmitting_ && tx_pos_.load() >= tx_waveform_.size()) {
        text_status_.set(STR_COLOR_GREEN "Burst sent.");
        stop_tx();
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream: menu_location TX, icon_color green. */
const app::Registrar reg_epirb_tx{{"epirb_tx", "EPIRB TX", app::Category::Transmit,
                                   ui::Color::green(), &ui::bitmap_icon_sonde,
                                   [] { return std::make_unique<app::EpirbTxView>(); }}};
}  // namespace
