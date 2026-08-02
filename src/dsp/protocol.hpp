/*
 * mayhem-b200 — protocol-layer DSP primitives.
 *
 * These are the pieces every packet decoder is assembled from, ported from the
 * PortaPack firmware:
 *
 *   baseband/clock_recovery.hpp   -> LinearResampler, GardnerTimingErrorDetector,
 *                                    LinearErrorFilter, FixedErrorFilter, ClockRecovery
 *   baseband/phase_detector.hpp   -> PhaseDetectorEarlyLateGate
 *   baseband/phase_accumulator.hpp-> PhaseAccumulator
 *   baseband/matched_filter.hpp   -> MatchedFilter
 *   baseband/packet_builder.hpp   -> Packet, NeverMatch, FixedLength, PacketBuilder
 *   baseband/symbol_coding.hpp    -> NrziDecoder, AcarsDecoder
 *   common/bit_pattern.hpp        -> BitHistory, BitPattern (+ BitCorrelator)
 *   common/manchester.*           -> manchester/bi-phase-mark encode and decode
 *   common/crc.hpp                -> Crc<Width, RevIn, RevOut>
 *   common/field_reader.hpp       -> Packet::read / BitStreamReader
 *
 * Two deliberate departures from upstream, both marked at the point of use:
 *
 *   1. ClockRecovery::configure() upstream reads `error_filter = error_filter;`
 *      — a self-assignment of the parameter, so the caller's error filter is
 *      silently discarded and the default-constructed member is used instead.
 *      Fixed here; a port that reproduced the bug would be untunable.
 *   2. BitCorrelator refuses to declare a match until at least `code_length`
 *      bits have been shifted in. Upstream's BitHistory starts at zero, so a
 *      sync word with a run of leading zeros can match on the very first bits
 *      of a stream that has not actually carried a preamble yet.
 *
 * Bit convention throughout: a "bit" or "symbol" is a uint8_t holding 0 or 1,
 * one per element. Packed bytes appear only where a function says so.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_PROTOCOL_H__
#define __MB200_PROTOCOL_H__

#include <bit>
#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

namespace dsp {

/* Same spelling as fir.hpp's; a repeated identical typedef is legal, so this
 * header stays usable on its own without dragging in the filter designers. */
using cfloat = std::complex<float>;

/* ===========================================================================
 * CRC
 *
 * Bit-at-a-time engine, a direct port of common/crc.hpp (itself a stripped
 * boost::crc_basic). Parameterised the same way the Rocksoft model is:
 *
 *   Width       register width in bits, 1..32
 *   RevIn       reflect each input chunk (refin)
 *   RevOut      reflect the final register (refout)
 *   polynomial  truncated polynomial, MSB-first form
 *   initial     initial register value (init)
 *   final_xor   value XORed into the result (xorout)
 * ===========================================================================*/

template <size_t Width, bool RevIn = false, bool RevOut = false>
class Crc {
    static_assert(Width >= 1 && Width <= 32, "Crc supports widths 1..32");

   public:
    using value_type = uint32_t;

    constexpr Crc(const value_type truncated_polynomial,
                  const value_type initial_remainder = 0,
                  const value_type final_xor_value = 0)
        : polynomial_{truncated_polynomial},
          initial_{initial_remainder},
          final_xor_{final_xor_value},
          remainder_{initial_remainder} {}

    constexpr value_type initial_remainder() const { return initial_; }
    constexpr value_type polynomial() const { return polynomial_; }
    constexpr value_type final_xor() const { return final_xor_; }

    void reset() { remainder_ = initial_; }
    void reset(const value_type new_initial_remainder) { remainder_ = new_initial_remainder; }

    void process_bit(const bool bit) {
        remainder_ ^= (bit ? top_bit() : 0U);
        const bool do_poly_div = (remainder_ & top_bit()) != 0U;
        remainder_ <<= 1;
        if (do_poly_div) remainder_ ^= polynomial_;
    }

    /* `bit_count` bits of `bits`, MSB-first unless RevIn. */
    void process_bits(const value_type bits, const size_t bit_count) {
        if (bit_count == 0) return;
        if constexpr (RevIn) {
            process_bits_lsb_first(bits, bit_count);
        } else {
            process_bits_msb_first(bits, bit_count);
        }
    }

    void process_byte(const uint8_t byte) { process_bits(byte, 8); }

