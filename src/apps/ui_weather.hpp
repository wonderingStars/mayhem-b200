/*
 * mayhem-b200 — Weather / TPMS receiver.
 *
 * Ported from the PortaPack firmware:
 *
 *   application/external/tpmsrx/tpms_app.*  -> the app, its recent-entries
 *                                              table and the detail view
 *   common/tpms_packet.*                    -> the three Schrader frame
 *                                              formats, their CRC/checksums and
 *                                              their field layouts
 *   common/manchester.*                     -> the Manchester symbol decoder
 *                                              (Phase A's dsp::manchester_*)
 *   baseband/proc_tpms.*                    -> the receive pipeline: channel
 *                                              decimation, matched filter,
 *                                              clock recovery, OOK slicer and
 *                                              the three PacketBuilders
 *
 * WHAT IS AND IS NOT PORTED
 *
 * The three tyre-pressure formats upstream decodes are all here, bit-for-bit:
 * FSK 19k2 Schrader (the FLM_64 / FLM_72 / FLM_80 length variants, told apart by
 * which checksum validates), OOK 8k192 Schrader, and OOK 8k4 Schrader (GMC_96).
 * Upstream's *separate* weather-station app (application/apps/ui_weatherstation.*
 * plus baseband/fprotos/) decodes a different family — Nexus, Acurite, Oregon
 * and friends — from its own protocol library. That library is not ported, and
 * the view says so on screen rather than showing an empty table that looks like
 * "nothing is transmitting".
 *
 * WHERE THE SAMPLES COME FROM
 *
 * On a PortaPack the M4 core runs proc_tpms and hands finished packets to the
 * app over a message queue. There is no second core here, so TpmsDecoder is
 * driven from the view's on_frame_sync(). The only sample tap ReceiverModel
 * exposes is take_spectrum_samples(), which is a *wideband, pre-channel-filter,
 * non-contiguous* snapshot: it hands back the tail of the most recent DSP block
 * and drops the rest. That is enough to prove the pipeline runs, and the decoder
 * itself is correct when fed a continuous stream (see tests/test_tpms_ert.cpp),
 * but frames straddling a gap between snapshots are lost.
 *
 *   IDEAL TAP: a continuous complex-baseband callback from ReceiverModel,
 *   already mixed to the tuned channel and decimated to a decoder-chosen rate —
 *   the host equivalent of BasebandProcessor::execute(buffer). Something like
 *   `set_channel_sink(std::function<void(const cfloat*, size_t)>, double rate)`.
 *   Until that exists this app cannot receive reliably, and says so.
 *
 * WHY THE DECODER IS HEADER-INLINE
 *
 * So tests/test_tpms_ert.cpp can exercise the frame formats, the checksums and
 * the whole sample-to-reading pipeline without linking the UI, the display or a
 * radio. Only the two ui::View subclasses live in the .cpp.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2023 Mark Thompson (original app and packet decoders)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_WEATHER_H__
#define __MB200_UI_WEATHER_H__

#include "../core/string_format.hpp"
#include "../dsp/demod_digital.hpp"
#include "../dsp/fir.hpp"
#include "../dsp/protocol.hpp"
#include "ui.hpp"
#include "ui_recent_entries.hpp"
#include "ui_widget.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace app {
namespace tpms {

/* --- Units (common/units.hpp) ---------------------------------------------- */

/* Pressure is carried in kilopascal and Temperature in Celsius, both int16 as
 * upstream does, so the integer truncation of the conversions matches. */
class Pressure {
   public:
    constexpr Pressure() : kpa_{0} {}
    constexpr explicit Pressure(int kilopascal)
        : kpa_{static_cast<int16_t>(kilopascal)} {}

    int kilopascal() const { return kpa_; }
    int psi() const { return kpa_ * 1000 / 6895; }
    int bar() const { return kpa_ / 100; }

   private:
    int16_t kpa_;
};

