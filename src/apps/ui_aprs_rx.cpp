/*
 * mayhem-b200 — APRS receiver.
 *
 * See ui_aprs_rx.hpp for what this is ported from and what the two deliberate
 * departures from upstream are.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_aprs_rx.hpp"

#include "../core/string_format.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace app {

/* ===========================================================================
 * AX.25 frame check sequence
 * ===========================================================================*/

namespace {

/* The one place the CRC parameters are written down. Upstream's transmitter
 * spells the same thing `CRC<16, true, true> crc_ccitt{0x1021, 0xFFFF, 0xFFFF}`
 * (application/protocols/ax25.hpp); its receiver spells it as a reflected
 * 0x8408 lookup table (baseband/proc_aprsrx.hpp). */
dsp::Crc<16, true, true> make_ax25_crc() {
    return dsp::Crc<16, true, true>{0x1021, 0xFFFF, 0xFFFF};
}

/* Residue over data+FCS. Upstream tests its reflected register against 0xF0B8;
 * dsp::Crc keeps the register un-reflected, where that value is 0x1D0F. */
constexpr uint32_t kAx25Residue = 0x1D0F;

}  // namespace

uint16_t ax25_fcs(const uint8_t* data, size_t length) {
    auto crc = make_ax25_crc();
    if (data != nullptr && length > 0) crc.process_bytes(data, length);
    return static_cast<uint16_t>(crc.checksum());
}

bool ax25_frame_valid(const uint8_t* frame, size_t length) {
    if (frame == nullptr || length < 2) return false;
    auto crc = make_ax25_crc();
    crc.process_bytes(frame, length);
    return crc.remainder() == kAx25Residue;
}

/* ===========================================================================
 * AprsPacket
 * ===========================================================================*/

void AprsPacket::clear() {
    payload_size_ = 0;
    valid_checksum_ = false;
}

void AprsPacket::set(size_t index, uint8_t data) {
    if (index >= kAprsMaxLength) return;
    payload_[index] = data;
    if (index + 1 > payload_size_) payload_size_ = index + 1;
}

void AprsPacket::set_bytes(const uint8_t* data, size_t length) {
    clear();
    if (data == nullptr) return;
    const size_t n = std::min(length, kAprsMaxLength);
    for (size_t i = 0; i < n; i++) payload_[i] = data[i];
    payload_size_ = n;
}

uint8_t AprsPacket::operator[](size_t index) const {
    return (index < kAprsMaxLength) ? payload_[index] : uint8_t{0};
}

uint64_t AprsPacket::get_source() const {
    uint64_t source = 0;
    for (size_t i = kSourceStart; i < kSourceStart + kAddressSize; i++) {
        source |= (static_cast<uint64_t>(payload_[i]) << ((i - kSourceStart) * 8));
    }
    return source;
}

/* Upstream's parse_address, with its fixed char[15] scratch buffer replaced by
 * an out-parameter string (the buffer was written with an explicit '\0' and
 * then wrapped in std::string, so this is the same result). */
bool AprsPacket::parse_address(size_t start, AprsAddressType type, std::string& out) const {
    out.clear();
    if (payload_size_ < 2) return false;

    /* Upstream bounds the walk at payload_size - 2 so the FCS bytes are never
     * mistaken for address characters. */
    const size_t limit = payload_size_ - 2;
    bool has_more = false;

    for (size_t i = start; i < start + kAddressSize && i < limit; i++) {
        uint8_t byte = payload_[i];

        if (i - start == 6) {
            has_more = (byte & 0x01) == 0;
            const uint8_t ssid = static_cast<uint8_t>((byte >> 1) & 0x0F);

            /* Digipeaters always show their SSID, even when it is zero — that
             * is how upstream renders the WIDEn-N path. */
            if (ssid != 0 || type == AprsAddressType::Repeater) {
                out += '-';
                if (ssid < 10) {
                    out += static_cast<char>('0' + ssid);
                } else {
                    out += '1';
                    out += static_cast<char>('0' + ssid - 10);
                }
            }
        } else {
            byte = static_cast<uint8_t>(byte >> 1);
            if (byte != ' ') out += static_cast<char>(byte);
        }
    }

    return has_more;
}

std::string AprsPacket::get_source_formatted() const {
    std::string out;
    parse_address(kSourceStart, AprsAddressType::Source, out);
    return out;
}