    void process_bytes(const void* const data, const size_t length) {
        const uint8_t* const p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < length; i++) process_byte(p[i]);
    }

    /* Any contiguous container of bytes (std::vector, std::array, span...). */
    template <typename Container>
    void process_bytes(const Container& c) {
        process_bytes(c.data(), c.size());
    }

    value_type checksum() const {
        value_type r = remainder_;
        if constexpr (RevOut) r = reflect(r);
        return (r ^ final_xor_) & mask();
    }

    /* The live register, before reflection and the final XOR. Decoders that
     * check for a fixed residue over data+CRC want this. */
    value_type remainder() const { return remainder_ & mask(); }

    static constexpr size_t width() { return Width; }

   private:
    value_type polynomial_;
    value_type initial_;
    value_type final_xor_;
    value_type remainder_;

    static constexpr value_type top_bit() {
        return static_cast<value_type>(value_type{1} << (Width - 1));
    }

    static constexpr value_type mask() {
        if constexpr (Width >= 32) {
            return ~value_type{0};
        } else {
            return static_cast<value_type>((value_type{1} << Width) - value_type{1});
        }
    }

    static value_type reflect(value_type x) {
        value_type reflection = 0;
        for (size_t i = 0; i < Width; ++i) {
            reflection = static_cast<value_type>(reflection << 1);
            reflection |= (x & 1U);
            x >>= 1;
        }
        return reflection;
    }

    void process_bits_msb_first(value_type bits, size_t bit_count) {
        constexpr size_t digits = static_cast<size_t>(std::numeric_limits<value_type>::digits);
        constexpr value_type msb = value_type{1} << (digits - 1);

        if (bit_count > digits) bit_count = digits;
        bits = static_cast<value_type>(bits << (digits - bit_count));
        for (size_t i = bit_count; i > 0; --i, bits = static_cast<value_type>(bits << 1)) {
            process_bit((bits & msb) != 0U);
        }
    }

    void process_bits_lsb_first(value_type bits, size_t bit_count) {
        for (size_t i = bit_count; i > 0; --i, bits >>= 1) {
            process_bit((bits & 1U) != 0U);
        }
    }
};

/* Named parameter sets. The check values quoted are the CRC of the nine ASCII
 * bytes "123456789", the standard Rocksoft check vector. */

using Crc8 = Crc<8>;                  /* poly 0x07,       init 0x00,       check 0xF4       */
using Crc16Ccitt = Crc<16>;           /* poly 0x1021,     init 0xFFFF,     check 0x29B1     */
using Crc16Ibm = Crc<16, true, true>; /* poly 0x8005,     init 0x0000,     check 0xBB3D     */
using Crc32 = Crc<32, true, true>;    /* poly 0x04C11DB7, init 0xFFFFFFFF, check 0xCBF43926 */

inline Crc8 make_crc8() { return Crc8{0x07, 0x00, 0x00}; }
inline Crc16Ccitt make_crc16_ccitt() { return Crc16Ccitt{0x1021, 0xFFFF, 0x0000}; }
inline Crc16Ibm make_crc16_ibm() { return Crc16Ibm{0x8005, 0x0000, 0x0000}; }
inline Crc32 make_crc32() { return Crc32{0x04C11DB7, 0xFFFFFFFF, 0xFFFFFFFF}; }

uint8_t crc8(const void* data, size_t length);
uint16_t crc16_ccitt(const void* data, size_t length);
uint16_t crc16_ibm(const void* data, size_t length);
uint32_t crc32(const void* data, size_t length);

/* ===========================================================================
 * Bit pattern correlation (preamble / sync-word detection)
 * ===========================================================================*/

/* A 64-bit shift register of the most recent bits, newest in the LSB. */
class BitHistory {
   public:
    void add(const uint_fast8_t bit) {
        history_ = (history_ << 1) | (static_cast<uint64_t>(bit) & 1ULL);
    }

    uint64_t value() const { return history_; }
    void reset() { history_ = 0; }

   private:
    uint64_t history_{0};
};

/* Matches the newest `code_length` bits of a BitHistory against `code`,
 * tolerating up to `max_hamming_distance` differing bits. */
class BitPattern {
   public:
    constexpr BitPattern()
        : code_{0}, mask_{0}, length_{0}, max_hamming_distance_{0} {}

    constexpr BitPattern(const uint64_t code,
                         const size_t code_length,
                         const size_t max_hamming_distance = 0)
        : code_{code},
          mask_{length_mask(code_length)},
          length_{code_length},
          max_hamming_distance_{max_hamming_distance} {}

    /* Upstream call shape: the second argument (symbols received so far) is
     * unused, and exists so PacketBuilder can treat every matcher alike. */
    bool operator()(const BitHistory& history, const size_t) const {
        return matches(history.value());
    }

    bool matches(const uint64_t history) const {
        return distance(history) <= max_hamming_distance_;
    }

    size_t distance(const uint64_t history) const {
        const uint64_t delta = (history ^ code_) & mask_;
        return static_cast<size_t>(std::popcount(delta));
    }

    constexpr uint64_t code() const { return code_; }
    constexpr uint64_t mask() const { return mask_; }
    constexpr size_t length() const { return length_; }
    constexpr size_t max_hamming_distance() const { return max_hamming_distance_; }

   private:
    static constexpr uint64_t length_mask(const size_t code_length) {
        return (code_length >= 64) ? ~uint64_t{0}
                                   : static_cast<uint64_t>((uint64_t{1} << code_length) - 1ULL);
    }