class Temperature {
   public:
    constexpr Temperature() : c_{0} {}
    constexpr explicit Temperature(int celsius) : c_{static_cast<int16_t>(celsius)} {}

    int celsius() const { return c_; }
    int fahrenheit() const { return (c_ * 9 / 5) + 32; }

   private:
    int16_t c_;
};

enum class PressureUnit : uint8_t { Kpa = 0, Psi = 1, Bar = 2 };
enum class TempUnit : uint8_t { Celsius = 0, Fahrenheit = 1 };

/* --- Signal types (tpms::SignalType) --------------------------------------- */

enum class SignalType : uint8_t {
    Fsk19k2Schrader = 1,
    Ook8k192Schrader = 2,
    Ook8k4Schrader = 3,
};

inline const char* signal_type_name(const SignalType t) {
    switch (t) {
        case SignalType::Fsk19k2Schrader:
            return "FSK 38400 19200 Schrader";
        case SignalType::Ook8k192Schrader:
            return "OOK - 8192 Schrader";
        case SignalType::Ook8k4Schrader:
            return "OOK - 8400 Schrader";
        default:
            return "- - - -";
    }
}

/* --- A decoded reading (tpms::Reading) ------------------------------------- */

struct Reading {
    enum class Type : uint8_t {
        None = 0,
        FLM_64 = 1,
        FLM_72 = 2,
        FLM_80 = 3,
        Schrader = 4,
        GMC_96 = 5,
    };

    Type type{Type::None};
    uint32_t id{0};

    bool has_pressure{false};
    Pressure pressure{};
    bool has_temperature{false};
    Temperature temperature{};
    bool has_flags{false};
    uint8_t flags{0};

    bool valid() const { return type != Type::None; }
};

inline const char* reading_type_name(const Reading::Type t) {
    switch (t) {
        case Reading::Type::None:
            return "None";
        case Reading::Type::FLM_64:
            return "FLM_64";
        case Reading::Type::FLM_72:
            return "FLM_72";
        case Reading::Type::FLM_80:
            return "FLM_80";
        case Reading::Type::Schrader:
            return "Schrader";
        case Reading::Type::GMC_96:
            return "GMC_96";
        default:
            return "Unknown";
    }
}

/* --- Field reader ----------------------------------------------------------
 *
 * common/field_reader.hpp's FieldReader<ManchesterDecoder, BitRemapNone>:
 * `start_bit` becomes the MSB of the result, and a read past the end of the
 * packet yields zero bits (upstream's ManchesterDecoder returns {0, error} for
 * an out-of-range index). Several of the checksum routines below rely on that,
 * so it is reproduced exactly rather than clamped or reported as an error. */
inline uint32_t read_field(const std::vector<uint8_t>& bits,
                           const size_t start_bit,
                           const size_t length) {
    uint32_t value = 0;
    for (size_t i = start_bit; i < (start_bit + length); i++) {
        const uint32_t b = (i < bits.size()) ? static_cast<uint32_t>(bits[i] & 1u) : 0u;
        value = (value << 1) | b;
    }
    return value;
}

/* --- Frame validation and field extraction (common/tpms_packet.cpp) --------- */

/* Which of the three FSK Schrader frame lengths validates, or 0 for none.
 *
 * 80 bits: CRC-8, truncated polynomial 0x01 (x^8 + 1), over bytes 1..9, residue
 *          zero.
 * 72 bits: the same CRC over bytes 0..8, residue zero.
 * 64 bits: the byte-sum of bytes 0..6 equals byte 7.
 *
 * Ten bytes are always read; bytes past the end of the frame read as zero. The
 * longest candidate wins, exactly as upstream. */