std::string AprsPacket::get_destination_formatted() const {
    std::string out;
    parse_address(kDestinationStart, AprsAddressType::Destination, out);
    return out;
}

std::string AprsPacket::get_digipeaters_formatted() const {
    std::string scratch;

    /* The source address's extension bit says whether any digipeaters follow. */
    bool has_more = parse_address(kSourceStart, AprsAddressType::Repeater, scratch);
    if (!has_more) return "";

    size_t position = kDigipeaterStart;
    std::string repeaters = ",";
    while (has_more) {
        has_more = parse_address(position, AprsAddressType::Repeater, scratch);
        repeaters += scratch;
        position += kAddressSize;
        if (has_more) repeaters += ",";
    }

    return repeaters;
}

size_t AprsPacket::get_number_of_digipeaters() const {
    std::string scratch;
    bool has_more = parse_address(kSourceStart, AprsAddressType::Repeater, scratch);

    size_t position = kDigipeaterStart;
    size_t repeaters = 0;
    while (has_more) {
        has_more = parse_address(position, AprsAddressType::Repeater, scratch);
        position += kAddressSize;
        repeaters++;
    }

    return repeaters;
}

size_t AprsPacket::get_information_start_index() const {
    /* Addresses, then the control byte and the PID byte. */
    return kDigipeaterStart + (get_number_of_digipeaters() * kAddressSize) + 2;
}

std::string AprsPacket::get_information_text_formatted() const {
    std::string information_text;
    if (payload_size_ < 2) return information_text;

    const size_t end = payload_size_ - 2;
    for (size_t i = get_information_start_index(); i < end; i++) {
        information_text += static_cast<char>(payload_[i]);
    }

    return information_text;
}

std::string AprsPacket::get_stream_text() const {
    /* get_digipeaters_formatted() supplies its own leading comma. */
    return get_source_formatted() + ">" + get_destination_formatted() +
           get_digipeaters_formatted() + ":" + get_information_text_formatted();
}

char AprsPacket::get_data_type_identifier() const {
    if (payload_size_ < 2) return '\0';
    const size_t start = get_information_start_index();
    if (start >= payload_size_ - 2) return '\0';
    return static_cast<char>(payload_[start]);
}

bool AprsPacket::has_position() const {
    const char ident = get_data_type_identifier();

    return ident == '!' ||
           ident == '=' ||
           ident == '/' ||
           ident == '@' ||
           ident == ';' ||
           ident == '`' ||
           ident == '\'' ||
           ident == '\x1d' ||
           ident == '\x1c';
}

uint32_t AprsPacket::parse_digits(const std::string& str) {
    if (str.empty() || str.at(0) == ' ') return 0;

    const auto last = str.find_last_not_of(' ');
    if (last == std::string::npos) return 0;
    const std::string sub = str.substr(0, last + 1);

    if (sub.find_last_not_of("0123456789") != std::string::npos) return 0;
    return static_cast<uint32_t>(std::atoi(sub.c_str()));
}

/* "DDMM.hhN" -> degrees. */
float AprsPacket::parse_lat_str(const std::string& lat_str) {
    if (lat_str.size() < 8) return 0.0f;

    const uint32_t lat_deg = parse_digits(lat_str.substr(0, 2));
    const uint32_t lat_min = parse_digits(lat_str.substr(2, 2));
    const uint32_t lat_hund = parse_digits(lat_str.substr(5, 2));

    float lat = static_cast<float>(lat_deg);
    lat += static_cast<float>((lat_min + (lat_hund / 100.0)) / 60.0);

    if (lat_str.at(7) == 'S') lat = -lat;
    return lat;
}

/* "DDDMM.hhW" -> degrees. */
float AprsPacket::parse_lng_str(const std::string& lng_str) {
    if (lng_str.size() < 9) return 0.0f;

    const uint32_t lng_deg = parse_digits(lng_str.substr(0, 3));
    const uint32_t lng_min = parse_digits(lng_str.substr(3, 2));
    const uint32_t lng_hund = parse_digits(lng_str.substr(6, 2));

    float lng = static_cast<float>(lng_deg);
    lng += static_cast<float>((lng_min + (lng_hund / 100.0)) / 60.0);

    if (lng_str.at(8) == 'W') lng = -lng;
    return lng;
}