    uint64_t code_;
    uint64_t mask_;
    size_t length_;
    size_t max_hamming_distance_;
};

/* Streaming correlator: feed bits one at a time, ask where the sync landed.
 *
 * Unlike a bare BitPattern this refuses to fire until `code_length` bits have
 * been shifted in, so the zero-initialised history cannot masquerade as a
 * preamble of leading zeros. */
class BitCorrelator {
   public:
    BitCorrelator() = default;
    BitCorrelator(uint64_t code, size_t code_length, size_t max_hamming_distance = 0);

    void configure(uint64_t code, size_t code_length, size_t max_hamming_distance = 0);
    void reset();

    /* Returns true if the bit just fed completed a match. */
    bool feed(uint_fast8_t bit);

    bool matched() const { return matched_; }
    size_t bits_fed() const { return bits_fed_; }

    /* Bit indices are 0-based positions within the stream fed since the last
     * reset(). Only meaningful once matched() has been true. */
    size_t match_end_bit() const { return match_end_bit_; }
    size_t match_start_bit() const { return match_start_bit_; }

    /* Hamming distance of the most recent evaluation. */
    size_t distance() const { return distance_; }

    uint64_t history() const { return history_.value(); }
    const BitPattern& pattern() const { return pattern_; }

   private:
    BitPattern pattern_{};
    BitHistory history_{};
    size_t bits_fed_{0};
    size_t match_end_bit_{0};
    size_t match_start_bit_{0};
    size_t distance_{64};
    bool matched_{false};
};

/* ===========================================================================
 * Symbol coding (baseband/symbol_coding.hpp)
 * ===========================================================================*/

/* NRZI: a data 1 is "no change", a data 0 is "change". */
class NrziDecoder {
   public:
    uint_fast8_t operator()(const uint_fast8_t symbol) {
        const uint_fast8_t out = static_cast<uint_fast8_t>((~(symbol ^ last_)) & 1);
        last_ = symbol;
        return out;
    }
    void reset() { last_ = 0; }

   private:
    uint_fast8_t last_{0};
};

/* The inverse of NrziDecoder — needed by every NRZI transmitter. */
class NrziEncoder {
   public:
    uint_fast8_t operator()(const uint_fast8_t data_bit) {
        last_ = static_cast<uint_fast8_t>((data_bit & 1) ? last_ : (~last_ & 1));
        return last_;
    }
    void reset() { last_ = 0; }

   private:
    uint_fast8_t last_{0};
};

class AcarsDecoder {
   public:
    uint_fast8_t operator()(const uint_fast8_t symbol) {
        last_ = static_cast<uint_fast8_t>(last_ ^ (~symbol & 1));
        return last_;
    }
    void reset() { last_ = 0; }

   private:
    uint_fast8_t last_{0};
};

/* ===========================================================================
 * Manchester and differential Manchester (bi-phase mark)
 * ===========================================================================*/

struct DecodedSymbol {
    uint_fast8_t value;
    uint_fast8_t error;
};

/* Manchester chip pairs.
 *   sense 0: data 1 -> chips {1, 0}, data 0 -> chips {0, 1}
 *   sense 1: the complement.
 * Matches common/manchester.cpp, where the encoder's `part` is 0xFF for
 * sense 0 and the decoder reads chip[sense]. */
std::vector<uint8_t> manchester_encode(const std::vector<uint8_t>& bits, size_t sense = 0);

std::vector<uint8_t> manchester_decode(const uint8_t* chips,
                                       size_t chip_count,
                                       size_t sense = 0,
                                       std::vector<uint8_t>* errors = nullptr);
std::vector<uint8_t> manchester_decode(const std::vector<uint8_t>& chips,
                                       size_t sense = 0,
                                       std::vector<uint8_t>* errors = nullptr);

/* Upstream's byte-oriented encoder: `src` is packed MSB-first, `dest` receives
 * two bytes per input bit, each 0x00 or 0xFF. dest must hold 2*bit_count. */
void manchester_encode_bytes(uint8_t* dest, const uint8_t* src, size_t bit_count, size_t sense = 0);

/* Differential Manchester / bi-phase mark (FM1): a transition at every symbol
 * boundary, plus a mid-symbol transition for a 1. */
std::vector<uint8_t> biphase_m_encode(const std::vector<uint8_t>& bits, uint8_t initial_level = 0);
std::vector<uint8_t> biphase_m_decode(const uint8_t* chips,
                                      size_t chip_count,
                                      std::vector<uint8_t>* errors = nullptr);
std::vector<uint8_t> biphase_m_decode(const std::vector<uint8_t>& chips,
                                      std::vector<uint8_t>* errors = nullptr);

/* Single-symbol accessors, byte-for-byte upstream's ManchesterDecoder and
 * BiphaseMDecoder operator[]. An index past the end yields {0, error}. */
DecodedSymbol manchester_symbol(const uint8_t* chips, size_t chip_count, size_t index, size_t sense = 0);
DecodedSymbol biphase_m_symbol(const uint8_t* chips, size_t chip_count, size_t index);