inline size_t crc_valid_length(const std::vector<uint8_t>& bits) {
    constexpr uint32_t checksum_bytes = 0b1111111u;
    constexpr uint32_t crc_72_bytes = 0b111111111u;
    constexpr uint32_t crc_80_bytes = 0b1111111110u;

    std::array<uint8_t, 10> bytes{};
    for (size_t i = 0; i < bytes.size(); i++) {
        bytes[i] = static_cast<uint8_t>(read_field(bits, i * 8, 8));
    }

    uint32_t checksum = 0;
    dsp::Crc<8> crc_72{0x01, 0x00};
    dsp::Crc<8> crc_80{0x01, 0x00};

    for (size_t i = 0; i < bytes.size(); i++) {
        const uint32_t byte_mask = 1u << i;
        const uint8_t byte = bytes[i];

        if (checksum_bytes & byte_mask) checksum += byte;
        if (crc_72_bytes & byte_mask) crc_72.process_byte(byte);
        if (crc_80_bytes & byte_mask) crc_80.process_byte(byte);
    }

    if (crc_80.checksum() == 0) return 80;
    if (crc_72.checksum() == 0) return 72;
    if ((checksum & 0xffu) == bytes[7]) return 64;
    return 0;
}

/* FSK 19k2 Schrader. Which fields exist depends on which length validated.
 * Pressure is (raw * 4 / 3) kPa in all three; temperature is (raw - 56) C, with
 * the 64-bit variant masking off the top bit of the temperature byte. */
inline Reading reading_fsk_19k2_schrader(const std::vector<uint8_t>& bits) {
    Reading r{};
    const size_t length = crc_valid_length(bits);

    switch (length) {
        case 64:
            r.type = Reading::Type::FLM_64;
            r.id = read_field(bits, 0, 32);
            r.has_pressure = true;
            r.pressure = Pressure{static_cast<int>(read_field(bits, 32, 8)) * 4 / 3};
            r.has_temperature = true;
            r.temperature =
                Temperature{static_cast<int>(read_field(bits, 40, 8) & 0x7fu) - 56};
            return r;

        case 72:
            r.type = Reading::Type::FLM_72;
            r.id = read_field(bits, 0, 32);
            r.has_pressure = true;
            r.pressure = Pressure{static_cast<int>(read_field(bits, 40, 8)) * 4 / 3};
            r.has_temperature = true;
            r.temperature = Temperature{static_cast<int>(read_field(bits, 48, 8)) - 56};
            return r;

        case 80:
            r.type = Reading::Type::FLM_80;
            r.id = read_field(bits, 8, 32);
            r.has_pressure = true;
            r.pressure = Pressure{static_cast<int>(read_field(bits, 48, 8)) * 4 / 3};
            r.has_temperature = true;
            r.temperature = Temperature{static_cast<int>(read_field(bits, 56, 8)) - 56};
            return r;

        default:
            return r;
    }
}

/* OOK 8k192 Schrader, 37 decoded symbols:
 *
 *   [0:3)   function code / flags
 *   [3:27)  transponder ID
 *   [27:35) pressure, (raw * 4 / 3) kPa
 *   [35:37) checksum
 *
 * The checksum is the two low bits of (bit 0) + (the eighteen 2-bit groups
 * starting at bit 1, which includes the checksum field itself); a valid frame
 * sums to 3. No temperature in this format. */
inline Reading reading_ook_8k192_schrader(const std::vector<uint8_t>& bits) {
    Reading r{};

    const uint32_t flags = read_field(bits, 0, 3);
    const uint32_t checksum = read_field(bits, 35, 2);

    uint32_t checksum_calculated = read_field(bits, 0, 1);
    for (size_t i = 1; i < 37; i += 2) {
        checksum_calculated += read_field(bits, i, 2);
    }

    if ((checksum_calculated & 3u) != 3u) return r;

    r.type = Reading::Type::Schrader;
    r.id = read_field(bits, 3, 24);
    r.has_pressure = true;
    r.pressure = Pressure{static_cast<int>(read_field(bits, 27, 8)) * 4 / 3};
    r.has_flags = true;
    r.flags = static_cast<uint8_t>((flags << 4) | checksum);
    return r;
}

