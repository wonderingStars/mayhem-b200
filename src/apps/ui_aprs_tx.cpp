/*
 * mayhem-b200 — APRS transmitter.
 *
 * See ui_aprs_tx.hpp for what this is ported from, the one deliberate parameter
 * (the TXDelay preamble length), the legality note and the no-hardware caveat.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_aprs_tx.hpp"

#include "../audio/audio_out.hpp"  /* audio::sample_rate */
#include "../core/string_format.hpp"
#include "../dsp/demod_digital.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_alphanum.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <cstring>

namespace app {

namespace {

/* SymField::set_value right-aligns and pads with spaces, and slot editing can
 * leave spaces anywhere, so the callsign read back from the widget may carry
 * leading, trailing or embedded spaces. A callsign never legitimately contains
 * one, and AX.25 pads the address field with spaces on the right regardless, so
 * stripping them here makes both a pre-filled default and an edited field encode
 * the same clean callsign. (Upstream passes the raw widget string through; this
 * is a host robustness fix, noted in the header's faithfulness section.) */
std::string strip_spaces(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in)
        if (c != ' ') out += c;
    return out;
}

}  // namespace

/* ===========================================================================
 * AprsTxFrame
 * ===========================================================================*/

void AprsTxFrame::reset() {
    bits_.clear();
    frame_bytes_.clear();
    crc_.reset();
    fcs_ = 0;
    level_ = 0;
    ones_ = 0;
}

/* NRZI: a data 0 flips the line, a data 1 leaves it. The stored level is the
 * physical bit — mark (1200 Hz) for 1, space (2200 Hz) for 0 downstream. */
void AprsTxFrame::nrzi_emit(uint8_t bit) {
    if (!bit) level_ ^= 1;
    bits_.push_back(level_);
}

/* Port of AX25Frame::add_byte: LSB-first, CRC only over data bytes, a stuffed
 * zero after five consecutive ones unless the byte is a flag. */
void AprsTxFrame::add_byte(uint8_t byte, bool is_flag, bool is_data) {
    if (is_data) {
        crc_.process_byte(byte);
        frame_bytes_.push_back(byte);
    }

    for (uint32_t i = 0; i < 8; i++) {
        const uint8_t bit = static_cast<uint8_t>((byte >> i) & 1);
        nrzi_emit(bit);

        if (bit) {
            ones_++;
            if ((ones_ == 5) && !is_flag) {
                nrzi_emit(0);
                ones_ = 0;
            }
        } else {
            ones_ = 0;
        }
    }
}

/* Port of AX25Frame::make_extended_field: every address byte is shifted left by
 * one, and the last byte of the last address group carries the HDLC extension
 * bit in its LSB. */
void AprsTxFrame::make_extended_field(const uint8_t* data, size_t length, bool is_last) {
    if (length == 0) return;

    size_t i = 0;
    for (i = 0; i < length - 1; i++)
        add_data(static_cast<uint8_t>(data[i] << 1));

    if (is_last)
        add_data(static_cast<uint8_t>((data[i] << 1) | 1));
    else
        add_data(static_cast<uint8_t>(data[i] << 1));
}

void AprsTxFrame::add_checksum() {
    fcs_ = static_cast<uint16_t>(crc_.checksum());
    add_byte(static_cast<uint8_t>(fcs_ & 0xFF), false, false);
    add_byte(static_cast<uint8_t>((fcs_ >> 8) & 0xFF), false, false);
}

std::string AprsTxFrame::build_address(const std::string& dest,
                                       uint8_t dest_ssid,
                                       const std::string& src,
                                       uint8_t src_ssid) {
    std::string address(14, ' ');

    const size_t dlen = std::min<size_t>(dest.size(), 6);
    for (size_t i = 0; i < dlen; i++) address[i] = dest[i];
    const size_t slen = std::min<size_t>(src.size(), 6);
    for (size_t i = 0; i < slen; i++) address[7 + i] = src[i];

    address[6] = static_cast<char>((dest_ssid & 0x0F) | 0x30);
    address[13] = static_cast<char>((src_ssid & 0x0F) | 0x30);
    return address;
}