/* ===========================================================================
 * BitStream reader / writer
 *
 * MsbFirst  the most significant bit of a value is written first, and bits
 *           fill each byte from 0x80 down. This is the on-air / network order
 *           and matches upstream's FieldReader with BitRemapNone.
 * LsbFirst  the least significant bit is written first, and bits fill each
 *           byte from 0x01 up. This is the UART / serial order and matches
 *           FieldReader with BitRemapByteReverse.
 * ===========================================================================*/

enum class BitOrder : uint8_t { MsbFirst = 0, LsbFirst = 1 };

class BitStreamWriter {
   public:
    explicit BitStreamWriter(BitOrder order = BitOrder::MsbFirst) : order_{order} {}

    void write_bit(bool bit);
    /* Writes the low `bit_count` bits of `value`; bit_count 0..64. */
    void write(uint64_t value, size_t bit_count);
    void write_bytes(const void* data, size_t length);

    /* Pads with `fill` until the stream is a whole number of bytes. */
    void align_to_byte(bool fill = false);

    size_t bit_count() const { return bit_count_; }
    /* Packed bytes; the final byte is zero-padded if bit_count() is not a
     * multiple of 8. */
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    BitOrder order() const { return order_; }

    void clear();

   private:
    std::vector<uint8_t> bytes_{};
    size_t bit_count_{0};
    BitOrder order_{BitOrder::MsbFirst};
};

class BitStreamReader {
   public:
    BitStreamReader(const uint8_t* data, size_t byte_count, BitOrder order = BitOrder::MsbFirst);
    BitStreamReader(const std::vector<uint8_t>& bytes, BitOrder order = BitOrder::MsbFirst);

    bool read_bit();
    uint64_t read(size_t bit_count);
    uint64_t peek(size_t bit_count) const;

    void seek_bits(size_t bit_position);
    void skip(size_t bit_count);

    size_t bit_position() const { return bit_position_; }
    size_t bit_size() const { return bit_size_; }
    size_t bits_remaining() const {
        return (bit_position_ < bit_size_) ? (bit_size_ - bit_position_) : 0;
    }

    /* True once a read has asked for bits past the end. Missing bits read as
     * zero; the flag is how a parser notices a truncated packet. */
    bool overrun() const { return overrun_; }
    void clear_overrun() { overrun_ = false; }

    BitOrder order() const { return order_; }

   private:
    bool bit_at(size_t index) const;

    const uint8_t* data_{nullptr};
    size_t bit_size_{0};
    size_t bit_position_{0};
    BitOrder order_{BitOrder::MsbFirst};
    mutable bool overrun_{false};
};

/* ===========================================================================
 * Packet and PacketBuilder (baseband/packet_builder.hpp)
 * ===========================================================================*/

class Packet {
   public:
    using Timestamp = std::chrono::system_clock::time_point;

    /* Upstream's baseband::Packet is a fixed std::bitset<2560>; the default
     * capacity here is the same so ported apps see the same truncation point. */
    static constexpr size_t default_capacity = 2560;

    Packet() { bits_.reserve(64); }

    void add(const bool symbol) {
        if (bits_.size() < capacity_) bits_.push_back(symbol ? uint8_t{1} : uint8_t{0});
    }

    uint_fast8_t operator[](const size_t index) const {
        return (index < bits_.size()) ? bits_[index] : uint_fast8_t{0};
    }

    size_t size() const { return bits_.size(); }
    size_t capacity() const { return capacity_; }
    void set_capacity(const size_t bits) {
        capacity_ = bits;
        if (bits_.size() > capacity_) bits_.resize(capacity_);
    }
    void clear() { bits_.clear(); }
    bool empty() const { return bits_.empty(); }

    const std::vector<uint8_t>& bits() const { return bits_; }

    void set_timestamp(const Timestamp& value) { timestamp_ = value; }
    Timestamp timestamp() const { return timestamp_; }

    /* MSB-first field read: `start_bit` becomes the MSB of the result.
     * Equivalent to upstream FieldReader<Packet, BitRemapNone>::read. */
    uint32_t read(size_t start_bit, size_t length) const;

    /* As read(), but with each byte's bits taken in reverse order — upstream
     * FieldReader<Packet, BitRemapByteReverse>. */
    uint32_t read_byte_reversed(size_t start_bit, size_t length) const;

    /* Packs the bits MSB-first into bytes; a partial final byte is padded with
     * zeros. */
    std::vector<uint8_t> to_bytes() const;

   private:
    std::vector<uint8_t> bits_{};
    size_t capacity_{default_capacity};
    Timestamp timestamp_{};
};

/* Matchers, upstream shape: operator()(history, symbols_received) -> bool. */

struct NeverMatch {
    bool operator()(const BitHistory&, const size_t) const { return false; }
};

struct AlwaysMatch {
    bool operator()(const BitHistory&, const size_t) const { return true; }
};

