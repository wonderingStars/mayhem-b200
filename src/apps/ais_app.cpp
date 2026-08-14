/*
 * mayhem-b200 — AIS (Automatic Identification System) receiver.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ais_app.hpp"

#include "../core/file_path.hpp"
#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "theme.hpp"
#include "ui_navigation.hpp"
#include "ui_painter.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <ctime>

namespace ais {

/* --- FCS ------------------------------------------------------------------ */

uint16_t fcs(const uint8_t* data, const size_t length) {
    /* CRC-16/X-25: poly 0x1021, init 0xFFFF, reflected in and out, xorout
     * 0xFFFF. Identical to the FCS-16 of RFC 1662, which is where the AIS/HDLC
     * framing gets it from. */
    dsp::Crc<16, true, true> crc{0x1021, 0xFFFF, 0xFFFF};
    if (data != nullptr) crc.process_bytes(data, length);
    return static_cast<uint16_t>(crc.checksum());
}

uint16_t fcs(const std::vector<uint8_t>& data) {
    return fcs(data.data(), data.size());
}

/* --- NRZI ----------------------------------------------------------------- */

std::vector<uint8_t> nrzi_decode(const std::vector<uint8_t>& symbols) {
    std::vector<uint8_t> out;
    out.reserve(symbols.size());
    dsp::NrziDecoder decoder{};
    for (const uint8_t symbol : symbols)
        out.push_back(static_cast<uint8_t>(decoder(static_cast<uint_fast8_t>(symbol & 1))));
    return out;
}

std::vector<uint8_t> nrzi_encode(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> out;
    out.reserve(bits.size());
    dsp::NrziEncoder encoder{};
    for (const uint8_t bit : bits)
        out.push_back(static_cast<uint8_t>(encoder(static_cast<uint_fast8_t>(bit & 1))));
    return out;
}

/* --- Bit stuffing --------------------------------------------------------- */

std::vector<uint8_t> hdlc_stuff(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> out;
    out.reserve(bits.size() + bits.size() / 5 + 1);

    size_t ones = 0;
    for (const uint8_t raw : bits) {
        const uint8_t bit = raw ? uint8_t{1} : uint8_t{0};
        out.push_back(bit);
        if (bit) {
            if (++ones == 5) {
                out.push_back(0);
                ones = 0;
            }
        } else {
            ones = 0;
        }
    }
    return out;
}

std::vector<uint8_t> hdlc_destuff(const std::vector<uint8_t>& bits) {
    /* Exactly what PacketBuilder's unstuff matcher does: drop the current bit
     * when the last six (this one included) are 111110. */
    std::vector<uint8_t> out;
    out.reserve(bits.size());

    size_t ones = 0;
    for (const uint8_t raw : bits) {
        const uint8_t bit = raw ? uint8_t{1} : uint8_t{0};
        if ((ones >= 5) && (bit == 0)) {
            ones = 0;
            continue;
        }
        out.push_back(bit);
        ones = bit ? (ones + 1) : 0;
    }
    return out;
}

/* --- Message length table (common/ais_packet.cpp) ------------------------- */

namespace {

struct PacketLengthRange {
    uint16_t min_bits;
    uint16_t max_bits;

    constexpr bool contains(const size_t bit_count) const {
        return !is_above(bit_count) && !is_below(bit_count);
    }
    constexpr bool is_above(const size_t bit_count) const { return min_bits > bit_count; }
    constexpr bool is_below(const size_t bit_count) const { return max_bits < bit_count; }
};

/* Upstream declares 64 entries and initialises 32; ids 32..63 are therefore
 * {0, 0} and never validate. Kept identical. */
constexpr std::array<PacketLengthRange, 64> packet_length_range{{
    {0, 0},      /*  0 */
    {168, 168},  /*  1 position report, scheduled */
    {168, 168},  /*  2 position report, assigned */
    {168, 168},  /*  3 position report, interrogated */
    {168, 168},  /*  4 base station report */
    {424, 424},  /*  5 static and voyage related data */
    {0, 0},      /*  6 */
    {0, 0},      /*  7 */
    {0, 1008},   /*  8 binary broadcast */
    {0, 0},      /*  9 */
    {0, 0},      /* 10 */
    {0, 0},      /* 11 */
    {0, 0},      /* 12 */
    {0, 0},      /* 13 */
    {0, 0},      /* 14 */
    {0, 0},      /* 15 */
    {0, 0},      /* 16 */
    {0, 0},      /* 17 */
    {168, 168},  /* 18 class B position report */
    {0, 0},      /* 19 */
    {72, 160},   /* 20 data link management */
    {272, 360},  /* 21 aid to navigation report */
    {168, 168},  /* 22 channel management */
    {160, 160},  /* 23 group assignment */
    {160, 168},  /* 24 class B static data report */
    {0, 168},    /* 25 single slot binary message */
    {0, 0},      /* 26 */
    {0, 0},      /* 27 */
    {0, 0},      /* 28 */
    {0, 0},      /* 29 */
    {0, 0},      /* 30 */
    {0, 0},      /* 31 */
}};

}  // namespace