std::string AprsTxFrame::fix_path(const std::string& path) {
    std::string fixed;
    const char* p = path.c_str();

    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        char call[6] = {' ', ' ', ' ', ' ', ' ', ' '};
        int idx = 0;

        while (*p && *p != '-' && *p != ',') {
            if (*p != ' ') {
                if (idx < 6) {
                    char c = *p;
                    if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
                    call[idx++] = c;
                }
            }
            p++;
        }

        int ssid = 0;
        if (*p == '-') {
            p++;
            while (*p == ' ') p++;
            while (*p >= '0' && *p <= '9') {
                ssid = ssid * 10 + (*p - '0');
                p++;
            }
        }

        if (ssid >= 0 && ssid <= 15) {
            for (int i = 0; i < 6; i++) fixed += call[i];
            fixed += static_cast<char>(ssid | 0x30);
        }

        while (*p && *p != ',') p++;
        if (*p == ',') p++;
    }

    return fixed;
}

std::string AprsTxFrame::format_coordinates(float latitude, float longitude) {
    std::string s;

    const char ns = latitude >= 0 ? 'N' : 'S';
    const float lat = (latitude >= 0) ? latitude : -latitude;
    uint32_t lat_d = static_cast<uint32_t>(lat);
    uint32_t lat_m = static_cast<uint32_t>((lat - lat_d) * 6000.0f + 0.5f);
    if (lat_m >= 6000) {
        lat_m = 0;
        lat_d++;
    }
    s += to_string_dec_uint(lat_d, 2, '0');
    s += to_string_dec_uint(lat_m / 100, 2, '0');
    s += ".";
    s += to_string_dec_uint(lat_m % 100, 2, '0');
    s += ns;

    s += "/";

    const char ew = longitude >= 0 ? 'E' : 'W';
    const float lon = (longitude >= 0) ? longitude : -longitude;
    uint32_t lon_d = static_cast<uint32_t>(lon);
    uint32_t lon_m = static_cast<uint32_t>((lon - lon_d) * 6000.0f + 0.5f);
    if (lon_m >= 6000) {
        lon_m = 0;
        lon_d++;
    }
    s += to_string_dec_uint(lon_d, 3, '0');
    s += to_string_dec_uint(lon_m / 100, 2, '0');
    s += ".";
    s += to_string_dec_uint(lon_m % 100, 2, '0');
    s += ew;

    return s;
}

void AprsTxFrame::build(const std::string& src,
                        uint8_t src_ssid,
                        const std::string& dest,
                        uint8_t dest_ssid,
                        const std::string& payload,
                        const std::string& path,
                        size_t lead_flags,
                        size_t trail_flags) {
    reset();

    const std::string address = build_address(dest, dest_ssid, src, src_ssid);
    const std::string fixed_path = fix_path(path);

    /* Upstream's has_path guard from make_ui_frame: a whole number of 7-byte
     * address groups, at most nine of them. */
    const bool has_path =
        (!fixed_path.empty() && fixed_path.size() <= 63 && (fixed_path.size() % 7 == 0));

    for (size_t i = 0; i < lead_flags; i++) add_flag();

    make_extended_field(reinterpret_cast<const uint8_t*>(address.data()), 14, !has_path);
    if (has_path)
        make_extended_field(reinterpret_cast<const uint8_t*>(fixed_path.data()),
                            fixed_path.size(), true);

    add_data(0x03);  /* UI control */
    add_data(0xF0);  /* PID: no layer 3 */

    for (char c : payload) add_data(static_cast<uint8_t>(c));

    add_checksum();

    for (size_t i = 0; i < trail_flags; i++) add_flag();
}

/* ===========================================================================
 * AprsTxView
 * ===========================================================================*/