/* OOK 8k4 Schrader (GMC_96), 76 decoded symbols:
 *
 *   [0:20)  rest of the system ID (its first nibble, 0x4, was consumed by the
 *           preamble match and is assumed — upstream's note, kept)
 *   [20:52) transponder ID
 *   [52:60) pressure, (raw * 11 / 4) kPa
 *   [60:68) temperature, (raw - 61) C
 *   [68:76) checksum: the 8-bit sum of the nine bytes starting with the system
 *           ID nibble pair
 */
inline Reading reading_ook_8k4_schrader(const std::vector<uint8_t>& bits) {
    Reading r{};

    constexpr uint32_t first_nibble = 0x4u;

    const uint32_t id = read_field(bits, 20, 32);
    const uint32_t value_0 = read_field(bits, 52, 8);
    const uint32_t value_1 = read_field(bits, 60, 8);
    const uint32_t checksum = read_field(bits, 68, 8);

    uint8_t checksum_calculated =
        static_cast<uint8_t>((first_nibble << 4) | read_field(bits, 0, 4));
    for (size_t i = 4; i < 68; i += 8) {
        checksum_calculated =
            static_cast<uint8_t>(checksum_calculated + read_field(bits, i, 8));
    }

    if (checksum_calculated != checksum) return r;

    r.type = Reading::Type::GMC_96;
    r.id = id;
    r.has_pressure = true;
    r.pressure = Pressure{static_cast<int>(value_0) * 11 / 4};
    r.has_temperature = true;
    r.temperature = Temperature{static_cast<int>(value_1) - 61};
    return r;
}

/* Dispatch on signal type. `bits` are Manchester-*decoded* data bits. */
inline Reading decode_reading(const SignalType signal_type,
                              const std::vector<uint8_t>& bits) {
    switch (signal_type) {
        case SignalType::Fsk19k2Schrader:
            return reading_fsk_19k2_schrader(bits);
        case SignalType::Ook8k192Schrader:
            return reading_ook_8k192_schrader(bits);
        case SignalType::Ook8k4Schrader:
            return reading_ook_8k4_schrader(bits);
        default:
            return Reading{};
    }
}

/* The same, starting from the raw Manchester chips a PacketBuilder produced.
 * Manchester sense 0 throughout, matching tpms::Packet's `decoder_{packet_, 0}`. */
inline Reading decode_chips(const SignalType signal_type,
                            const std::vector<uint8_t>& chips) {
    const auto bits = dsp::manchester_decode(chips, 0);
    return decode_reading(signal_type, bits);
}

/* common/manchester.cpp's format_symbols(): the decoded symbols as hex, plus a
 * parallel hex string carrying a 1 wherever a chip pair had no transition. */
struct FormattedSymbols {
    std::string data;
    std::string errors;
};

inline FormattedSymbols format_symbols(const std::vector<uint8_t>& chips) {
    const size_t symbols = chips.size() / 2;
    const size_t hex_chars = (symbols + 3) / 4;
    const size_t rounded = hex_chars * 4;

    std::string hex_data;
    std::string hex_error;
    hex_data.reserve(hex_chars);
    hex_error.reserve(hex_chars);

    uint_fast8_t data = 0;
    uint_fast8_t error = 0;
    for (size_t i = 0; i < rounded; i++) {
        const auto symbol = dsp::manchester_symbol(chips.data(), chips.size(), i, 0);

        data = static_cast<uint_fast8_t>((data << 1) | (symbol.value & 1u));
        error = static_cast<uint_fast8_t>((error << 1) | (symbol.error & 1u));

        if ((i & 3u) == 3u) {
            hex_data += to_string_hex(data & 0xfu, 1);
            hex_error += to_string_hex(error & 0xfu, 1);
        }
    }

    return {hex_data, hex_error};
}

