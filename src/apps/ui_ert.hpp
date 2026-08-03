/*
 * mayhem-b200 — ERT (Encoder Receiver Transmitter) utility meter receiver.
 *
 * Ported from the PortaPack firmware:
 *
 *   application/external/ert/ert_app.*  -> the app and its recent-entries table
 *   common/ert_packet.*                 -> SCM / SCM+ / IDM field layouts, the
 *                                          SCM BCH check and the CCITT check
 *   common/manchester.*                 -> the Manchester symbol decoder
 *                                          (Phase A's dsp::manchester_*)
 *   baseband/proc_ert.*                 -> the receive pipeline: envelope
 *                                          integration over half a symbol, the
 *                                          Manchester matched response, clock
 *                                          recovery and the three PacketBuilders
 *
 * ERT is the telemetry Itron/Landis+Gyr electricity, gas and water meters
 * broadcast in the 902-928 MHz ISM band. Three frame types matter:
 *
 *   SCM      96 bits, 21-bit preamble/sync (0x1F2A60), 16-bit BCH (poly 0x6F63)
 *   SCM+     Manchester sync 0x16A3, 14 payload bytes, CRC-16/CCITT
 *   IDM      Manchester sync 0x555516A3, 88 payload bytes, CRC-16/CCITT
 *
 * All three are Manchester-coded at 32768 baud, so one receive chain feeds three
 * frame detectors — exactly as upstream does.
 *
 * WHERE THE SAMPLES COME FROM
 *
 * There is no M4 core here, so ErtDecoder is driven from the view's
 * on_frame_sync(). The only sample tap ReceiverModel exposes is
 * take_spectrum_samples(): a wideband, pre-channel-filter snapshot of the tail
 * of the most recent DSP block, with the rest dropped. That happens to suit ERT
 * better than most decoders — proc_ert also runs straight off the wideband
 * stream with no channel filter, its half-symbol envelope integrator being the
 * only filtering — but the snapshot is still *not contiguous*, so frames that
 * straddle a gap are lost.
 *
 *   IDEAL TAP: a continuous complex-baseband callback from ReceiverModel at a
 *   decoder-chosen rate, the host equivalent of
 *   BasebandProcessor::execute(buffer). Until that exists this app cannot
 *   receive reliably, and says so on screen.
 *
 * WHY THE DECODER IS HEADER-INLINE
 *
 * So tests/test_tpms_ert.cpp can exercise the frame formats, the checks and the
 * sample-to-reading pipeline without linking the UI, the display or a radio.
 * Only the ui::View subclass lives in the .cpp.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2023 Mark Thompson (original app and packet decoders)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_ERT_H__
#define __MB200_UI_ERT_H__

#include "../core/string_format.hpp"
#include "../dsp/demod_digital.hpp"
#include "../dsp/protocol.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_recent_entries.hpp"
#include "ui_widget.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace app {
namespace ert {

using ID = uint32_t;
using Consumption = uint32_t;
using CommodityType = uint32_t;
using TamperFlags = uint32_t;

constexpr ID invalid_id = 0;
constexpr CommodityType invalid_commodity_type = static_cast<CommodityType>(-1);
constexpr Consumption invalid_consumption = 0;
constexpr TamperFlags invalid_tamper_flags = 0;

enum class PacketType : uint8_t {
    Unknown = 0,
    IDM = 1,
    SCM = 2,
    SCMPLUS = 3,
};

inline const char* packet_type_name(const PacketType t) {
    switch (t) {
        case PacketType::IDM:
            return "IDM";
        case PacketType::SCM:
            return "SCM";
        case PacketType::SCMPLUS:
            return "SCM+";
        default:
            return "???";
    }
}

/* common/field_reader.hpp semantics: `start_bit` becomes the MSB of the result,
 * and bits past the end of the packet read as zero (upstream's ManchesterDecoder
 * returns {0, error} for an out-of-range index). The SCM check below relies on
 * that — its last byte read runs two bits past a 75-bit payload. */
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