bool length_valid_for(const uint32_t message_id, const size_t data_bits) {
    if (message_id >= packet_length_range.size()) return false;
    return packet_length_range[message_id].contains(data_bits);
}

/* --- Packet --------------------------------------------------------------- */

Packet Packet::from_bits(const std::vector<uint8_t>& bits) {
    dsp::Packet p{};
    for (const uint8_t bit : bits) p.add(bit != 0);
    p.set_timestamp(std::chrono::system_clock::now());
    return Packet{p};
}

size_t Packet::data_and_fcs_length() const {
    /* The closing flag is eight bits, but the unstuffer swallows one of them
     * (its last six bits are 111110), so seven of them landed in the packet. */
    const size_t len = length();
    return (len >= 7) ? (len - 7) : 0;
}

size_t Packet::data_length() const {
    const size_t dfl = data_and_fcs_length();
    return (dfl >= fcs_length) ? (dfl - fcs_length) : 0;
}

bool Packet::length_valid() const {
    const size_t dfl = data_and_fcs_length();

    /* Host guard: upstream computes this on a size_t and underflows on a runt
     * frame, then indexes the length table with whatever the empty packet's
     * first six bits read as. A frame needs at least one payload octet. */
    if (dfl < (fcs_length + 8)) return false;
    if ((dfl & 7) != 0) return false;

    return length_valid_for(message_id(), data_length());
}

bool Packet::crc_ok() const {
    const size_t n = data_length();
    if (n == 0 || (n & 7) != 0) return false;

    /* Upstream's spelling: feed the transmission-order octets (which are the
     * payload octets bit-reversed) into a non-reflected CRC-16, then compare
     * against the sixteen FCS bits read in transmission order. Algebraically
     * identical to ais::fcs() over the payload octets; tests assert that. */
    dsp::Crc<16> ais_fcs{0x1021, 0xFFFF, 0xFFFF};
    for (size_t i = 0; i < n; i += 8)
        ais_fcs.process_byte(static_cast<uint8_t>(read_stream(i, 8)));

    return ais_fcs.checksum() == read_stream(n, fcs_length);
}

uint32_t Packet::read(const size_t start_bit, const size_t length_bits) const {
    return packet_.read_byte_reversed(start_bit, length_bits);
}

uint32_t Packet::read_stream(const size_t start_bit, const size_t length_bits) const {
    return packet_.read(start_bit, length_bits);
}

std::string Packet::text(const size_t start_bit, const size_t character_count) const {
    std::string result;
    result.reserve(character_count);

    constexpr size_t character_length = 6;
    const size_t end_bit = start_bit + character_count * character_length;
    for (size_t i = start_bit; i < end_bit; i += character_length)
        result += six_bit_to_ascii(static_cast<uint8_t>(read(i, character_length)));

    return result;
}

DateTime Packet::datetime(const size_t start_bit) const {
    return {
        static_cast<uint16_t>(read(start_bit + 0, 14)),
        static_cast<uint8_t>(read(start_bit + 14, 4)),
        static_cast<uint8_t>(read(start_bit + 18, 5)),
        static_cast<uint8_t>(read(start_bit + 23, 5)),
        static_cast<uint8_t>(read(start_bit + 28, 6)),
        static_cast<uint8_t>(read(start_bit + 34, 6)),
    };
}

Latitude Packet::latitude(const size_t start_bit) const {
    return Latitude{static_cast<int32_t>(read(start_bit, 27))};
}

Longitude Packet::longitude(const size_t start_bit) const {
    return Longitude{static_cast<int32_t>(read(start_bit, 28))};
}

std::string Packet::received_at() const {
    const std::time_t t = std::chrono::system_clock::to_time_t(packet_.timestamp());
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm) == 0) return "";
    return std::string{buf};
}