/* --- Receive pipeline (baseband/proc_tpms.*) -------------------------------
 *
 * Structure, and every constant, from TPMSProcessor:
 *
 *   input -> decimate to ~307.2 kHz (channel rate)
 *         -> FSK: matched filter (38.4 kHz tone, one symbol long, decimating to
 *                 2x the 19200 symbol rate) -> Gardner clock recovery
 *                 -> slice at zero -> PacketBuilder(30-bit preamble, 160 chips)
 *         -> OOK: every 2nd channel sample (153.6 kHz), magnitude-squared
 *                 -> adaptive slicer -> two early-late symbol clocks at 8192
 *                 and 8400 baud -> PacketBuilder(24-bit / 32-bit preamble,
 *                 74 / 152 chips)
 *
 * Two deliberate departures, both harmless and both stated rather than hidden:
 *
 *   1. The channel rate is `input_rate / round(input_rate / 307200)` rather than
 *      exactly 307200, because the host's capture rate is whatever the USRP was
 *      set to and is not generally a multiple of 307.2 kHz. Every downstream
 *      constant is derived from the achieved rate, so the symbol timing stays
 *      correct.
 *   2. Upstream runs one OOK slicer into two clock recoveries; Phase A's
 *      OokDemod bundles slicer and clock, so there are two slicers here, each
 *      configured for its own baud. The slicer's only rate-dependent parameter
 *      is its leak (1/8 dB per symbol), which 8192 vs 8400 baud changes by 2.5%.
 */
class TpmsDecoder {
   public:
    /* Called with the Manchester chips of each candidate frame, before any
     * checksum is applied — the same point at which proc_tpms pushes a
     * TPMSPacketMessage. */
    using PacketHandler = std::function<void(SignalType, const std::vector<uint8_t>&)>;

    static constexpr double nominal_channel_rate = 307200.0;
    static constexpr double fsk_symbol_rate = 19200.0;
    static constexpr double fsk_tone_rate = 38400.0;
    static constexpr double ook_8k192_rate = 8192.0;
    static constexpr double ook_8k4_rate = 8400.0;

    void set_packet_handler(PacketHandler handler) { handler_ = std::move(handler); }