/* --- ert::Packet ----------------------------------------------------------- */

struct FormattedSymbols {
    std::string data;
    std::string errors;
};

class Packet {
   public:
    Packet() = default;

    /* From the raw Manchester chips a PacketBuilder produced. Sense 0, as
     * ert::Packet's `decoder_{packet_}`. */
    Packet(const PacketType type, std::vector<uint8_t> chips)
        : type_{type}, chips_{std::move(chips)} {
        bits_ = dsp::manchester_decode(chips_, 0, &errors_);
    }

    /* From already Manchester-decoded data bits — what the tests build, and
     * what a future logger replaying a captured frame would use. */
    static Packet from_bits(const PacketType type, std::vector<uint8_t> bits) {
        Packet p{};
        p.type_ = type;
        p.bits_ = std::move(bits);
        p.errors_.assign(p.bits_.size(), uint8_t{0});
        p.chips_ = dsp::manchester_encode(p.bits_, 0);
        return p;
    }

    PacketType type() const { return type_; }

    /* Decoded symbol count — upstream's decoder_.symbols_count(). */
    size_t length() const { return bits_.size(); }

    const std::vector<uint8_t>& bits() const { return bits_; }
    const std::vector<uint8_t>& chips() const { return chips_; }

    ID id() const {
        if (type_ == PacketType::SCM) {
            const uint32_t msb = read_field(bits_, 0, 2);
            const uint32_t lsb = read_field(bits_, 35, 24);
            return (msb << 24) | lsb;
        }
        if (type_ == PacketType::SCMPLUS) return read_field(bits_, 2 * 8, 32);
        if (type_ == PacketType::IDM) return read_field(bits_, 5 * 8, 32);
        return invalid_id;
    }

    Consumption consumption() const {
        if (type_ == PacketType::SCM) return read_field(bits_, 11, 24);
        if (type_ == PacketType::SCMPLUS) return read_field(bits_, 6 * 8, 32);
        if (type_ == PacketType::IDM) return read_field(bits_, 25 * 8, 32);
        return invalid_consumption;
    }

    CommodityType commodity_type() const {
        if (type_ == PacketType::SCM) return read_field(bits_, 5, 4);
        if (type_ == PacketType::SCMPLUS) return read_field(bits_, 1 * 8 + 4, 4);
        if (type_ == PacketType::IDM) return read_field(bits_, 4 * 8 + 4, 4);
        return invalid_commodity_type;
    }

    /* SCM packs the physical tamper count into the low nibble and the encoder
     * tamper count into the high nibble; the other two types carry a wider
     * bit field.
     *
     * IDM's field is 48 bits wide and upstream reads all 48 into a 32-bit
     * accumulator, so the leading 16 bits shift out and the result is the
     * field's low 32 bits. That truncation is reproduced here rather than
     * "fixed", because the app displays `value & 0xFFFF` and a different
     * truncation would change which bits reach the screen. */
    TamperFlags tamper_flags() const {
        if (type_ == PacketType::SCM) {
            return (read_field(bits_, 9, 2) << 4) | read_field(bits_, 3, 2);
        }
        if (type_ == PacketType::SCMPLUS) return read_field(bits_, 10 * 8, 16);
        if (type_ == PacketType::IDM) return read_field(bits_, 11 * 8, 48);
        return invalid_tamper_flags;
    }

    bool crc_ok() const {
        switch (type_) {
            case PacketType::SCM:
                return crc_ok_scm();
            case PacketType::SCMPLUS:
            case PacketType::IDM:
                return crc_ok_ccitt();
            default:
                return false;
        }
    }