/* Base-91 compressed latitude, APRS 1.0.1 chapter 9:
 *
 *     lat = 90 - (y1*91^3 + y2*91^2 + y3*91 + y4) / 380926,  yn = char - 33
 *
 * DEPARTURE: upstream omits the "- 33" on the fourth digit, which biases every
 * compressed latitude by 33/380926 degrees (~9.6 m) and every longitude by
 * 33/190463 degrees (~19 m at the equator). Corrected here; see the header. */
float AprsPacket::parse_lat_str_cmp(const std::string& lat_str) {
    if (lat_str.size() < 4) return 0.0f;
    const double y = (lat_str.at(0) - 33) * (91.0 * 91.0 * 91.0) +
                     (lat_str.at(1) - 33) * (91.0 * 91.0) +
                     (lat_str.at(2) - 33) * 91.0 +
                     (lat_str.at(3) - 33);
    return static_cast<float>(90.0 - y / 380926.0);
}

float AprsPacket::parse_lng_str_cmp(const std::string& lng_str) {
    if (lng_str.size() < 4) return 0.0f;
    const double x = (lng_str.at(0) - 33) * (91.0 * 91.0 * 91.0) +
                     (lng_str.at(1) - 33) * (91.0 * 91.0) +
                     (lng_str.at(2) - 33) * 91.0 +
                     (lng_str.at(3) - 33);
    return static_cast<float>(-180.0 + x / 190463.0);
}

AprsPosition AprsPacket::get_position() const {
    AprsPosition pos{};

    const char ident = get_data_type_identifier();
    const std::string info_text = get_information_text_formatted();

    size_t start = 0;
    bool is_mic_e_format = false;

    switch (ident) {
        case '/':
        case '@':
            start = 8; /* data type identifier + 7-character timestamp */
            break;
        case '=':
        case '!':
            start = 1;
            break;
        case ';':
            start = 18; /* identifier + 9-character object name + '*' + timestamp */
            break;
        case '`':
        case '\'':
        case '\x1c':
        case '\x1d':
            is_mic_e_format = true;
            break;
        default:
            return pos;
    }

    if (is_mic_e_format) {
        parse_mic_e_format(pos);
        return pos;
    }

    if (start >= info_text.size()) return pos;

    const char first = info_text.at(start);

    if (std::isdigit(static_cast<unsigned char>(first))) {
        /* Uncompressed: DDMM.hhN/DDDMM.hhW$ */
        if (start + 18 < info_text.size()) {
            pos.sym_table_id = static_cast<uint8_t>(info_text.at(start + 8));
            pos.symbol_code = static_cast<uint8_t>(info_text.at(start + 18));
            pos.latitude = parse_lat_str(info_text.substr(start, 8));
            pos.longitude = parse_lng_str(info_text.substr(start + 9, 9));
        }
    } else {
        /* Compressed: /YYYYXXXX$cs T */
        if (start + 9 < info_text.size()) {
            pos.sym_table_id = static_cast<uint8_t>(info_text.at(start));
            pos.symbol_code = static_cast<uint8_t>(info_text.at(start + 9));
            pos.latitude = parse_lat_str_cmp(info_text.substr(start + 1, 4));
            pos.longitude = parse_lng_str_cmp(info_text.substr(start + 5, 4));
        }
    }

    return pos;
}

/* Mic-E: latitude and the N/S, longitude-offset and E/W flags are hidden in the
 * (shifted) destination address; the longitude degrees/minutes/hundredths are
 * the first three bytes of the information field after the identifier. */