    /* Rebuilds the chain for a new input sample rate; a no-op if unchanged. */
    void configure(const double input_sample_rate_hz) {
        if (!(input_sample_rate_hz > 0.0)) return;
        if (configured_ && input_sample_rate_hz == input_rate_) return;

        input_rate_ = input_sample_rate_hz;

        const double raw_decim = input_sample_rate_hz / nominal_channel_rate;
        decimation_ = (raw_decim < 1.5) ? size_t{1}
                                        : static_cast<size_t>(std::lround(raw_decim));
        channel_rate_ = input_sample_rate_hz / static_cast<double>(decimation_);

        /* Channel filter: proc_tpms's two decimation stages use the shared
         * taps_200k_* pair, i.e. a ~200 kHz-wide channel. Same width here. */
        if (decimation_ > 1) {
            decimator_.configure(
                dsp::design_lowpass(100000.0, 40000.0, input_sample_rate_hz, 60.0),
                decimation_);
        } else {
            decimator_.configure(std::vector<float>{1.0f}, 1);
        }

        /* Matched filter: one symbol long, two cycles of the 38.4 kHz tone,
         * decimating to twice the symbol rate. At the nominal 307.2 kHz this is
         * upstream's rect_taps_307k2_38k4_1t_19k2_p exactly (16 taps of
         * magnitude 1/16, decimation 8). */
        mf_decimation_ = static_cast<size_t>(
            std::lround(channel_rate_ / fsk_tone_rate));
        if (mf_decimation_ < 1) mf_decimation_ = 1;
        const size_t taps_count = mf_decimation_ * 2;
        matched_filter_.configure(
            dsp::design_matched_filter_taps(fsk_tone_rate, channel_rate_, taps_count),
            mf_decimation_);

        const float mf_out_rate =
            static_cast<float>(channel_rate_ / static_cast<double>(mf_decimation_));
        clock_fsk_.configure(mf_out_rate, static_cast<float>(fsk_symbol_rate),
                             dsp::FixedErrorFilter{0.0555f});
        clock_fsk_.set_symbol_handler([this](const float raw_symbol) {
            builder_fsk_.execute((raw_symbol >= 0.0f) ? uint_fast8_t{1} : uint_fast8_t{0});
        });

        /* Preamble: 010101...0110, 30 chips, one bit of slack. Payload: 160
         * chips = 80 Manchester symbols. */
        builder_fsk_.configure(
            dsp::BitPattern{0b010101010101010101010101010110ull, 30, 1}, dsp::NeverMatch{});
        builder_fsk_.set_end_matcher(dsp::FixedLength{160});
        builder_fsk_.set_payload_handler([this](const dsp::Packet& p) {
            count_fsk_++;
            emit(SignalType::Fsk19k2Schrader, p);
        });

        const float ook_sample_rate = static_cast<float>(channel_rate_ * 0.5);
        ook_8k192_.configure(ook_sample_rate, static_cast<float>(ook_8k192_rate));
        ook_8k4_.configure(ook_sample_rate, static_cast<float>(ook_8k4_rate));

        /* Preamble 11*2, 01*14, 11, 10 — its last 24 chips. Payload 37
         * Manchester symbols. */
        builder_8k192_.configure(
            dsp::BitPattern{0b010101010101010101011110ull, 24, 0}, dsp::NeverMatch{});
        builder_8k192_.set_end_matcher(dsp::FixedLength{37 * 2});
        builder_8k192_.set_payload_handler([this](const dsp::Packet& p) {
            count_8k192_++;
            emit(SignalType::Ook8k192Schrader, p);
        });

        /* Preamble 01*40 then 01, 10, 01, 01 — its last 32 chips. Payload 76
         * Manchester symbols. */
        builder_8k4_.configure(
            dsp::BitPattern{0b01010101010101010101010101100101ull, 32, 0},
            dsp::NeverMatch{});
        builder_8k4_.set_end_matcher(dsp::FixedLength{76 * 2});
        builder_8k4_.set_payload_handler([this](const dsp::Packet& p) {
            count_8k4_++;
            emit(SignalType::Ook8k4Schrader, p);
        });

        configured_ = true;
        reset();
    }

    bool configured() const { return configured_; }

    void reset() {
        matched_filter_.reset();
        clock_fsk_.reset();
        builder_fsk_.reset();
        ook_8k192_.reset();
        ook_8k4_.reset();
        builder_8k192_.reset();
        builder_8k4_.reset();
    }

    void process(const dsp::cfloat* in, const size_t count) {
        if (!configured_ || in == nullptr || count == 0) return;

        channel_.clear();
        decimator_.process(in, count, channel_);

        /* FSK arm — every channel sample. */
        for (const auto& s : channel_) {
            if (matched_filter_.execute_once(s)) {
                clock_fsk_(matched_filter_.get_output());
            }
        }

        /* OOK arm — every second channel sample, magnitude squared. */
        ook_mag2_.clear();
        ook_mag2_.reserve((channel_.size() + 1) / 2);
        for (size_t i = 0; i < channel_.size(); i += 2) {
            const auto& s = channel_[i];
            ook_mag2_.push_back(s.real() * s.real() + s.imag() * s.imag());
        }

        ook_bits_.clear();
        ook_8k192_.process_mag2(ook_mag2_.data(), ook_mag2_.size(), ook_bits_);
        for (const uint8_t bit : ook_bits_) builder_8k192_.execute(bit);

        ook_bits_.clear();
        ook_8k4_.process_mag2(ook_mag2_.data(), ook_mag2_.size(), ook_bits_);
        for (const uint8_t bit : ook_bits_) builder_8k4_.execute(bit);
    }

    void process(const std::vector<dsp::cfloat>& in) { process(in.data(), in.size()); }