struct FixedLength {
    size_t length{0};
    bool operator()(const BitHistory&, const size_t symbols_received) const {
        return symbols_received >= length;
    }
};

/* Accumulates payload bits after a preamble match, optionally dropping stuffed
 * bits, and hands a Packet to the payload handler when the end matcher fires. */
template <typename PreambleMatcher, typename UnstuffMatcher, typename EndMatcher>
class PacketBuilder {
   public:
    using PayloadHandlerFunc = std::function<void(const Packet&)>;

    PacketBuilder() = default;

    PacketBuilder(PreambleMatcher preamble_matcher,
                  UnstuffMatcher unstuff_matcher,
                  EndMatcher end_matcher,
                  PayloadHandlerFunc payload_handler)
        : payload_handler_{std::move(payload_handler)},
          preamble_{std::move(preamble_matcher)},
          unstuff_{std::move(unstuff_matcher)},
          end_{std::move(end_matcher)} {}

    void configure(PreambleMatcher preamble_matcher, UnstuffMatcher unstuff_matcher) {
        preamble_ = std::move(preamble_matcher);
        unstuff_ = std::move(unstuff_matcher);
        reset_state();
    }

    void set_end_matcher(EndMatcher end_matcher) {
        end_ = std::move(end_matcher);
        reset_state();
    }

    void set_payload_handler(PayloadHandlerFunc handler) {
        payload_handler_ = std::move(handler);
    }

    void set_capacity(const size_t bits) { packet_.set_capacity(bits); }

    void execute(const uint_fast8_t symbol) {
        bit_history_.add(symbol);

        switch (state_) {
            case State::Preamble:
                if (preamble_(bit_history_, packet_.size())) state_ = State::Payload;
                break;

            case State::Payload:
                if (!unstuff_(bit_history_, packet_.size())) packet_.add(symbol != 0);

                if (end_(bit_history_, packet_.size())) {
                    packet_.set_timestamp(std::chrono::system_clock::now());
                    if (payload_handler_) payload_handler_(packet_);
                    packets_emitted_++;
                    reset_state();
                } else if (packet_.size() >= packet_.capacity()) {
                    packets_truncated_++;
                    reset_state();
                }
                break;
        }
    }

    void reset() {
        bit_history_.reset();
        reset_state();
    }

    bool in_payload() const { return state_ == State::Payload; }
    size_t packets_emitted() const { return packets_emitted_; }
    size_t packets_truncated() const { return packets_truncated_; }

   private:
    enum class State : uint8_t { Preamble, Payload };

    void reset_state() {
        packet_.clear();
        state_ = State::Preamble;
    }

    PayloadHandlerFunc payload_handler_{};
    BitHistory bit_history_{};
    PreambleMatcher preamble_{};
    UnstuffMatcher unstuff_{};
    EndMatcher end_{};

    State state_{State::Preamble};
    Packet packet_{};
    size_t packets_emitted_{0};
    size_t packets_truncated_{0};
};

/* Sync word, no bit stuffing, fixed payload length — by far the most common
 * decoder shape. */
using FixedLengthPacketBuilder = PacketBuilder<BitPattern, NeverMatch, FixedLength>;

/* HDLC/AIS shape: sync word, five-ones bit stuffing, closing flag. */
using FlaggedPacketBuilder = PacketBuilder<BitPattern, BitPattern, BitPattern>;

/* ===========================================================================
 * Symbol timing recovery
 * ===========================================================================*/

/* Linear interpolating resampler, upstream baseband/linear_resampler.hpp.
 * `advance()` nudges the sampling phase; that is how the timing loop closes. */
class LinearResampler {
   public:
    void configure(const float input_rate, const float output_rate) {
        phase_increment_ = (output_rate > 0.0f) ? (input_rate / output_rate) : 1.0f;
    }

    void reset() {
        last_sample_ = 0.0f;
        phase_ = 0.0f;
    }

    template <typename InterpolatedSampleHandler>
    void operator()(const float sample, InterpolatedSampleHandler handler) {
        const float sample_delta = sample - last_sample_;
        while (phase_ < 1.0f) {
            handler(last_sample_ + phase_ * sample_delta);
            phase_ += phase_increment_;
        }
        last_sample_ = sample;
        phase_ -= 1.0f;
    }

    void advance(const float fraction) { phase_ += fraction * phase_increment_; }

    float phase_increment() const { return phase_increment_; }

   private:
    float last_sample_{0.0f};
    float phase_{0.0f};
    float phase_increment_{0.0f};
};

/* Gardner timing error detector. Expects two samples per symbol; reports the
 * on-time sample plus a "lateness" whose sign is what the loop should use —
 * the magnitude scales with the input level. */
class GardnerTimingErrorDetector {
   public:
    static constexpr size_t samples_per_symbol = 2;