void AprsPacket::parse_mic_e_format(AprsPosition& pos) const {
    std::string lat_str;

    bool is_north = false;
    bool is_west = false;
    uint8_t lng_offset = 0;

    for (size_t i = kDestinationStart; i < kDestinationStart + kAddressSize - 1; i++) {
        const uint8_t ascii = static_cast<uint8_t>(payload_[i] >> 1);
        const size_t index = i - kDestinationStart;

        lat_str += static_cast<char>(mic_e_lat_digit(ascii));

        if (index == 3) {
            lat_str += ".";
            is_north = mic_e_lat_is_north(ascii);
        }
        if (index == 4) lng_offset = mic_e_lng_offset(ascii);
        if (index == 5) is_west = mic_e_lng_is_west(ascii);
    }

    lat_str += is_north ? "N" : "S";
    pos.latitude = parse_lat_str(lat_str);

    double lng = 0.0;
    const size_t information_start = get_information_start_index() + 1;
    const size_t end = (payload_size_ >= 2) ? (payload_size_ - 2) : 0;

    for (size_t i = information_start; i < information_start + 3 && i < end; i++) {
        uint8_t ascii = payload_[i];
        const size_t index = i - information_start;

        if (index == 0) {  /* degrees */
            ascii = static_cast<uint8_t>(ascii - 28);
            ascii = static_cast<uint8_t>(ascii + lng_offset);

            if (ascii >= 180 && ascii <= 189)
                ascii = static_cast<uint8_t>(ascii - 80);
            else if (ascii >= 190 && ascii <= 199)
                ascii = static_cast<uint8_t>(ascii - 190);

            lng += ascii;
        } else if (index == 1) {  /* minutes */
            ascii = static_cast<uint8_t>(ascii - 28);
            if (ascii >= 60) ascii = static_cast<uint8_t>(ascii - 60);
            lng += ascii / 60.0;
        } else {  /* hundredths of a minute */
            ascii = static_cast<uint8_t>(ascii - 28);
            lng += (ascii / 100.0) / 60.0;
        }
    }

    if (is_west) lng = -lng;
    pos.longitude = static_cast<float>(lng);
}

uint8_t AprsPacket::mic_e_lat_digit(uint8_t ascii) {
    if (ascii >= '0' && ascii <= '9') return ascii;
    if (ascii >= 'A' && ascii <= 'J') return static_cast<uint8_t>(ascii - 17);
    if (ascii >= 'P' && ascii <= 'Y') return static_cast<uint8_t>(ascii - 32);
    if (ascii == 'K' || ascii == 'L' || ascii == 'Z') return ' ';
    return '\0';
}

bool AprsPacket::mic_e_lat_is_north(uint8_t ascii) {
    return (ascii >= 'P' && ascii <= 'Z');
}

bool AprsPacket::mic_e_lng_is_west(uint8_t ascii) {
    return (ascii >= 'P' && ascii <= 'Z');
}

uint8_t AprsPacket::mic_e_lng_offset(uint8_t ascii) {
    return (ascii >= 'P' && ascii <= 'Z') ? uint8_t{100} : uint8_t{0};
}

/* ===========================================================================
 * Ax25Decoder — port of APRSRxProcessor::parse_bit / parse_packet
 * ===========================================================================*/

void Ax25Decoder::reset() {
    state_ = State::WaitFlag;
    last_bit_ = 0;
    ones_count_ = 0;
    current_byte_ = 0;
    bit_index_ = 0;
    buffer_size_ = 0;
}

void Ax25Decoder::abort_frame() {
    state_ = State::WaitFlag;
    current_byte_ = 0;
    ones_count_ = 0;
    bit_index_ = 0;
    buffer_size_ = 0;
}

void Ax25Decoder::feed_bit(uint8_t raw_bit) {
    bits_fed_++;

    if (!parse_bit(raw_bit)) return;

    /* A closing flag. Upstream calls parse_packet() here: run the FCS over the
     * whole buffer, and hand the frame up only when the residue matches. */
    if (buffer_size_ < kAprsMinLength) {
        frames_too_short_++;
    } else if (ax25_frame_valid(buffer_, buffer_size_)) {
        frames_valid_++;
        if (on_frame_) on_frame_(buffer_, buffer_size_);
    } else {
        frames_bad_fcs_++;
    }

    /* DEPARTURE: upstream leaves packet_buffer_size alone here, so when two
     * frames share a single flag the second one appends to the first and both
     * are lost. Clearing is what makes back-to-back frames decode. */
    buffer_size_ = 0;
}

void Ax25Decoder::feed_bits(const uint8_t* bits, size_t count) {
    if (bits == nullptr) return;
    for (size_t i = 0; i < count; i++) feed_bit(bits[i]);
}

void Ax25Decoder::feed_bits(const std::vector<uint8_t>& bits) {
    feed_bits(bits.data(), bits.size());
}