    double input_rate() const { return input_rate_; }
    double channel_rate() const { return channel_rate_; }
    double ook_rate() const { return channel_rate_ * 0.5; }
    size_t decimation() const { return decimation_; }
    size_t matched_filter_decimation() const { return mf_decimation_; }

    /* Candidate frames handed to the packet handler, per format — not
     * checksum-validated readings. */
    size_t packets_fsk() const { return count_fsk_; }
    size_t packets_ook_8k192() const { return count_8k192_; }
    size_t packets_ook_8k4() const { return count_8k4_; }

   private:
    using Builder = dsp::PacketBuilder<dsp::BitPattern, dsp::NeverMatch, dsp::FixedLength>;

    void emit(const SignalType type, const dsp::Packet& packet) {
        if (!handler_) return;
        handler_(type, packet.bits());
    }

    PacketHandler handler_{};

    dsp::FirDecimateC decimator_{};
    dsp::MatchedFilter matched_filter_{};
    dsp::ClockRecovery<dsp::FixedErrorFilter> clock_fsk_{};
    Builder builder_fsk_{};

    dsp::OokDemod ook_8k192_{};
    dsp::OokDemod ook_8k4_{};
    Builder builder_8k192_{};
    Builder builder_8k4_{};

    std::vector<dsp::cfloat> channel_{};
    std::vector<float> ook_mag2_{};
    std::vector<uint8_t> ook_bits_{};

    double input_rate_{0.0};
    double channel_rate_{0.0};
    size_t decimation_{1};
    size_t mf_decimation_{1};
    size_t count_fsk_{0};
    size_t count_8k192_{0};
    size_t count_8k4_{0};
    bool configured_{false};
};

/* --- Recent-entries table (TPMSRecentEntry) -------------------------------- */

struct RecentEntry {
    using Key = std::pair<Reading::Type, uint32_t>;

    static const Key invalid_key;

    Reading::Type type{Reading::Type::None};
    uint32_t id{0};
    SignalType signal_type{SignalType::Ook8k192Schrader};

    size_t received_count{0};

    bool has_pressure{false};
    Pressure last_pressure{};
    bool has_temperature{false};
    Temperature last_temperature{};
    bool has_flags{false};
    uint8_t last_flags{0};

    explicit RecentEntry(const Key& key) : type{key.first}, id{key.second} {}

    Key key() const { return {type, id}; }

    void update(const Reading& reading) {
        received_count++;

        if (reading.has_pressure) {
            last_pressure = reading.pressure;
            has_pressure = true;
        }
        if (reading.has_temperature) {
            last_temperature = reading.temperature;
            has_temperature = true;
        }
        if (reading.has_flags) {
            last_flags = reading.flags;
            has_flags = true;
        }
    }
};

inline const RecentEntry::Key RecentEntry::invalid_key{Reading::Type::None, 0};

using RecentEntries = ui::RecentEntries<RecentEntry>;

/* Column text, factored out so the table renderer and the tests agree. */
inline std::string format_pressure(const Pressure p, const PressureUnit unit) {
    const int v = (unit == PressureUnit::Psi)   ? p.psi()
                  : (unit == PressureUnit::Bar) ? p.bar()
                                                : p.kilopascal();
    return to_string_dec_int(v, 3);
}

inline std::string format_temperature(const Temperature t, const TempUnit unit) {
    const int v = (unit == TempUnit::Fahrenheit) ? t.fahrenheit() : t.celsius();
    return to_string_dec_int(v, 3);
}

inline const char* pressure_unit_name(const PressureUnit unit) {
    switch (unit) {
        case PressureUnit::Psi:
            return "PSI";
        case PressureUnit::Bar:
            return "BAR";
        default:
            return "kPa";
    }
}

}  // namespace tpms

/* --- Views ----------------------------------------------------------------- */

class TpmsDetailView : public ui::View {
   public:
    TpmsDetailView(const tpms::RecentEntry& entry,
                   tpms::PressureUnit pressure_unit,
                   tpms::TempUnit temp_unit);