std::string Packet::to_hex_nibbles() const {
    std::string entry;
    entry.reserve((length() + 3) / 4);

    for (size_t i = 0; i < length(); i += 4) {
        const uint32_t nibble = read(i, 4);
        entry += (nibble >= 10) ? static_cast<char>('W' + nibble)
                                : static_cast<char>('0' + nibble);
    }
    return entry;
}

/* --- Presentation --------------------------------------------------------- */

namespace format {

std::string latlon_abs_normalized(const int32_t normalized, const char suffixes[2]) {
    const char suffix = suffixes[(normalized < 0) ? 0 : 1];
    const uint32_t normalized_abs = static_cast<uint32_t>(std::abs(normalized));
    /* 1/10000 minute -> micro-degrees: v / 600000 degrees = v * 5 / 3 udeg. */
    const uint32_t t = (normalized_abs * 5) / 3;
    const uint32_t degrees = t / (100 * 10000);
    const uint32_t fraction = t % (100 * 10000);
    return to_string_dec_uint(degrees) + "." + to_string_dec_uint(fraction, 6, '0') + suffix;
}

std::string latlon(const Latitude& latitude, const Longitude& longitude) {
    if (latitude.is_valid() && longitude.is_valid()) {
        return latlon_abs_normalized(latitude.normalized(), "SN") + " " +
               latlon_abs_normalized(longitude.normalized(), "WE");
    } else if (latitude.is_not_available() && longitude.is_not_available()) {
        return "not available";
    } else {
        return "invalid";
    }
}

float latlon_float(const int32_t normalized) {
    return ((static_cast<float>(normalized) * 5) / 3) / (100 * 10000);
}

std::string mmsi(const MMSI& mmsi_value) {
    /* Always nine characters, zero padded. */
    return to_string_dec_uint(mmsi_value, 9, '0');
}

std::string text(const std::string& value) {
    const size_t end = value.find_last_not_of("@");
    return (end == std::string::npos) ? "" : value.substr(0, end + 1);
}

std::string navigational_status(const unsigned int value) {
    switch (value) {
        case 0:
            return "under way w/engine";
        case 1:
            return "at anchor";
        case 2:
            return "not under command";
        case 3:
            return "restricted maneuv";
        case 4:
            return "constrained draught";
        case 5:
            return "moored";
        case 6:
            return "aground";
        case 7:
            return "fishing";
        case 8:
            return "sailing";
        case 9:
        case 10:
        case 13:
            return "reserved";
        case 11:
            return "towing astern";
        case 12:
            return "towing ahead/along";
        case 14:
            return "SART/MOB/EPIRB";
        case 15:
            return "undefined";
        default:
            return "unknown";
    }
}

std::string rate_of_turn(const RateOfTurn value) {
    switch (value) {
        case -128:
            return "not available";
        case -127:
            return "left >5 deg/30sec";
        case 0:
            return "0 deg/min";
        case 127:
            return "right >5 deg/30sec";
        default: {
            std::string result = (value < 0) ? "left " : "right ";
            const float value_deg_sqrt = static_cast<float>(value) / 4.733f;
            const int32_t value_deg = static_cast<int32_t>(value_deg_sqrt * value_deg_sqrt);
            result += to_string_dec_uint(static_cast<uint64_t>(value_deg));
            result += " deg/min";
            return result;
        }
    }
}

std::string speed_over_ground(const SpeedOverGround value) {
    if (value == 1023) {
        return "not available";
    } else if (value == 1022) {
        return ">= 102.2 knots";
    } else {
        return to_string_dec_uint(value / 10u) + "." + to_string_dec_uint(value % 10u, 1) +
               " knots";
    }
}

std::string course_over_ground(const CourseOverGround value) {
    if (value > 3600) {
        return "invalid";
    } else if (value == 3600) {
        return "not available";
    } else {
        return to_string_dec_uint(value / 10u) + "." + to_string_dec_uint(value % 10u, 1) + " deg";
    }
}

std::string true_heading(const TrueHeading value) {
    if (value == 511) {
        return "not available";
    } else if (value > 359) {
        return "invalid";
    } else {
        return to_string_dec_uint(value) + " deg";
    }
}

}  // namespace format

/* --- Frame construction --------------------------------------------------- */