    /* The 16-bit BCH the SCM frame carries, poly 0x6F63, zero init, checked for
     * a zero residue over the message.
     *
     * The five bits before the payload are the tail of the 21-bit preamble
     * 0x1F2A60, which are all zero, so upstream reads the first five payload
     * bits as a whole byte (three zero pad bits ahead of them) and lets the
     * final byte read run two bits past the 75-bit payload into zeros. Neither
     * changes the answer — a zero register stays zero when fed zeros — and
     * both are reproduced here so this agrees with upstream bit for bit. */
    bool crc_ok_scm() const {
        dsp::Crc<16> bch{0x6f63};
        constexpr size_t start_bit = 5;
        bch.process_byte(static_cast<uint8_t>(read_field(bits_, 0, start_bit)));
        for (size_t i = start_bit; i < length(); i += 8) {
            bch.process_byte(static_cast<uint8_t>(read_field(bits_, i, 8)));
        }
        return bch.checksum() == 0x0000;
    }

    /* CRC-16/CCITT, poly 0x1021, init 0xFFFF. The expected residue over the
     * whole payload is 0x1D0F rather than zero — the value rtlamr uses too —
     * so it is carried as the final XOR and the result compared against zero,
     * exactly as upstream. */
    bool crc_ok_ccitt() const {
        dsp::Crc<16> crc{0x1021, 0xffff, 0x1d0f};
        for (size_t i = 0; i < length(); i += 8) {
            crc.process_byte(static_cast<uint8_t>(read_field(bits_, i, 8)));
        }
        return crc.checksum() == 0x0000;
    }

    /* common/manchester.cpp's format_symbols(). */
    FormattedSymbols symbols_formatted() const {
        const size_t hex_chars = (bits_.size() + 3) / 4;
        const size_t rounded = hex_chars * 4;

        std::string hex_data;
        std::string hex_error;
        hex_data.reserve(hex_chars);
        hex_error.reserve(hex_chars);

        uint_fast8_t data = 0;
        uint_fast8_t error = 0;
        for (size_t i = 0; i < rounded; i++) {
            const uint_fast8_t v = (i < bits_.size()) ? uint_fast8_t(bits_[i] & 1u) : 0u;
            const uint_fast8_t e =
                (i < errors_.size()) ? uint_fast8_t(errors_[i] & 1u) : uint_fast8_t{1};

            data = static_cast<uint_fast8_t>((data << 1) | v);
            error = static_cast<uint_fast8_t>((error << 1) | e);

            if ((i & 3u) == 3u) {
                hex_data += to_string_hex(data & 0xfu, 1);
                hex_error += to_string_hex(error & 0xfu, 1);
            }
        }

        return {hex_data, hex_error};
    }

   private:
    PacketType type_{PacketType::Unknown};
    std::vector<uint8_t> chips_{};
    std::vector<uint8_t> bits_{};
    std::vector<uint8_t> errors_{};
};

/* --- Receive pipeline (baseband/proc_ert.*) --------------------------------
 *
 * ERTProcessor's front end, generalised to an arbitrary input rate:
 *
 *   |x - dc|  ->  boxcar over half a symbol  (one output per half symbol)
 *             ->  sum of two adjacent halves = one symbol's energy
 *             ->  s[n-2] - s[n]              = the Manchester matched response
 *             ->  m[n] - m[n-2]              = the differenced discriminator
 *             ->  Gardner clock recovery at the symbol rate
 *             ->  slice at zero -> three PacketBuilders
 *
 * The half-symbol integrator is the only filter, so no channel filter is needed
 * — which is exactly why upstream runs it at the full 4.194304 MHz baseband
 * rate with decimation 1.
 *
 * THE DC TRACKER IS DELIBERATELY SLOW. Upstream adds one sample per *buffer* to
 * its 2048-entry accumulator (the accumulate sits outside its per-sample loop),
 * so with 2048-sample buffers the estimate spans 2048 x 2048 samples — one
 * second at 4.194304 MHz. That is reproduced here with an explicit stride,
 * because "cleaning it up" into a per-sample average is actively wrong and this
 * port had that bug until a test caught it: an OOK signal has a mean of about
 * half its amplitude, so subtracting a fast-tracking mean makes |x| the same for
 * a mark and a space and flattens the envelope the whole decoder rests on. The
 * estimate is meant to remove the *receiver's* I/Q offset, which moves slowly,
 * not the signal's own mean.
 */