    std::string title() const override { return "TPMS Detail"; }
    void on_show() override;

   private:
    ui::Labels labels_{
        {{0, 1 * 16}, "Type:", ui::Color::light_grey()},
        {{0, 3 * 16}, "ID:", ui::Color::light_grey()},
        {{0, 5 * 16}, "Signal:", ui::Color::light_grey()},
        {{0, 7 * 16}, "Pressure:", ui::Color::light_grey()},
        {{0, 9 * 16}, "Temperature:", ui::Color::light_grey()},
        {{0, 11 * 16}, "Flags:", ui::Color::light_grey()},
        {{0, 13 * 16}, "Count:", ui::Color::light_grey()},
    };

    ui::Text text_type_{{13 * 8, 1 * 16, 17 * 8, 16}, ""};
    ui::Text text_id_{{13 * 8, 3 * 16, 17 * 8, 16}, ""};
    ui::Text text_signal_{{0, 6 * 16, 30 * 8, 16}, ""};
    ui::Text text_pressure_{{13 * 8, 7 * 16, 17 * 8, 16}, ""};
    ui::Text text_temperature_{{13 * 8, 9 * 16, 17 * 8, 16}, ""};
    ui::Text text_flags_{{13 * 8, 11 * 16, 17 * 8, 16}, ""};
    ui::Text text_count_{{13 * 8, 13 * 16, 17 * 8, 16}, ""};

    ui::Button button_done_{{8 * 8, 15 * 16, 14 * 8, 32}, "Done"};
};

class WeatherView : public ui::View {
   public:
    WeatherView();

    WeatherView(const WeatherView&) = delete;
    WeatherView& operator=(const WeatherView&) = delete;

    std::string title() const override { return "Weather/TPMS"; }

    void on_show() override;
    void on_frame_sync() override;
    void set_parent_rect(const ui::Rect new_parent_rect) override;

   private:
    static constexpr ui::Dim header_height = 32;

    void on_frame(tpms::SignalType signal_type, const std::vector<uint8_t>& chips);
    void update_status();

    tpms::TpmsDecoder decoder_{};
    tpms::RecentEntries recent_{};

    tpms::PressureUnit pressure_unit_{tpms::PressureUnit::Kpa};
    tpms::TempUnit temp_unit_{tpms::TempUnit::Celsius};

    std::vector<dsp::cfloat> samples_{};
    uint32_t frame_counter_{0};
    size_t candidates_{0};
    size_t decoded_{0};

    ui::OptionsField options_band_{
        {0, 0},
        5,
        {{"314.9", 314900000},
         {"315.0", 315000000},
         {"433.9", 433920000}}};

    ui::OptionsField options_pressure_{
        {6 * 8, 0},
        3,
        {{"kPa", static_cast<int32_t>(tpms::PressureUnit::Kpa)},
         {"PSI", static_cast<int32_t>(tpms::PressureUnit::Psi)},
         {"BAR", static_cast<int32_t>(tpms::PressureUnit::Bar)}}};

    ui::OptionsField options_temp_{
        {10 * 8, 0},
        1,
        {{"C", static_cast<int32_t>(tpms::TempUnit::Celsius)},
         {"F", static_cast<int32_t>(tpms::TempUnit::Fahrenheit)}}};

    ui::NumberField field_gain_{{12 * 8, 0}, 3, {0, 76}, 1, ' '};

    ui::Text text_status_{{16 * 8, 0, 14 * 8, 16}, ""};
    ui::Text text_note_{{0, 16, 240, 16}, ""};

    ui::RecentEntriesColumns columns_{{
        {"Tp", 2},
        {"ID", 0},
        {"Pres", 4},
        {"Temp", 4},
        {"Cnt", 3},
        {"Fl", 2},
    }};

    ui::RecentEntriesView<tpms::RecentEntries> recent_view_{columns_, recent_};
};

}  // namespace app

#endif /*__MB200_UI_WEATHER_H__*/