    template <typename SymbolHandler>
    void operator()(const float in, SymbolHandler symbol_handler) {
        t_[2] = t_[1];
        t_[1] = t_[0];
        t_[0] = in;

        if (symbol_phase_ == 0) {
            const float symbol = t_[0];
            const float lateness = (t_[0] - t_[2]) * t_[1];
            symbol_handler(symbol, lateness);
        }

        symbol_phase_ = (symbol_phase_ + 1) % samples_per_symbol;
    }

    void reset() {
        t_[0] = t_[1] = t_[2] = 0.0f;
        symbol_phase_ = 0;
    }

   private:
    float t_[3]{0.0f, 0.0f, 0.0f};
    size_t symbol_phase_{0};
};

/* Complex Gardner: same detector for a complex baseband, as PSK needs.
 * error = Re{ conj(mid) * (late - early) }. */
class ComplexGardnerTimingErrorDetector {
   public:
    static constexpr size_t samples_per_symbol = 2;

    template <typename SymbolHandler>
    void operator()(const cfloat in, SymbolHandler symbol_handler) {
        t_[2] = t_[1];
        t_[1] = t_[0];
        t_[0] = in;

        if (symbol_phase_ == 0) {
            const cfloat diff = t_[0] - t_[2];
            const float lateness = diff.real() * t_[1].real() + diff.imag() * t_[1].imag();
            symbol_handler(t_[0], lateness);
        }

        symbol_phase_ = (symbol_phase_ + 1) % samples_per_symbol;
    }

    void reset() {
        t_[0] = t_[1] = t_[2] = cfloat{0.0f, 0.0f};
        symbol_phase_ = 0;
    }

   private:
    cfloat t_[3]{};
    size_t symbol_phase_{0};
};

/* One-pole smoothed proportional error filter. */
class LinearErrorFilter {
   public:
    LinearErrorFilter() = default;
    explicit LinearErrorFilter(const float filter_alpha, const float error_weight = -1.0f)
        : filter_alpha_{filter_alpha}, error_weight_{error_weight} {}

    float operator()(const float error) {
        error_filtered_ = filter_alpha_ * error_filtered_ + (1.0f - filter_alpha_) * error;
        return error_filtered_ * error_weight_;
    }

    void reset() { error_filtered_ = 0.0f; }

   private:
    float filter_alpha_{0.95f};
    float error_weight_{-1.0f};
    float error_filtered_{0.0f};
};

/* Bang-bang error filter: a fixed step against the sign of the lateness.
 * Verbatim upstream, including its behaviour at zero — see
 * DeadbandErrorFilter below for why that matters. */
class FixedErrorFilter {
   public:
    FixedErrorFilter() = default;
    explicit FixedErrorFilter(const float weight) : weight_{weight} {}

    float operator()(const float lateness) const {
        return (lateness < 0.0f) ? weight_ : -weight_;
    }

    float weight() const { return weight_; }
    void reset() {}

   private:
    float weight_{1.0f / 16.0f};
};

/* Bang-bang error filter that abstains on weak evidence, and the one the host
 * demodulators use.
 *
 * A Gardner detector's error is (current - previous) * midpoint, so a symbol
 * that repeats its predecessor carries no timing information: the error is
 * zero, or — once a real channel filter has rung a little — a small residue
 * whose sign happens to be consistent. FixedErrorFilter cannot tell the
 * difference, because it discards the magnitude and votes a full step either
 * way (and votes -weight even on an exact zero).
 *
 * That is not a hypothetical. Measured on a 2FSK burst at ten samples per
 * symbol: symbols at a real transition report a mean |lateness| of 0.26, while
 * symbols inside a run report a consistently-signed 0.014. Upstream's filter
 * treats the two identically, so a nine-bit run drags the sampling instant
 * nearly a third of a symbol in one direction on no evidence at all. A couple
 * of those and the loop slips a symbol and the packet is lost. AIS never trips
 * over it because its training sequence alternates every symbol.
 *
 * So: abstain unless this symbol's |lateness| is at least `deadband` times the
 * mean |lateness| the signal has been providing. The threshold has to be
 * relative, because the Gardner error scales with the square of the signal
 * amplitude and no fixed number can be right for both a normalised
 * discriminator output and a raw matched-filter output. The mean starts at
 * zero, so a filter that has just been reset abstains on nothing and the loop
 * acquires at full speed. */
class DeadbandErrorFilter {
   public:
    DeadbandErrorFilter() = default;
    explicit DeadbandErrorFilter(const float weight,
                                 const float deadband = 0.25f,
                                 const float tracking_rate = 1.0f / 64.0f)
        : weight_{weight}, deadband_{deadband}, tracking_rate_{tracking_rate} {}

    float operator()(const float lateness) {
        const float magnitude = (lateness < 0.0f) ? -lateness : lateness;
        mean_magnitude_ += tracking_rate_ * (magnitude - mean_magnitude_);

        if (!(magnitude > deadband_ * mean_magnitude_)) return 0.0f;
        return (lateness < 0.0f) ? weight_ : -weight_;
    }