namespace {

void append_lsb_first_byte(std::vector<uint8_t>& bits, const uint8_t byte) {
    for (size_t k = 0; k < 8; k++) bits.push_back(static_cast<uint8_t>((byte >> k) & 1));
}

/* Payload octets and FCS, all LSB-first, as they go on the wire. */
std::vector<uint8_t> payload_and_fcs_bits(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> bits;
    bits.reserve(payload.size() * 8 + 16);

    for (const uint8_t byte : payload) append_lsb_first_byte(bits, byte);

    const uint16_t f = fcs(payload);
    for (size_t k = 0; k < 16; k++) bits.push_back(static_cast<uint8_t>((f >> k) & 1));

    return bits;
}

/* 01111110, oldest bit first — the order the packet builder's bit history
 * sees, not a byte to be reversed. */
constexpr std::array<uint8_t, 8> hdlc_flag{{0, 1, 1, 1, 1, 1, 1, 0}};

}  // namespace

std::vector<uint8_t> build_packet_bits(const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> bits = payload_and_fcs_bits(payload);
    /* Seven of the closing flag's eight bits; the eighth is eaten by the
     * unstuffer. */
    for (size_t i = 0; i < 7; i++) bits.push_back(hdlc_flag[i]);
    return bits;
}

std::vector<uint8_t> build_air_bits(const std::vector<uint8_t>& payload,
                                    const size_t training_bits) {
    std::vector<uint8_t> bits;

    /* Training: alternating, ending on a 1 so the eight bits before the start
     * flag read 01010101 and the preamble matcher's sixteen-bit constant
     * 0101010101111110 lines up. Defined before NRZI encoding, exactly as
     * upstream's matcher expects to see it after NRZI decoding. */
    for (size_t i = 0; i < training_bits; i++) bits.push_back(static_cast<uint8_t>(i & 1));

    bits.insert(bits.end(), hdlc_flag.begin(), hdlc_flag.end());

    const std::vector<uint8_t> stuffed = hdlc_stuff(payload_and_fcs_bits(payload));
    bits.insert(bits.end(), stuffed.begin(), stuffed.end());

    bits.insert(bits.end(), hdlc_flag.begin(), hdlc_flag.end());

    return nrzi_encode(bits);
}

/* --- Decoder -------------------------------------------------------------- */

Decoder::Decoder() {
    builder_.configure(dsp::BitPattern{0x557E, 16, 1}, /* 0101010101111110 */
                       dsp::BitPattern{0x3E, 6});      /* 111110           */
    builder_.set_end_matcher(dsp::BitPattern{0x7E, 8}); /* 01111110         */
    builder_.set_payload_handler([this](const dsp::Packet& p) { this->on_frame(p); });

    clock_recovery_.set_symbol_handler([this](const float symbol) {
        this->consume_symbol(symbol);
    });

    configure(38400.0f);
}

void Decoder::configure(const float channel_rate_hz) {
    if (channel_rate_hz <= 0.0f) return;
    channel_rate_ = channel_rate_hz;

    /* One symbol of rectangular window, as upstream's four taps are at
     * 38.4 kHz, and decimate to two samples per symbol for Gardner. */
    size_t taps_count =
        static_cast<size_t>(std::lround(static_cast<double>(channel_rate_hz / symbol_rate)));
    if (taps_count < 2) taps_count = 2;

    size_t decimation = static_cast<size_t>(
        std::lround(static_cast<double>(channel_rate_hz / (symbol_rate * 2.0f))));
    if (decimation < 1) decimation = 1;

    const auto taps = dsp::design_matched_filter_taps(
        static_cast<double>(deviation_hz), static_cast<double>(channel_rate_hz), taps_count);
    mf_.configure(taps.data(), taps.size(), decimation);

    const float mf_rate = channel_rate_hz / static_cast<float>(decimation);
    /* Upstream's bang-bang loop filter and its weight. AIS bounds any run of
     * identical symbols at six (bit stuffing), so the run-length problem
     * DeadbandErrorFilter exists to solve does not arise here. */
    clock_recovery_.configure(mf_rate, symbol_rate, dsp::FixedErrorFilter{0.0555f});

    reset();
}

void Decoder::reset() {
    mf_.reset();
    clock_recovery_.reset();
    nrzi_.reset();
    builder_.reset();
}

void Decoder::feed(const dsp::cfloat* in, const size_t count) {
    if (in == nullptr) return;
    for (size_t i = 0; i < count; i++) {
        if (mf_.execute_once(in[i])) clock_recovery_(mf_.get_output());
    }
}

void Decoder::consume_symbol(const float raw_symbol) {
    const uint_fast8_t sliced_symbol = (raw_symbol >= 0.0f) ? 1u : 0u;
    const auto decoded_symbol = nrzi_(sliced_symbol);
    builder_.execute(decoded_symbol);
}