AprsTxView::AprsTxView() {
    add_children({&labels_,
                  &sym_source_,
                  &num_ssid_source_,
                  &sym_dest_,
                  &num_ssid_dest_,
                  &field_path_,
                  &field_payload_,
                  &field_lat_,
                  &field_lon_,
                  &button_gps_,
                  &text_gps_,
                  &field_frequency_,
                  &field_step_,
                  &field_gain_,
                  &legality_,
                  &console_,
                  &button_tx_,
                  &button_stop_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_frequency_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                                   static_cast<uint64_t>(caps.tx_freq.max));
        field_gain_.set_range(static_cast<int32_t>(caps.tx_gain.min),
                              static_cast<int32_t>(caps.tx_gain.max));
    }

    sym_source_.set_value(std::string_view{src_call_});
    sym_dest_.set_value(std::string_view{dest_call_});
    num_ssid_source_.set_value(0, false);
    num_ssid_dest_.set_value(0, false);

    field_path_.set_text(path_);
    field_payload_.set_text(payload_.empty() ? std::string{"(set info)"} : payload_);

    /* Upstream's radio_state default: 144.390 MHz, North-American APRS. */
    field_frequency_.set_step_index(2);  /* 100 Hz */
    field_frequency_.set_value(144'390'000, false);

    if (auto* tx = globals().transmitter)
        field_gain_.set_value(static_cast<int32_t>(tx->gain()), false);

    field_lat_.set_value(0.0f, false);
    field_lon_.set_value(0.0f, false);

    sym_source_.on_change = [this](ui::SymField& f) {
        src_call_ = strip_spaces(f.to_string());
        update_preview();
    };
    sym_dest_.on_change = [this](ui::SymField& f) {
        dest_call_ = strip_spaces(f.to_string());
        update_preview();
    };
    num_ssid_source_.on_change = [this](int32_t) { update_preview(); };
    num_ssid_dest_.on_change = [this](int32_t) { update_preview(); };

    field_path_.on_select = [this](ui::TextField&) {
        auto* nav = globals().nav;
        if (nav == nullptr) return;
        prompt_buffer_ = path_;
        ui::text_prompt(*nav, prompt_buffer_, 60, ENTER_KEYBOARD_MODE_ALPHA,
                        [this](std::string& s) {
                            path_ = s;
                            field_path_.set_text(path_);
                            update_preview();
                        });
    };

    field_payload_.on_select = [this](ui::TextField&) {
        auto* nav = globals().nav;
        if (nav == nullptr) return;
        prompt_buffer_ = payload_;
        ui::text_prompt(*nav, prompt_buffer_, 63, ENTER_KEYBOARD_MODE_ALPHA,
                        [this](std::string& s) {
                            payload_ = s;
                            field_payload_.set_text(payload_.empty() ? std::string{"(set info)"}
                                                                     : payload_);
                            update_preview();
                        });
    };

    button_gps_.on_select = [this](ui::Button&) {
        const std::string coord =
            AprsTxFrame::format_coordinates(field_lat_.value(), field_lon_.value());
        text_gps_.set(coord);
        update_preview();
    };

    field_frequency_.on_change = [this](uint64_t hz) {
        if (auto* tx = globals().transmitter) tx->set_target_frequency(hz);
    };
    field_gain_.on_change = [this](int32_t db) {
        if (auto* tx = globals().transmitter) tx->set_gain(static_cast<double>(db));
    };

    button_tx_.on_select = [this](ui::Button&) { start_tx(); };
    button_stop_.on_select = [this](ui::Button&) { stop_tx(); };

    console_.enable_scrolling(true);
    log(STR_COLOR_LIGHT_GREY "APRS 1200bd Bell202 AFSK / NBFM.");
    log(STR_COLOR_LIGHT_GREY "Use ?GPS? in Info for the position.");
    log(STR_COLOR_LIGHT_GREY "RF output needs a USRP B200.");

    update_preview();
}

AprsTxView::~AprsTxView() {
    stop_tx();
}

void AprsTxView::focus() {
    sym_source_.focus();
}

std::string AprsTxView::current_payload() const {
    /* Port of APRSTXView::start_tx's ?GPS? substitution. */
    std::string out = payload_;
    const std::string token = "?GPS?";
    const std::string coord = text_gps_.get();
    size_t pos = 0;
    while ((pos = out.find(token, pos)) != std::string::npos) {
        out.replace(pos, token.size(), coord);
        pos += coord.size();
    }
    return out;
}

std::string AprsTxView::tnc2_line() const {
    /* The TNC2 monitor form, built straight from the fields so the operator sees
     * exactly what will be keyed before pressing TX. */
    std::string line = src_call_;
    const int ssid_src = num_ssid_source_.value();
    if (ssid_src != 0) line += "-" + to_string_dec_uint(static_cast<uint64_t>(ssid_src));
    line += ">";
    line += dest_call_;
    const int ssid_dst = num_ssid_dest_.value();
    if (ssid_dst != 0) line += "-" + to_string_dec_uint(static_cast<uint64_t>(ssid_dst));
    if (!path_.empty()) line += "," + path_;
    line += ":" + current_payload();
    return line;
}