    float weight() const { return weight_; }
    float deadband() const { return deadband_; }
    float mean_magnitude() const { return mean_magnitude_; }
    void reset() { mean_magnitude_ = 0.0f; }

   private:
    float weight_{1.0f / 16.0f};
    float deadband_{0.25f};
    float tracking_rate_{1.0f / 64.0f};
    float mean_magnitude_{0.0f};
};

/* Resample to 2x symbol rate, run Gardner, feed the timing error back into the
 * resampler phase, hand each recovered symbol to the handler. */
template <typename ErrorFilter>
class ClockRecovery {
   public:
    using SymbolHandler = std::function<void(float)>;

    ClockRecovery() = default;
    explicit ClockRecovery(SymbolHandler symbol_handler)
        : symbol_handler_{std::move(symbol_handler)} {}

    ClockRecovery(const float sampling_rate,
                  const float symbol_rate,
                  ErrorFilter error_filter,
                  SymbolHandler symbol_handler)
        : symbol_handler_{std::move(symbol_handler)} {
        configure(sampling_rate, symbol_rate, std::move(error_filter));
    }

    void configure(const float sampling_rate, const float symbol_rate, ErrorFilter error_filter) {
        resampler_.configure(sampling_rate,
                             symbol_rate * static_cast<float>(
                                               GardnerTimingErrorDetector::samples_per_symbol));
        /* Upstream writes `error_filter = error_filter;` here, which assigns
         * the parameter to itself and leaves the member default-constructed.
         * See the file header. */
        error_filter_ = std::move(error_filter);
    }

    void set_symbol_handler(SymbolHandler handler) { symbol_handler_ = std::move(handler); }

    void reset() {
        resampler_.reset();
        timing_error_detector_.reset();
        error_filter_.reset();
    }

    void operator()(const float baseband_sample) {
        resampler_(baseband_sample, [this](const float interpolated) {
            this->resampler_callback(interpolated);
        });
    }

   private:
    void resampler_callback(const float interpolated_sample) {
        timing_error_detector_(interpolated_sample,
                               [this](const float symbol, const float lateness) {
                                   this->symbol_callback(symbol, lateness);
                               });
    }

    void symbol_callback(const float symbol, const float lateness) {
        if (symbol_handler_) symbol_handler_(symbol);
        resampler_.advance(error_filter_(lateness));
    }

    LinearResampler resampler_{};
    GardnerTimingErrorDetector timing_error_detector_{};
    ErrorFilter error_filter_{};
    SymbolHandler symbol_handler_{};
};

/* Complex twin of LinearResampler, for the PSK timing loop. */
class ComplexLinearResampler {
   public:
    void configure(const float input_rate, const float output_rate) {
        phase_increment_ = (output_rate > 0.0f) ? (input_rate / output_rate) : 1.0f;
    }

    void reset() {
        last_sample_ = cfloat{0.0f, 0.0f};
        phase_ = 0.0f;
    }

    template <typename InterpolatedSampleHandler>
    void operator()(const cfloat sample, InterpolatedSampleHandler handler) {
        const cfloat sample_delta = sample - last_sample_;
        while (phase_ < 1.0f) {
            handler(last_sample_ + phase_ * sample_delta);
            phase_ += phase_increment_;
        }
        last_sample_ = sample;
        phase_ -= 1.0f;
    }

    void advance(const float fraction) { phase_ += fraction * phase_increment_; }

    float phase_increment() const { return phase_increment_; }

   private:
    cfloat last_sample_{0.0f, 0.0f};
    float phase_{0.0f};
    float phase_increment_{0.0f};
};

/* Complex twin of ClockRecovery. */
template <typename ErrorFilter>
class ComplexClockRecovery {
   public:
    using SymbolHandler = std::function<void(cfloat)>;

    ComplexClockRecovery() = default;

    void configure(const float sampling_rate, const float symbol_rate, ErrorFilter error_filter) {
        resampler_.configure(
            sampling_rate,
            symbol_rate *
                static_cast<float>(ComplexGardnerTimingErrorDetector::samples_per_symbol));
        error_filter_ = std::move(error_filter);
    }

    void set_symbol_handler(SymbolHandler handler) { symbol_handler_ = std::move(handler); }

    void reset() {
        resampler_.reset();
        timing_error_detector_.reset();
        error_filter_.reset();
    }

    void operator()(const cfloat baseband_sample) {
        resampler_(baseband_sample,
                   [this](const cfloat interpolated) { this->resampler_callback(interpolated); });
    }

   private:
    void resampler_callback(const cfloat interpolated_sample) {
        timing_error_detector_(interpolated_sample,
                               [this](const cfloat symbol, const float lateness) {
                                   if (symbol_handler_) symbol_handler_(symbol);
                                   resampler_.advance(error_filter_(lateness));
                               });
    }

    ComplexLinearResampler resampler_{};
    ComplexGardnerTimingErrorDetector timing_error_detector_{};
    ErrorFilter error_filter_{};
    SymbolHandler symbol_handler_{};
};