bool Ax25Decoder::parse_bit(uint8_t raw_bit) {
    /* NRZI: no transition is a 1, a transition is a 0. */
    const uint8_t decoded_bit = static_cast<uint8_t>((~(raw_bit ^ last_bit_)) & 0x01);
    last_bit_ = static_cast<uint8_t>(raw_bit & 0x01);

    if (decoded_bit & 0x01) {
        if (ones_count_ < 8) ones_count_++;
    } else {
        if (ones_count_ > 6) {
            /* Seven or more ones is an HDLC abort, and nothing else. */
            abort_frame();
            return false;
        } else if (ones_count_ == 6) {
            /* 0111 1110 — a flag. */
            bool done = false;
            if (state_ == State::InFrame) {
                done = true;
            } else {
                buffer_size_ = 0;
            }
            state_ = State::WaitFrame;
            current_byte_ = 0;
            ones_count_ = 0;
            bit_index_ = 0;
            return done;
        } else if (ones_count_ == 5) {
            /* A stuffed zero: drop it, it is not data. */
            ones_count_ = 0;
            return false;
        } else {
            ones_count_ = 0;
        }
    }

    /* Bytes arrive least-significant bit first. */
    current_byte_ = static_cast<uint8_t>(current_byte_ >> 1);
    current_byte_ = static_cast<uint8_t>(current_byte_ | (decoded_bit ? 0x80 : 0x00));
    bit_index_++;

    if (bit_index_ >= 8) {
        bit_index_ = 0;
        if (state_ == State::WaitFrame) state_ = State::InFrame;

        if (state_ == State::InFrame) {
            if (buffer_size_ + 1 >= kAprsMaxLength) {
                abort_frame();
                return false;
            }
            buffer_[buffer_size_++] = current_byte_;
        }
    }

    return false;
}

/* ===========================================================================
 * AprsChannelDecoder
 * ===========================================================================*/

void AprsChannelDecoder::configure(double input_rate_hz, double frequency_offset_hz) {
    configured_ = false;
    if (!(input_rate_hz > 0.0)) return;

    input_rate_hz_ = input_rate_hz;
    offset_hz_ = frequency_offset_hz;

    /* Decimate to as close to the nominal channel rate as an integer ratio
     * allows, then tell the AFSK demodulator the rate it actually got. */
    long dec = std::lround(input_rate_hz / kNominalChannelRate);
    if (dec < 1) dec = 1;
    decimation_ = static_cast<size_t>(dec);
    channel_rate_hz_ = input_rate_hz / static_cast<double>(decimation_);

    /* The channel filter has to stop everything that would alias into the
     * decimated band, so its transition has to close before channel_rate/2. An
     * input already at or below the channel rate leaves no room for the full
     * 11 kHz channel, so the cutoff comes down with it rather than being asked
     * for a passband edge above Nyquist. */
    const double cutoff = std::min(kChannelCutoffHz, channel_rate_hz_ * 0.4);
    const double transition = std::max(1000.0, (channel_rate_hz_ / 2.0 - cutoff) * 0.8);
    channel_filter_.configure(
        dsp::design_lowpass(cutoff, transition, input_rate_hz, 60.0),
        decimation_);

    afsk_.configure(static_cast<float>(channel_rate_hz_), dsp::AfskDemod::Standard::Bell202);

    nco_.set_frequency(-offset_hz_, input_rate_hz_);

    /* Upstream's parse_ax25() pushes an APRSPacketMessage to the application
     * queue; here the framer hands the bytes straight to the packet parser. */
    framer_.set_frame_handler([this](const uint8_t* frame, size_t length) {
        if (!on_packet) return;
        AprsPacket packet;
        packet.set_bytes(frame, length);
        packet.set_valid_checksum(true);
        on_packet(packet);
    });

    configured_ = true;
    reset();
}

void AprsChannelDecoder::set_frequency_offset(double frequency_offset_hz) {
    offset_hz_ = frequency_offset_hz;
    if (configured_) nco_.set_frequency(-offset_hz_, input_rate_hz_);
}

void AprsChannelDecoder::reset() {
    nco_.reset();
    nco_.set_frequency(-offset_hz_, input_rate_hz_);
    channel_filter_.reset();
    afsk_.reset();
    framer_.reset();
    mixed_.clear();
    channel_.clear();
    bits_.clear();
}