void Decoder::on_frame(const dsp::Packet& packet) {
    frames_seen_++;

    const Packet p{packet};
    if (!p.length_valid()) {
        length_failures_++;
        return;
    }
    if (!p.crc_ok()) {
        crc_failures_++;
        return;
    }

    packets_valid_++;
    if (packet_handler_) packet_handler_(p);
}

}  // namespace ais

namespace app {

/* --- Recent entry --------------------------------------------------------- */

void AISRecentEntry::update(const ais::Packet& packet) {
    received_count++;

    switch (packet.message_id()) {
        case 1:
        case 2:
        case 3:
            navigational_status = static_cast<int8_t>(packet.read(38, 4));
            last_position.rate_of_turn = static_cast<ais::RateOfTurn>(packet.read(42, 8));
            last_position.speed_over_ground =
                static_cast<ais::SpeedOverGround>(packet.read(50, 10));
            last_position.timestamp = packet.received_at();
            last_position.latitude = packet.latitude(89);
            last_position.longitude = packet.longitude(61);
            last_position.course_over_ground =
                static_cast<ais::CourseOverGround>(packet.read(116, 12));
            last_position.true_heading = static_cast<ais::TrueHeading>(packet.read(128, 9));
            break;

        case 4:
            last_position.timestamp = packet.received_at();
            last_position.latitude = packet.latitude(107);
            last_position.longitude = packet.longitude(79);
            break;

        case 5:
            call_sign = trim(packet.text(70, 7));
            name = trim(packet.text(112, 20));
            destination = trim(packet.text(302, 20));
            break;

        case 18:
            last_position.timestamp = packet.received_at();
            last_position.speed_over_ground =
                static_cast<ais::SpeedOverGround>(packet.read(46, 10));
            last_position.latitude = packet.latitude(85);
            last_position.longitude = packet.longitude(57);
            last_position.course_over_ground =
                static_cast<ais::CourseOverGround>(packet.read(112, 12));
            last_position.true_heading = static_cast<ais::TrueHeading>(packet.read(124, 9));
            break;

        case 21:
            name = trim(packet.text(43, 20));
            last_position.timestamp = packet.received_at();
            last_position.latitude = packet.latitude(192);
            last_position.longitude = packet.longitude(164);
            break;

        case 24:
            switch (packet.read(38, 2)) {
                case 0:
                    name = trim(packet.text(40, 20));
                    break;
                case 1:
                    call_sign = trim(packet.text(90, 7));
                    break;
                default:
                    break;
            }
            break;

        default:
            break;
    }
}

/* --- Logger --------------------------------------------------------------- */

bool AISLogger::open() {
    if (file_.is_open()) return true;

    const std::string dir = core::data_directory() + "/LOGS";
    if (!core::ensure_directory(dir)) {
        error_ = "cannot create " + dir;
        return false;
    }

    path_ = dir + "/AIS.TXT";
    file_.open(path_, std::ios::app);
    if (!file_.is_open()) {
        error_ = "cannot open " + path_;
        return false;
    }

    error_.clear();
    return true;
}

void AISLogger::close() {
    if (file_.is_open()) file_.close();
}

void AISLogger::on_packet(const ais::Packet& packet) {
    if (!file_.is_open()) return;
    file_ << packet.received_at() << ' ' << packet.to_hex_nibbles() << '\n';
    file_.flush();
}

/* --- Detail page ---------------------------------------------------------- */

AISRecentEntryDetailView::AISRecentEntryDetailView() {
    add_children({&button_see_map_, &button_done_});

    button_done_.on_select = [this](ui::Button&) {
        if (on_close) on_close();
    };

    button_see_map_.on_select = [this](ui::Button&) {
        auto* nav = globals().nav;
        if (nav == nullptr) return;

        auto link = map_link_;
        auto view = std::make_unique<ui::GeoMapView>(
            ais::format::text(entry_.name),
            0,
            ui::GeoPos::alt_unit::METERS,
            ui::GeoPos::spd_unit::KNOTS,
            ais::format::latlon_float(entry_.last_position.latitude.normalized()),
            ais::format::latlon_float(entry_.last_position.longitude.normalized()),
            entry_.last_position.true_heading,
            [link]() { link->view = nullptr; });

        map_link_->view = view.get();
        nav->push(std::move(view));
    };
}

void AISRecentEntryDetailView::update_position() {
    if (map_link_->view == nullptr) return;

    const auto& pos = entry_.last_position;
    const int32_t speed =
        (pos.speed_over_ground > 1022) ? 0 : static_cast<int32_t>(pos.speed_over_ground / 10);

    map_link_->view->update_position(ais::format::latlon_float(pos.latitude.normalized()),
                                     ais::format::latlon_float(pos.longitude.normalized()),
                                     pos.true_heading, 0, speed);
}

bool AISRecentEntryDetailView::add_map_marker(const AISRecentEntry& entry) {
    if (map_link_->view == nullptr) return false;

    ui::GeoMarker marker{};
    marker.lat = ais::format::latlon_float(entry.last_position.latitude.normalized());
    marker.lon = ais::format::latlon_float(entry.last_position.longitude.normalized());
    marker.angle = entry.last_position.true_heading;
    marker.tag = entry.call_sign.empty() ? to_string_dec_uint(entry.mmsi) : entry.call_sign;

    return map_link_->view->store_marker(marker) == ui::MARKER_STORED;
}

void AISRecentEntryDetailView::update_map_markers(const AISRecentEntries& entries) {
    if (map_link_->view == nullptr) return;

    map_link_->view->clear_markers();
    for (const auto& entry : entries) {
        if (entry.last_position.latitude.is_valid() && entry.last_position.longitude.is_valid())
            add_map_marker(entry);
    }
    update_position();
}

void AISRecentEntryDetailView::focus() {
    button_done_.focus();
}

ui::Rect AISRecentEntryDetailView::draw_field(ui::Painter& painter,
                                              const ui::Rect& draw_rect,
                                              const ui::Style& style,
                                              const std::string& label,
                                              const std::string& value) {
    constexpr int label_length_max = 4;

    painter.draw_string(ui::Point{draw_rect.left(), draw_rect.top()}, style, label);
    painter.draw_string(
        ui::Point{draw_rect.left() + (label_length_max + 1) * 8, draw_rect.top()}, style, value);

    return {draw_rect.left(), draw_rect.top() + draw_rect.height(), draw_rect.width(),
            draw_rect.height()};
}

void AISRecentEntryDetailView::paint(ui::Painter& painter) {
    ui::View::paint(painter);

    const ui::Style s = style();
    const auto rect = screen_rect();

    auto field_rect = ui::Rect{rect.left(), rect.top(), rect.width(), 16};

    const auto& pos = entry_.last_position;

    field_rect = draw_field(painter, field_rect, s, "MMSI", ais::format::mmsi(entry_.mmsi));
    field_rect = draw_field(painter, field_rect, s, "Name", ais::format::text(entry_.name));
    field_rect = draw_field(painter, field_rect, s, "Call", ais::format::text(entry_.call_sign));
    field_rect = draw_field(painter, field_rect, s, "Dest", ais::format::text(entry_.destination));
    field_rect = draw_field(painter, field_rect, s, "Last", pos.timestamp);
    field_rect = draw_field(painter, field_rect, s, "Pos ",
                            ais::format::latlon(pos.latitude, pos.longitude));
    field_rect = draw_field(
        painter, field_rect, s, "Stat",
        (entry_.navigational_status < 0)
            ? std::string{"unknown"}
            : ais::format::navigational_status(
                  static_cast<unsigned int>(entry_.navigational_status)));
    field_rect = draw_field(painter, field_rect, s, "RoT ",
                            ais::format::rate_of_turn(pos.rate_of_turn));
    field_rect = draw_field(painter, field_rect, s, "SoG ",
                            ais::format::speed_over_ground(pos.speed_over_ground));
    field_rect = draw_field(painter, field_rect, s, "CoG ",
                            ais::format::course_over_ground(pos.course_over_ground));
    field_rect = draw_field(painter, field_rect, s, "Head",
                            ais::format::true_heading(pos.true_heading));
    field_rect = draw_field(painter, field_rect, s, "Rx #",
                            to_string_dec_uint(entry_.received_count));
    (void)field_rect;
}

void AISRecentEntryDetailView::set_entry(const AISRecentEntry& new_entry) {
    entry_ = new_entry;
    set_dirty();
}

/* --- App ------------------------------------------------------------------ */

AISAppView::AISAppView() {
    add_children({&labels_, &options_channel_, &text_level_, &text_counts_, &text_note_,
                  &recent_entries_view_, &recent_entry_detail_view_});

    recent_entry_detail_view_.hidden(true);

    /* Honesty rule: the host receive tap is a periodic snapshot of the wideband
     * input, not a continuous channel stream, so bursts that fall in the gaps
     * are simply never seen. Say so rather than let an empty list read as
     * "no ships nearby". Exactly 30 characters, the full screen width. */
    text_note_.set(STR_COLOR_YELLOW "Snapshot tap - bursts may drop");

    options_channel_.on_change = [this](size_t, ui::OptionsField::value_t v) {
        if (auto* rx = globals().receiver) rx->set_target_frequency(static_cast<uint64_t>(v));
        configure_front_end();
    };
    options_channel_.set_by_value(static_cast<int32_t>(channel_88b_hz), false);

    recent_entries_view_.on_select = [this](const AISRecentEntry& entry) {
        on_show_detail(entry);
    };
    recent_entry_detail_view_.on_close = [this]() { on_show_list(); };

    recent_entries_view_.table().on_draw =
        [](const AISRecentEntry& entry, const ui::Rect& target_rect, ui::Painter& painter,
           const ui::Style& s, ui::RecentEntriesColumns&) {
            std::string line = ais::format::mmsi(entry.mmsi) + " ";
            if (!entry.name.empty())
                line += ais::format::text(entry.name);
            else
                line += ais::format::text(entry.call_sign);

            const auto chars = static_cast<size_t>(std::max(0, target_rect.width() / 8));
            line.resize(chars, ' ');
            painter.draw_string(target_rect.location(), s, line);
        };

    decoder_.set_packet_handler([this](const ais::Packet& packet) { this->on_packet(packet); });

    logger_.open();
}

AISAppView::~AISAppView() {
    logger_.close();

    auto* rx = globals().receiver;
    if (rx == nullptr) return;

    /* Put the receiver back the way it was found. */
    if (previous_mode_ >= 0) {
        if (!receiver_was_running_ && rx->running()) rx->stop();
        rx->set_mode(static_cast<radio::ReceiverModel::Mode>(previous_mode_));
        rx->set_sampling_rate(previous_sample_rate_);
        rx->set_target_frequency(previous_frequency_);
    }
}

void AISAppView::set_parent_rect(const ui::Rect new_parent_rect) {
    ui::View::set_parent_rect(new_parent_rect);

    const ui::Rect content_rect{0, header_height, new_parent_rect.width(),
                                new_parent_rect.height() - header_height};
    recent_entries_view_.set_parent_rect(content_rect);
    recent_entry_detail_view_.set_parent_rect(content_rect);
}

void AISAppView::focus() {
    options_channel_.focus();
}

void AISAppView::on_show() {
    ui::View::on_show();

    auto* rx = globals().receiver;
    if (rx == nullptr) {
        update_status();
        return;
    }

    /* Only the first show captures the previous state — the view is hidden and
     * re-shown whenever the map is pushed on top of it. */
    if (previous_mode_ < 0) {
        receiver_was_running_ = rx->running();
        previous_mode_ = static_cast<int>(rx->mode());
        previous_sample_rate_ = rx->sampling_rate();
        previous_frequency_ = rx->target_frequency();
    }

    rx->set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    rx->set_sampling_rate(capture_rate_hz);
    /* AIS is two 25 kHz channels 50 kHz apart; we capture 307 kHz for
     * timing, but the analog filter only needs to pass the ~50 kHz the
     * channels occupy. 150 kHz keeps a 3x margin over that while cutting
     * ~3 dB of noise vs the rate-wide default — marine VHF from a boat near
     * the horizon needs exactly that margin. Must follow set_sampling_rate. */
    rx->set_rx_bandwidth_hint(150'000.0);
    rx->set_target_frequency(
        static_cast<uint64_t>(options_channel_.selected_index_value()));

    if (!rx->running()) rx->start();

    configure_front_end();
}

void AISAppView::on_hide() {
    /* Deliberately does not stop the receiver: the map view is pushed on top of
     * this one, which hides it, and decoding should carry on underneath. */
    ui::View::on_hide();
}

void AISAppView::configure_front_end() {
    auto* rx = globals().receiver;
    if (rx == nullptr) return;

    const double rate = rx->sampling_rate();
    if (rate <= 0.0) return;

    const double lo = (globals().radio != nullptr) ? globals().radio->rx_frequency() : 0.0;
    const double offset = static_cast<double>(rx->target_frequency()) - lo;

    if (rate == configured_rate_ && offset == configured_offset_) return;

    configured_rate_ = rate;
    configured_offset_ = offset;

    /* Bring the AIS channel from wherever it sits in the captured band down to
     * DC, exactly as ReceiverModel's own NCO does for its audio path — the
     * spectrum tap is taken before that mixer, so this one is needed. */
    nco_.set_frequency(-offset, rate);

    size_t decimation = static_cast<size_t>(std::lround(rate / 38400.0));
    if (decimation < 1) decimation = 1;
    const double channel_rate = rate / static_cast<double>(decimation);

    /* Passband to the AIS channel's ~9.6 kHz half-width, stopband by the
     * decimated Nyquist so nothing folds in. */
    auto taps = dsp::design_lowpass(9600.0, std::max(2400.0, channel_rate / 2.0 - 9600.0), rate,
                                    60.0);
    channel_filter_.configure(std::move(taps), decimation);

    decoder_.configure(static_cast<float>(channel_rate));

    max_samples_per_frame_ = 4096;
}

void AISAppView::on_frame_sync() {
    ui::View::on_frame_sync();
    process_samples();
    update_status();
}

void AISAppView::process_samples() {
    auto* rx = globals().receiver;
    if (rx == nullptr || !rx->running()) return;

    configure_front_end();

    if (!rx->take_spectrum_samples(samples_, max_samples_per_frame_)) return;
    if (samples_.empty()) return;

    nco_.mix(samples_.data(), samples_.data(), samples_.size());

    channel_.clear();
    channel_filter_.process(samples_.data(), samples_.size(), channel_);
    if (channel_.empty()) return;

    decoder_.feed(channel_.data(), channel_.size());
}

void AISAppView::on_packet(const ais::Packet& packet) {
    /* Drop a frame the snapshot tap handed us twice. See last_frame_hex_. */
    const std::string hex = packet.to_hex_nibbles();
    const auto now = std::chrono::steady_clock::now();
    if (hex == last_frame_hex_ &&
        (now - last_frame_time_) < std::chrono::milliseconds(500)) {
        return;
    }
    last_frame_hex_ = hex;
    last_frame_time_ = now;

    logger_.on_packet(packet);

    auto& entry = ui::on_packet(recent_, packet.source_id());
    entry.update(packet);
    recent_entries_view_.set_dirty();

    if (entry.key() == recent_entry_detail_view_.entry().key()) {
        recent_entry_detail_view_.set_entry(entry);
        recent_entry_detail_view_.update_position();
    }

    if (!recent_entry_detail_view_.hidden())
        recent_entry_detail_view_.update_map_markers(recent_);
}

void AISAppView::on_show_list() {
    recent_entries_view_.hidden(false);
    recent_entry_detail_view_.hidden(true);
    recent_entries_view_.focus();
}

void AISAppView::on_show_detail(const AISRecentEntry& entry) {
    recent_entries_view_.hidden(true);
    recent_entry_detail_view_.hidden(false);
    recent_entry_detail_view_.set_entry(entry);
    recent_entry_detail_view_.focus();
    recent_entry_detail_view_.update_map_markers(recent_);
}

void AISAppView::update_status() {
    auto* rx = globals().receiver;

    /* Honesty rule: never let an empty list read as "no ships nearby" when the
     * real reason is that nothing is being received. Exactly 30 characters, the
     * full width of the screen. */
    const char* note = nullptr;
    if (rx == nullptr)
        note = STR_COLOR_RED "No receiver: cannot decode  ";
    else if (!rx->running())
        note = STR_COLOR_RED "Receiver stopped              ";
    else
        note = STR_COLOR_YELLOW "Snapshot tap - bursts may drop";
    if (note != text_note_.get()) text_note_.set(note);

    std::string level = "---";
    if (rx != nullptr && rx->running()) {
        const int db = static_cast<int>(std::lround(rx->rf_level_db()));
        level = to_string_dec_int(db) + "dB";
    }
    if (level != text_level_.get()) text_level_.set(level);

    const std::string counts = "P" + to_string_dec_uint(decoder_.packets_valid()) + " X" +
                               to_string_dec_uint(decoder_.crc_failures() +
                                                  decoder_.length_failures());
    if (counts != text_counts_.get()) text_counts_.set(counts);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_ais{{"ais", "AIS Boats", app::Category::Receive, ui::Color::green(),
                              &ui::bitmap_icon_ais,
                              [] { return std::make_unique<app::AISAppView>(); }}};
}  // namespace