/* Free-running symbol clock: a 32-bit phase accumulator that reports its
 * wrap. Upstream baseband/phase_accumulator.hpp. */
class PhaseAccumulator {
   public:
    PhaseAccumulator() = default;
    constexpr explicit PhaseAccumulator(const uint32_t phase_inc) : phase_inc_{phase_inc} {}

    bool operator()() {
        const uint32_t last_phase = phase_;
        phase_ += phase_inc_;
        return phase_ < last_phase;
    }

    void set_inc(const uint32_t new_phase_inc) { phase_inc_ = new_phase_inc; }
    uint32_t inc() const { return phase_inc_; }
    void reset() { phase_ = 0; }

   private:
    uint32_t phase_{0};
    uint32_t phase_inc_{0};
};

/* Early-late gate over a history of sliced (hard) samples, upstream
 * baseband/phase_detector.hpp. Fed the last `samples_per_symbol` slicer
 * outputs, it returns the majority symbol and a lateness. */
class PhaseDetectorEarlyLateGate {
   public:
    using history_t = uint32_t;

    struct result_t {
        bool symbol;
        int error;
    };

    PhaseDetectorEarlyLateGate() = default;
    constexpr explicit PhaseDetectorEarlyLateGate(const size_t samples_per_symbol)
        : sample_threshold_{samples_per_symbol / 2},
          late_mask_{mask_of(samples_per_symbol / 2)},
          early_mask_{static_cast<history_t>(mask_of(samples_per_symbol / 2)
                                             << (samples_per_symbol / 2))} {}

    result_t operator()(const history_t symbol_history) const {
        /* history = ...0111 -> early;  history = ...1110 -> late. */
        const size_t late_side = static_cast<size_t>(std::popcount(symbol_history & late_mask_));
        const size_t early_side = static_cast<size_t>(std::popcount(symbol_history & early_mask_));
        const size_t total_count = late_side + early_side;
        const int lateness = static_cast<int>(late_side) - static_cast<int>(early_side);
        const bool symbol = (total_count >= sample_threshold_);
        return {symbol, symbol ? -lateness : lateness};
    }

   private:
    static constexpr history_t mask_of(const size_t bits) {
        return (bits >= 32) ? ~history_t{0}
                            : static_cast<history_t>((history_t{1} << bits) - history_t{1});
    }

    size_t sample_threshold_{1};
    history_t late_mask_{1};
    history_t early_mask_{2};
};

/* ===========================================================================
 * Matched filter (baseband/matched_filter.hpp)
 *
 * The taps are expected to be a lowpass multiplied by a complex sinusoid, so
 * the filter simultaneously shifts the wanted tone to DC. The output is
 * |correlation with taps| - |correlation with conjugated taps|, which for a
 * two-tone FSK signal is a discriminator: positive for one tone, negative for
 * the other.
 * ===========================================================================*/

class MatchedFilter {
   public:
    using sample_t = cfloat;
    using tap_t = cfloat;

    MatchedFilter() = default;
    MatchedFilter(const tap_t* taps, size_t taps_count, size_t decimation_factor = 1) {
        configure(taps, taps_count, decimation_factor);
    }

    template <typename Container>
    explicit MatchedFilter(const Container& taps, size_t decimation_factor = 1) {
        configure(taps.data(), taps.size(), decimation_factor);
    }

    void configure(const tap_t* taps, size_t taps_count, size_t decimation_factor);

    template <typename Container>
    void configure(const Container& taps, size_t decimation_factor) {
        configure(taps.data(), taps.size(), decimation_factor);
    }

    void reset();

    /* Returns true when a new output is available (once per decimation
     * cycle); read it with get_output(). */
    bool execute_once(sample_t input);

    float get_output() const { return output_; }
    size_t taps_count() const { return taps_count_; }
    size_t decimation_factor() const { return decimation_factor_; }

   private:
    void shift_by_decimation_factor();
    void advance_decimation_phase() {
        decimation_phase_ = (decimation_phase_ + 1) % decimation_factor_;
    }
    bool is_new_decimation_cycle() const { return decimation_phase_ == 0; }

    std::vector<sample_t> samples_{};
    std::vector<tap_t> taps_reversed_{};
    size_t taps_count_{0};
    size_t decimation_factor_{1};
    size_t decimation_phase_{0};
    float output_{0.0f};
};

/* Taps for a matched filter that translates `tone_hz` to DC: a rectangular
 * window times a complex sinusoid,
 *
 *     taps[n] = exp(j * 2*pi * tone_hz * n / sample_rate_hz) / taps_count
 *
 * which is the generalisation of upstream's hand-computed
 * baseband::ais::square_taps_38k4_1t_p — calling this with
 * (2400, 38400, 4) reproduces those four taps exactly. */
std::vector<cfloat> design_matched_filter_taps(double tone_hz,
                                               double sample_rate_hz,
                                               size_t taps_count);

}  // namespace dsp

#endif /*__MB200_PROTOCOL_H__*/