class ErtDecoder {
   public:
    /* Called with the Manchester chips of each candidate frame, before the CRC
     * is applied — the point at which proc_ert pushes an ERTPacketMessage. */
    using PacketHandler = std::function<void(PacketType, const std::vector<uint8_t>&)>;

    static constexpr double symbol_rate = 32768.0;
    static constexpr double nominal_sample_rate = 4194304.0;

    /* Manchester-coded preamble+sync words and payload lengths, from proc_ert.
     * Each code is the chip sequence; only its low `length` chips are matched. */
    static constexpr uint64_t scm_sync = 0b101010101001011001100110010110100101010101ull;
    static constexpr size_t scm_sync_length = 42 - 10;
    static constexpr size_t scm_payload_chips = 150;

    static constexpr uint64_t scmplus_sync = 0b01010110011010011001100101011010ull;
    static constexpr size_t scmplus_sync_length = 32;
    static constexpr size_t scmplus_payload_chips = 224;

    static constexpr uint64_t idm_sync =
        0b0110011001100110011001100110011001010110011010011001100101011010ull;
    static constexpr size_t idm_sync_length = 64 - 16;
    static constexpr size_t idm_payload_chips = 1408;

    void set_packet_handler(PacketHandler handler) { handler_ = std::move(handler); }

    void configure(const double input_sample_rate_hz) {
        if (!(input_sample_rate_hz > 0.0)) return;
        if (configured_ && input_sample_rate_hz == input_rate_) return;

        input_rate_ = input_sample_rate_hz;

        /* One integrator output per half symbol. At upstream's 4.194304 MHz
         * this is 64 samples, giving exactly 65536 outputs per second — twice
         * the symbol rate, which is what the Gardner detector wants. */
        const double raw = input_sample_rate_hz / (2.0 * symbol_rate);
        half_symbol_ = (raw < 1.0) ? size_t{1} : static_cast<size_t>(std::lround(raw));
        detector_rate_ = input_sample_rate_hz / static_cast<double>(half_symbol_);

        /* Upstream's 1/(128 * samples_per_symbol) normalises int8 samples; here
         * the samples are floats near unit magnitude, so only the symbol length
         * divides out. Scale is irrelevant downstream — the slicer compares
         * against zero and the error filter is bang-bang — but keeping the
         * output near unity makes the intermediate values readable. */
        gain_k_ = 1.0f / static_cast<float>(2 * half_symbol_);

        clock_recovery_.configure(static_cast<float>(detector_rate_),
                                  static_cast<float>(symbol_rate),
                                  dsp::FixedErrorFilter{1.0f / 18.0f});
        clock_recovery_.set_symbol_handler(
            [this](const float symbol) { this->consume_symbol(symbol); });

        scm_builder_.configure(dsp::BitPattern{scm_sync, scm_sync_length, 1},
                               dsp::NeverMatch{});
        scm_builder_.set_end_matcher(dsp::FixedLength{scm_payload_chips});
        scm_builder_.set_payload_handler([this](const dsp::Packet& p) {
            count_scm_++;
            emit(PacketType::SCM, p);
        });

        scmplus_builder_.configure(dsp::BitPattern{scmplus_sync, scmplus_sync_length, 1},
                                   dsp::NeverMatch{});
        scmplus_builder_.set_end_matcher(dsp::FixedLength{scmplus_payload_chips});
        scmplus_builder_.set_payload_handler([this](const dsp::Packet& p) {
            count_scmplus_++;
            emit(PacketType::SCMPLUS, p);
        });

        idm_builder_.configure(dsp::BitPattern{idm_sync, idm_sync_length, 1},
                               dsp::NeverMatch{});
        idm_builder_.set_end_matcher(dsp::FixedLength{idm_payload_chips});
        idm_builder_.set_payload_handler([this](const dsp::Packet& p) {
            count_idm_++;
            emit(PacketType::IDM, p);
        });

        configured_ = true;
        reset();
    }

    bool configured() const { return configured_; }