void AprsChannelDecoder::process(const dsp::cfloat* in, size_t count) {
    if (!configured_ || in == nullptr || count == 0) return;

    mixed_.resize(count);
    nco_.mix(in, mixed_.data(), count);

    channel_.clear();
    channel_filter_.process(mixed_.data(), count, channel_);
    if (channel_.empty()) return;

    bits_.clear();
    afsk_.process(channel_.data(), channel_.size(), bits_);
    framer_.feed_bits(bits_);
}

/* ===========================================================================
 * UI
 * ===========================================================================*/

const AprsRegion kAprsRegions[] = {
    {"MAN", 0},           /* keep whatever the operator dialled in */
    {"NA ", 144390000},   /* North America — upstream's default */
    {"NZ ", 144575000},
    {"JAP", 144640000},
    {"PHI", 144740000},
    {"EUR", 144800000},
    {"THA", 144900000},
    {"AUS", 145175000},
    {"BR ", 145570000},
    {"ISS", 145825000},
};

static_assert(sizeof(kAprsRegions) / sizeof(kAprsRegions[0]) == kAprsRegionCount,
              "kAprsRegionCount must match kAprsRegions");

/* --- AprsStreamView ------------------------------------------------------- */

AprsStreamView::AprsStreamView(ui::Rect parent_rect)
    : ui::View{parent_rect} {
    /* TabView's contract: a tab's content view arrives hidden. */
    hidden(true);

    add_children({&options_region_,
                  &field_frequency_,
                  &field_gain_,
                  &text_gain_label_,
                  &text_stats_,
                  &labels_note_,
                  &console_});

    ui::OptionsField::options_t regions;
    for (size_t i = 0; i < kAprsRegionCount; i++) {
        regions.push_back({kAprsRegions[i].name, static_cast<int32_t>(i)});
    }
    options_region_.set_options(std::move(regions));

    auto& ctx = globals();

    if (auto* r = ctx.radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    field_frequency_.set_step_index(2);  /* 100 Hz, as upstream's set_step(100) */
    field_frequency_.set_value(kAprsRegions[region_index_].frequency, false);

    field_frequency_.on_change = [this](uint64_t hz) {
        /* Hand-tuning means the region no longer describes the frequency. */
        if (!suppress_region_) {
            suppress_region_ = true;
            options_region_.set_selected_index(0, false);
            region_index_ = 0;
            suppress_region_ = false;
        }
        if (on_frequency_changed) on_frequency_changed(hz);
    };

    options_region_.on_change = [this](size_t index, int32_t) { apply_region(index); };
    options_region_.set_selected_index(region_index_, false);

    if (auto* rx = ctx.receiver) {
        field_gain_.set_value(static_cast<int32_t>(rx->gain()), false);
        field_gain_.on_change = [rx](int32_t db) { rx->set_gain(db); };
    }

    console_.enable_scrolling(true);
}

void AprsStreamView::focus() {
    field_frequency_.focus();
}

void AprsStreamView::apply_region(size_t index) {
    if (index >= kAprsRegionCount) return;
    region_index_ = index;
    if (index == 0) return;  /* MAN: leave the frequency alone */

    suppress_region_ = true;
    field_frequency_.set_value(kAprsRegions[index].frequency, false);
    suppress_region_ = false;

    if (on_frequency_changed) on_frequency_changed(kAprsRegions[index].frequency);
}

void AprsStreamView::set_frequency(uint64_t hz) {
    suppress_region_ = true;
    field_frequency_.set_value(hz, false);
    suppress_region_ = false;
}

void AprsStreamView::set_region(size_t index) {
    if (index >= kAprsRegionCount) return;
    options_region_.set_selected_index(index, true);
}

void AprsStreamView::on_packet(const AprsPacket& packet) {
    /* Upstream cycles the console colour per packet through blue/green/cyan/red
     * so consecutive lines are separable at a glance. */
    std::string line = "\x1B";
    line += static_cast<char>((console_color_++ & 3) + 9);
    line += packet.get_stream_text();
    console_.writeln(line);
}

void AprsStreamView::set_stats(size_t valid, size_t bad_fcs, size_t bits) {
    text_stats_.set("Pkt " + to_string_dec_uint(valid) +
                    " FCS-" + to_string_dec_uint(bad_fcs) +
                    " bits " + to_string_dec_uint(bits));
}

/* --- AprsDetailsView ------------------------------------------------------ */

AprsDetailsView::AprsDetailsView() {
    hidden(true);

    add_children({&text_source_,
                  &text_position_,
                  &console_,
                  &button_close_,
                  &button_map_});

    button_close_.on_select = [this](ui::Button&) {
        console_.clear(true);
        if (on_close) on_close();
    };

    button_map_.on_select = [this](ui::Button&) {
        auto* nav = globals().nav;
        if (nav == nullptr) return;

        auto map = std::make_unique<ui::GeoMapView>(
            entry_.source_formatted,
            0,
            ui::GeoPos::alt_unit::FEET,
            ui::GeoPos::spd_unit::HIDDEN,
            entry_.pos.latitude,
            entry_.pos.longitude,
            ui::invalid_angle,
            [this]() {
                /* GeoMapView calls this from its destructor, so the pointer is
                 * cleared exactly when it stops being valid. */
                geomap_view_ = nullptr;
                hidden(false);
                update();
            });

        geomap_view_ = map.get();
        nav->push(std::move(map));
        hidden(true);
    };
}

void AprsDetailsView::focus() {
    button_close_.focus();
}

void AprsDetailsView::set_entry(const AprsRecentEntry& entry) {
    entry_ = entry;
}

void AprsDetailsView::update() {
    if (!hidden()) {
        text_source_.set(entry_.source_formatted);

        if (entry_.has_position) {
            text_position_.set(to_string_decimal(entry_.pos.latitude, 4) + " " +
                               to_string_decimal(entry_.pos.longitude, 4));
        } else {
            text_position_.set("no position");
        }

        console_.clear(true);
        console_.write(entry_.info_string);

        button_map_.hidden(!entry_.has_position);
    }

    if (geomap_view_ != nullptr && entry_.has_position) {
        geomap_view_->update_position(entry_.pos.latitude, entry_.pos.longitude,
                                      ui::invalid_angle, 0, 0);
    }
}

/* --- AprsTableView -------------------------------------------------------- */

AprsTableView::AprsTableView(ui::Rect parent_rect)
    : ui::View{parent_rect} {
    hidden(true);

    add_children({&entries_view_, &details_view_});

    entries_view_.set_parent_rect({0, 0, parent_rect.width(), parent_rect.height()});
    details_view_.set_parent_rect({0, 0, parent_rect.width(), parent_rect.height()});
    details_view_.hidden(true);

    entries_view_.on_select = [this](const AprsRecentEntry& entry) { show_detail(entry); };
    details_view_.on_close = [this]() { show_list(); };

    /* Upstream provides this as an explicit RecentEntriesTable::draw template
     * specialization; the host's std::function hook is the same rendering. */
    entries_view_.table().on_draw = [](const AprsRecentEntry& entry,
                                       const ui::Rect& target_rect,
                                       ui::Painter& painter,
                                       const ui::Style& style,
                                       ui::RecentEntriesColumns& columns) {
        std::string line = entry.source_formatted;
        line.resize(columns.at(0).second, ' ');
        line += "        ";
        line += (entry.hits <= 999) ? to_string_dec_uint(entry.hits, 4) : std::string{"999+"};
        line += " ";
        line += entry.time_string;

        painter.draw_string(target_rect.location(), style, line);

        /* Upstream draws bitmap_target here; the host bitmap set has no such
         * icon, so the position flag is a character in the Loc column. */
        if (entry.has_position) {
            const ui::Style marker{style.font, style.background, ui::Color::green()};
            painter.draw_string(target_rect.location() + ui::Point{12 * 8, 0}, marker, "*");
        }
    };
}

void AprsTableView::show_list() {
    details_view_.hidden(true);
    entries_view_.hidden(false);
    focus();
}

void AprsTableView::show_detail(const AprsRecentEntry& entry) {
    entries_view_.hidden(true);
    details_view_.hidden(false);
    details_view_.set_entry(entry);
    details_view_.update();
    details_view_.focus();
}

void AprsTableView::on_show() {
    ui::View::on_show();
    show_list();
}

void AprsTableView::on_hide() {
    details_view_.hidden(true);
    ui::View::on_hide();
}

void AprsTableView::focus() {
    entries_view_.focus();
}

void AprsTableView::on_packet(const AprsPacket& packet, const std::string& timestamp) {
    auto& entry = ui::on_packet(recent_, packet.get_source());

    entry.inc_hit();
    entry.source_formatted = packet.get_source_formatted();
    entry.time_string = timestamp;
    entry.info_string = packet.get_stream_text();

    /* A station that has reported a position once keeps it: a following status
     * or telemetry packet carries none, and blanking it would drop the marker. */
    if (packet.has_position()) {
        entry.has_position = true;
        entry.pos = packet.get_position();
    }

    if (!details_view_.hidden() && details_view_.entry().key() == entry.key()) {
        details_view_.set_entry(entry);
        details_view_.update();
    }

    entries_view_.table().set_dirty();
}

/* --- AprsRxView ----------------------------------------------------------- */

namespace {

/* The most the wideband snapshot tap will ever hand back in one go. */
constexpr size_t kTapSamples = 4096;

/* A narrow capture makes the snapshot tap cover as much wall-clock time per UI
 * frame as it can, and keeps the channel filter cheap. */
constexpr double kAprsSamplingRate = 500'000.0;

}  // namespace

AprsRxView::AprsRxView()
    : receiver_{*globals().receiver} {
    add_children({&tab_view_, &view_stream_, &view_table_});

    view_stream_.on_frequency_changed = [this](uint64_t hz) { retune(hz); };

    decoder_.on_packet = [this](const AprsPacket& packet) { handle_packet(packet); };
}

AprsRxView::~AprsRxView() {
    /* The receive chain is left running, as the other host RX apps do. */
}

void AprsRxView::focus() {
    tab_view_.focus();
}

void AprsRxView::on_show() {
    ui::View::on_show();

    receiver_.set_mode(radio::ReceiverModel::Mode::NarrowbandFMAudio);
  /* Data decoder: no speaker monitor. It reads its own tap; the
     * demodulated audio would be modem tones nobody needs. */
    receiver_.set_audio_monitor(false);
    receiver_.set_nfm_configuration(radio::ReceiverModel::NfmConfig::Medium11k);
    if (receiver_.sampling_rate() != kAprsSamplingRate)
        receiver_.set_sampling_rate(kAprsSamplingRate);

    retune(view_stream_.frequency());

    if (!receiver_.running()) receiver_.start();
}

void AprsRxView::retune(uint64_t hz) {
    if (hz == 0) return;
    receiver_.set_target_frequency(hz);
    update_offset();
}

void AprsRxView::update_offset() {
    auto* r = globals().radio;
    if (r == nullptr) return;

    const double lo = r->rx_frequency();
    if (!(lo > 0.0)) return;

    /* The snapshot tap sits before the ReceiverModel's own NCO, so the wanted
     * signal is still at (target - LO) in these samples. */
    const double offset = static_cast<double>(receiver_.target_frequency()) - lo;
    if (std::fabs(offset - decoder_.frequency_offset()) > 1.0)
        decoder_.set_frequency_offset(offset);
}

void AprsRxView::handle_packet(const AprsPacket& packet) {
    view_stream_.on_packet(packet);

    /* "YYYY-MM-DD HH:MM:SS" -> "HH:MM:SS", the field upstream shows. */
    const std::string now = to_string_datetime_now();
    const std::string hms = (now.size() >= 19) ? now.substr(11, 8) : now;
    view_table_.on_packet(packet, hms);
}

void AprsRxView::on_frame_sync() {
    ui::View::on_frame_sync();

    const double rate = receiver_.sampling_rate();
    if (rate > 0.0 && rate != configured_rate_) {
        configured_rate_ = rate;
        decoder_.configure(rate, decoder_.frequency_offset());
    }

    if (!decoder_.configured()) return;

    update_offset();

    if (receiver_.take_spectrum_samples(samples_, kTapSamples) && !samples_.empty())
        decoder_.process(samples_.data(), samples_.size());

    frame_counter_++;
    if ((frame_counter_ % 10) == 0) {
        view_stream_.set_stats(decoder_.framer().frames_valid(),
                               decoder_.framer().frames_bad_fcs(),
                               decoder_.framer().bits_fed());
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_aprs_rx{{"aprsrx", "APRS RX", app::Category::Receive,
                                  ui::Color::green(), &ui::bitmap_icon_aprs,
                                  [] { return std::make_unique<app::AprsRxView>(); }}};
}  // namespace