void AprsTxView::build_frame() {
    frame_.build(src_call_,
                 static_cast<uint8_t>(num_ssid_source_.value()),
                 dest_call_,
                 static_cast<uint8_t>(num_ssid_dest_.value()),
                 current_payload(),
                 path_,
                 kPreambleFlags,
                 /*trail_flags*/ 3);
}

void AprsTxView::update_preview() {
    build_frame();
    console_.clear(true);
    log(STR_COLOR_CYAN + tnc2_line());
    log(STR_COLOR_LIGHT_GREY "Frame " +
        to_string_dec_uint(frame_.frame_bytes().size()) + "B  FCS " +
        to_string_hex(frame_.fcs(), 4) + "  " +
        to_string_dec_uint(frame_.bits().size()) + " bits");
}

void AprsTxView::start_tx() {
    if (transmitting_.load()) return;

    if (src_call_.empty()) {
        log(STR_COLOR_RED "Set a source callsign first.");
        return;
    }

    auto* tx = globals().transmitter;
    if (tx == nullptr) {
        log(STR_COLOR_RED "No transmitter wired.");
        log(STR_COLOR_RED "Needs a USRP B200.");
        return;
    }

    build_frame();

    /* AX.25 NRZI physical bits -> Bell 202 AFSK audio at the model's 48 kHz
     * audio-source rate, then NBFM. Leading/trailing silence keeps the ring from
     * clipping the first and last tones. */
    const auto& bits = frame_.bits();
    tx_audio_ = dsp::afsk_modulate(bits, static_cast<float>(audio::sample_rate),
                                   kMarkHz, kSpaceHz, kBaud, 0.8f);
    const size_t tail = audio::sample_rate / 10;  /* 100 ms */
    tx_audio_.insert(tx_audio_.begin(), tail, 0.0f);
    tx_audio_.insert(tx_audio_.end(), tail, 0.0f);
    tx_pos_.store(0);

    tx->set_mode(radio::TransmitterModel::Mode::NarrowbandFM);
    tx->set_nfm_configuration(radio::TransmitterModel::NfmConfig::Medium11k);
    tx->set_deviation(kDeviationHz);
    tx->set_target_frequency(field_frequency_.value());
    tx->set_gain(static_cast<double>(field_gain_.value()));
    tx->set_audio_gain(1.0f);

    tx->set_audio_source([this](float* out, size_t n) -> size_t {
        const size_t pos = tx_pos_.load();
        const size_t total = tx_audio_.size();
        const size_t avail = (pos < total) ? (total - pos) : 0;
        const size_t k = std::min(n, avail);
        for (size_t i = 0; i < k; i++) out[i] = tx_audio_[pos + i];
        tx_pos_.store(pos + k);
        return k;
    });

    if (!tx->start()) {
        log(STR_COLOR_RED "TX start failed (no B200?).");
        tx->set_audio_source(nullptr);
        return;
    }

    transmitting_.store(true);
    tail_frames_ = 0;
    log(STR_COLOR_GREEN "Transmitting...");
}

void AprsTxView::stop_tx() {
    const bool was = transmitting_.exchange(false);
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_audio_source(nullptr);
    }
    if (was) log(STR_COLOR_LIGHT_GREY "Idle.");
}

void AprsTxView::on_frame_sync() {
    View::on_frame_sync();
    if (!transmitting_.load()) return;

    /* Done once the whole buffer (frame + trailing silence) has been pulled.
     * Give it a few extra frames so the TX ring fully drains before stopping. */
    if (tx_pos_.load() >= tx_audio_.size()) {
        if (++tail_frames_ >= 8) {
            stop_tx();
            log(STR_COLOR_GREEN "Sent.");
        }
    }
}

void AprsTxView::log(std::string_view line) {
    console_.writeln(line);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream menu_location is TX; its tile is the APRS glyph. */
const app::Registrar reg_aprs_tx{{"aprstx", "APRS TX", app::Category::Transmit,
                                  ui::Color::orange(), &ui::bitmap_icon_aprs,
                                  [] { return std::make_unique<app::AprsTxView>(); }}};
}  // namespace