    void reset() {
        clock_recovery_.reset();
        scm_builder_.reset();
        scmplus_builder_.reset();
        idm_builder_.reset();

        half_sum_ = 0.0f;
        half_count_ = 0;
        sum_half_period_[0] = sum_half_period_[1] = 0.0f;
        sum_period_[0] = sum_period_[1] = sum_period_[2] = 0.0f;
        manchester_[0] = manchester_[1] = manchester_[2] = 0.0f;

        acc_i_ = acc_q_ = 0.0;
        acc_count_ = 0;
        stride_count_ = 0;
        dc_i_ = dc_q_ = 0.0f;
    }

    void process(const dsp::cfloat* in, const size_t count) {
        if (!configured_ || in == nullptr) return;

        for (size_t n = 0; n < count; n++) {
            const float re = in[n].real();
            const float im = in[n].imag();

            /* One sample in every dc_stride joins the average — see the note
             * above the class on why this must stay slow. */
            if (++stride_count_ >= dc_stride) {
                stride_count_ = 0;
                acc_i_ += static_cast<double>(re);
                acc_q_ += static_cast<double>(im);
                if (++acc_count_ == average_window) {
                    dc_i_ = static_cast<float>(acc_i_ / static_cast<double>(average_window));
                    dc_q_ = static_cast<float>(acc_q_ / static_cast<double>(average_window));
                    acc_i_ = acc_q_ = 0.0;
                    acc_count_ = 0;
                }
            }

            const float r = re - dc_i_;
            const float i = im - dc_q_;
            half_sum_ += std::sqrt(r * r + i * i);

            if (++half_count_ < half_symbol_) continue;
            half_count_ = 0;

            sum_half_period_[1] = sum_half_period_[0];
            sum_half_period_[0] = half_sum_;
            half_sum_ = 0.0f;

            sum_period_[2] = sum_period_[1];
            sum_period_[1] = sum_period_[0];
            sum_period_[0] = (sum_half_period_[0] + sum_half_period_[1]) * gain_k_;

            manchester_[2] = manchester_[1];
            manchester_[1] = manchester_[0];
            manchester_[0] = sum_period_[2] - sum_period_[0];

            clock_recovery_(manchester_[0] - manchester_[2]);
        }
    }

    void process(const std::vector<dsp::cfloat>& in) { process(in.data(), in.size()); }

    double input_rate() const { return input_rate_; }
    double detector_rate() const { return detector_rate_; }
    size_t half_symbol_samples() const { return half_symbol_; }

    size_t packets_scm() const { return count_scm_; }
    size_t packets_scmplus() const { return count_scmplus_; }
    size_t packets_idm() const { return count_idm_; }

   private:
    using Builder = dsp::PacketBuilder<dsp::BitPattern, dsp::NeverMatch, dsp::FixedLength>;

    /* 2048 samples averaged, one taken every 2048 — upstream's one-per-buffer
     * accumulation with 2048-sample buffers, i.e. a one-second window at its
     * 4.194304 MHz baseband rate. */
    static constexpr size_t average_window = 2048;
    static constexpr size_t dc_stride = 2048;

    void consume_symbol(const float raw_symbol) {
        const uint_fast8_t sliced = (raw_symbol >= 0.0f) ? uint_fast8_t{1} : uint_fast8_t{0};
        scm_builder_.execute(sliced);
        scmplus_builder_.execute(sliced);
        idm_builder_.execute(sliced);
    }

    void emit(const PacketType type, const dsp::Packet& packet) {
        if (handler_) handler_(type, packet.bits());
    }

    PacketHandler handler_{};

    dsp::ClockRecovery<dsp::FixedErrorFilter> clock_recovery_{};
    Builder scm_builder_{};
    Builder scmplus_builder_{};
    Builder idm_builder_{};

    double input_rate_{0.0};
    double detector_rate_{0.0};
    size_t half_symbol_{1};
    float gain_k_{1.0f};

    float half_sum_{0.0f};
    size_t half_count_{0};
    float sum_half_period_[2]{0.0f, 0.0f};
    float sum_period_[3]{0.0f, 0.0f, 0.0f};
    float manchester_[3]{0.0f, 0.0f, 0.0f};

    double acc_i_{0.0};
    double acc_q_{0.0};
    size_t acc_count_{0};
    size_t stride_count_{0};
    float dc_i_{0.0f};
    float dc_q_{0.0f};

    size_t count_scm_{0};
    size_t count_scmplus_{0};
    size_t count_idm_{0};
    bool configured_{false};
};

/* --- Recent-entries table (ERTRecentEntry) --------------------------------- */

struct Key {
    ID id{invalid_id};
    CommodityType commodity_type{invalid_commodity_type};

    bool operator==(const Key& other) const {
        return (id == other.id) && (commodity_type == other.commodity_type);
    }
};

struct RecentEntry {
    using Key = ert::Key;

    static const Key invalid_key;

    ID id{invalid_id};
    CommodityType commodity_type{invalid_commodity_type};
    Consumption last_consumption{};
    TamperFlags last_tamper_flags{};
    PacketType packet_type{PacketType::Unknown};

    size_t received_count{0};

    explicit RecentEntry(const Key& key) : id{key.id}, commodity_type{key.commodity_type} {}

    Key key() const { return {id, commodity_type}; }

    void update(const Packet& packet) {
        received_count++;
        last_consumption = packet.consumption();
        last_tamper_flags = packet.tamper_flags();
        packet_type = packet.type();
    }
};

inline const Key RecentEntry::invalid_key{};

using RecentEntries = ui::RecentEntries<RecentEntry>;

/* --- Column formatting (ert::format) --------------------------------------- */

inline std::string format_id(const ID value) { return to_string_dec_uint(value, 10); }
inline std::string format_consumption(const Consumption value) {
    return to_string_dec_uint(value, 8);
}
inline std::string format_commodity_type(const CommodityType value) {
    return to_string_dec_uint(value, 2);
}
/* Bits above 16 of an IDM's tamper field do not fit the column, as upstream
 * notes. */
inline std::string format_tamper_flags(const TamperFlags value) {
    return to_string_hex(value & 0xFFFFu, 4);
}
inline std::string format_tamper_flags_scm(const TamperFlags value) {
    return " " + to_string_hex(value & 0x0Fu, 1) + "/" + to_string_hex(value >> 4, 1);
}

}  // namespace ert

/* --- View ------------------------------------------------------------------ */

class ErtView : public ui::View {
   public:
    ErtView();

    ErtView(const ErtView&) = delete;
    ErtView& operator=(const ErtView&) = delete;

    std::string title() const override { return "ERT Meter"; }

    void on_show() override;
    void on_frame_sync() override;
    void set_parent_rect(const ui::Rect new_parent_rect) override;

   private:
    static constexpr ui::Dim header_height = 32;

    void on_frame(ert::PacketType type, const std::vector<uint8_t>& chips);
    void update_status();

    ert::ErtDecoder decoder_{};
    ert::RecentEntries recent_{};

    std::vector<dsp::cfloat> samples_{};
    uint32_t frame_counter_{0};
    size_t candidates_{0};
    size_t decoded_{0};

    ui::FrequencyField field_frequency_{{0, 0}};
    ui::FrequencyStepView step_view_{{11 * 8, 0}, field_frequency_};
    ui::NumberField field_gain_{{17 * 8, 0}, 3, {0, 76}, 1, ' '};
    ui::Text text_status_{{21 * 8, 0, 9 * 8, 16}, ""};
    ui::Text text_note_{{0, 16, 240, 16}, ""};

    ui::RecentEntriesColumns columns_{{
        {"ID", 0},
        {"Ty", 2},
        {"Consumpt", 8},
        {"Tamp", 4},
        {"Ct", 2},
    }};

    ui::RecentEntriesView<ert::RecentEntries> recent_view_{columns_, recent_};
};

}  // namespace app

#endif /*__MB200_UI_ERT_H__*/
