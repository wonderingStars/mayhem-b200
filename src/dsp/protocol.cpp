/*
 * mayhem-b200 — protocol-layer DSP primitives.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "protocol.hpp"

#include <algorithm>
#include <cmath>

namespace dsp {

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

/* --- CRC one-shots ---------------------------------------------------------- */

uint8_t crc8(const void* data, size_t length) {
    auto crc = make_crc8();
    crc.process_bytes(data, length);
    return static_cast<uint8_t>(crc.checksum());
}

uint16_t crc16_ccitt(const void* data, size_t length) {
    auto crc = make_crc16_ccitt();
    crc.process_bytes(data, length);
    return static_cast<uint16_t>(crc.checksum());
}

uint16_t crc16_ibm(const void* data, size_t length) {
    auto crc = make_crc16_ibm();
    crc.process_bytes(data, length);
    return static_cast<uint16_t>(crc.checksum());
}

uint32_t crc32(const void* data, size_t length) {
    auto crc = make_crc32();
    crc.process_bytes(data, length);
    return crc.checksum();
}

/* --- BitCorrelator ---------------------------------------------------------- */

BitCorrelator::BitCorrelator(uint64_t code, size_t code_length, size_t max_hamming_distance) {
    configure(code, code_length, max_hamming_distance);
}

void BitCorrelator::configure(uint64_t code, size_t code_length, size_t max_hamming_distance) {
    pattern_ = BitPattern{code, code_length, max_hamming_distance};
    reset();
}

void BitCorrelator::reset() {
    history_.reset();
    bits_fed_ = 0;
    match_end_bit_ = 0;
    match_start_bit_ = 0;
    distance_ = 64;
    matched_ = false;
}

bool BitCorrelator::feed(uint_fast8_t bit) {
    history_.add(bit);
    bits_fed_++;
    matched_ = false;

    const size_t length = pattern_.length();
    if (length == 0) {
        distance_ = 64;
        return false;
    }

    distance_ = pattern_.distance(history_.value());

    /* Refuse to fire before the shift register holds real data — see the
     * departure note in the header. */
    if (bits_fed_ < length) return false;

    if (distance_ <= pattern_.max_hamming_distance()) {
        matched_ = true;
        match_end_bit_ = bits_fed_ - 1;
        match_start_bit_ = match_end_bit_ + 1 - length;
        return true;
    }
    return false;
}

/* --- Manchester ------------------------------------------------------------- */

DecodedSymbol manchester_symbol(const uint8_t* chips,
                                size_t chip_count,
                                size_t index,
                                size_t sense) {
    const size_t encoded_index = index * 2;
    if ((encoded_index + 1) < chip_count) {
        const uint_fast8_t value = static_cast<uint_fast8_t>(chips[encoded_index + (sense & 1)] & 1);
        const uint_fast8_t error =
            static_cast<uint_fast8_t>((chips[encoded_index + 0] & 1) == (chips[encoded_index + 1] & 1));
        return {value, error};
    }
    return {0, 1};
}

DecodedSymbol biphase_m_symbol(const uint8_t* chips, size_t chip_count, size_t index) {
    const size_t encoded_index = index * 2;
    if ((encoded_index + 1) < chip_count) {
        const uint_fast8_t value =
            static_cast<uint_fast8_t>((chips[encoded_index + 0] & 1) != (chips[encoded_index + 1] & 1));
        /* No transition across the symbol boundary means the phase was lost. */
        const uint_fast8_t error =
            encoded_index ? static_cast<uint_fast8_t>((chips[encoded_index - 1] & 1) ==
                                                      (chips[encoded_index + 0] & 1))
                          : uint_fast8_t{0};
        return {value, error};
    }
    return {0, 1};
}

std::vector<uint8_t> manchester_encode(const std::vector<uint8_t>& bits, size_t sense) {
    /* sense 0: first chip carries the data bit. sense 1: second chip does. */
    const uint8_t first_for_one = (sense & 1) ? uint8_t{0} : uint8_t{1};

    std::vector<uint8_t> chips;
    chips.reserve(bits.size() * 2);
    for (const uint8_t b : bits) {
        const uint8_t first = (b & 1) ? first_for_one : static_cast<uint8_t>(first_for_one ^ 1);
        chips.push_back(first);
        chips.push_back(static_cast<uint8_t>(first ^ 1));
    }
    return chips;
}

std::vector<uint8_t> manchester_decode(const uint8_t* chips,
                                       size_t chip_count,
                                       size_t sense,
                                       std::vector<uint8_t>* errors) {
    const size_t symbols = chip_count / 2;

    std::vector<uint8_t> bits;
    bits.reserve(symbols);
    if (errors) {
        errors->clear();
        errors->reserve(symbols);
    }

    for (size_t i = 0; i < symbols; i++) {
        const DecodedSymbol s = manchester_symbol(chips, chip_count, i, sense);
        bits.push_back(static_cast<uint8_t>(s.value & 1));
        if (errors) errors->push_back(static_cast<uint8_t>(s.error & 1));
    }
    return bits;
}

std::vector<uint8_t> manchester_decode(const std::vector<uint8_t>& chips,
                                       size_t sense,
                                       std::vector<uint8_t>* errors) {
    return manchester_decode(chips.data(), chips.size(), sense, errors);
}

void manchester_encode_bytes(uint8_t* dest, const uint8_t* src, size_t bit_count, size_t sense) {
    /* Verbatim port of common/manchester.cpp manchester_encode(). */
    const uint8_t part = (sense & 1) ? uint8_t{0x00} : uint8_t{0xFF};

    for (size_t c = 0; c < bit_count; c++) {
        if ((static_cast<unsigned>(src[c >> 3]) << (c & 7)) & 0x80u) {
            *(dest++) = part;
            *(dest++) = static_cast<uint8_t>(~part);
        } else {
            *(dest++) = static_cast<uint8_t>(~part);
            *(dest++) = part;
        }
    }
}

std::vector<uint8_t> biphase_m_encode(const std::vector<uint8_t>& bits, uint8_t initial_level) {
    uint8_t level = static_cast<uint8_t>(initial_level & 1);

    std::vector<uint8_t> chips;
    chips.reserve(bits.size() * 2);
    for (const uint8_t b : bits) {
        level ^= 1;  /* always a transition at the symbol boundary */
        chips.push_back(level);
        if (b & 1) level ^= 1;  /* a 1 adds a mid-symbol transition */
        chips.push_back(level);
    }
    return chips;
}

std::vector<uint8_t> biphase_m_decode(const uint8_t* chips,
                                      size_t chip_count,
                                      std::vector<uint8_t>* errors) {
    const size_t symbols = chip_count / 2;

    std::vector<uint8_t> bits;
    bits.reserve(symbols);
    if (errors) {
        errors->clear();
        errors->reserve(symbols);
    }

    for (size_t i = 0; i < symbols; i++) {
        const DecodedSymbol s = biphase_m_symbol(chips, chip_count, i);
        bits.push_back(static_cast<uint8_t>(s.value & 1));
        if (errors) errors->push_back(static_cast<uint8_t>(s.error & 1));
    }
    return bits;
}

std::vector<uint8_t> biphase_m_decode(const std::vector<uint8_t>& chips,
                                      std::vector<uint8_t>* errors) {
    return biphase_m_decode(chips.data(), chips.size(), errors);
}

/* --- BitStreamWriter -------------------------------------------------------- */

void BitStreamWriter::write_bit(bool bit) {
    const size_t index = bit_count_;
    if ((index % 8) == 0) bytes_.push_back(0);

    if (bit) {
        const size_t byte_index = index / 8;
        const unsigned shift = static_cast<unsigned>(index % 8);
        const uint8_t mask = (order_ == BitOrder::MsbFirst)
                                 ? static_cast<uint8_t>(0x80u >> shift)
                                 : static_cast<uint8_t>(1u << shift);
        bytes_[byte_index] = static_cast<uint8_t>(bytes_[byte_index] | mask);
    }
    bit_count_++;
}

void BitStreamWriter::write(uint64_t value, size_t bit_count) {
    if (bit_count == 0) return;
    if (bit_count > 64) bit_count = 64;

    if (order_ == BitOrder::MsbFirst) {
        for (size_t i = bit_count; i > 0; i--) {
            write_bit(((value >> (i - 1)) & 1ULL) != 0ULL);
        }
    } else {
        for (size_t i = 0; i < bit_count; i++) {
            write_bit(((value >> i) & 1ULL) != 0ULL);
        }
    }
}

void BitStreamWriter::write_bytes(const void* data, size_t length) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; i++) write(p[i], 8);
}

void BitStreamWriter::align_to_byte(bool fill) {
    while ((bit_count_ % 8) != 0) write_bit(fill);
}

void BitStreamWriter::clear() {
    bytes_.clear();
    bit_count_ = 0;
}

/* --- BitStreamReader -------------------------------------------------------- */

BitStreamReader::BitStreamReader(const uint8_t* data, size_t byte_count, BitOrder order)
    : data_{data}, bit_size_{byte_count * 8}, order_{order} {}

BitStreamReader::BitStreamReader(const std::vector<uint8_t>& bytes, BitOrder order)
    : data_{bytes.data()}, bit_size_{bytes.size() * 8}, order_{order} {}

bool BitStreamReader::bit_at(size_t index) const {
    if (data_ == nullptr || index >= bit_size_) {
        overrun_ = true;
        return false;
    }
    const uint8_t byte = data_[index / 8];
    const unsigned shift = static_cast<unsigned>(index % 8);
    if (order_ == BitOrder::MsbFirst) {
        return ((byte >> (7u - shift)) & 1u) != 0u;
    }
    return ((byte >> shift) & 1u) != 0u;
}

bool BitStreamReader::read_bit() {
    const bool b = bit_at(bit_position_);
    bit_position_++;
    return b;
}

uint64_t BitStreamReader::peek(size_t bit_count) const {
    if (bit_count == 0) return 0;
    if (bit_count > 64) bit_count = 64;

    uint64_t value = 0;
    if (order_ == BitOrder::MsbFirst) {
        for (size_t i = 0; i < bit_count; i++) {
            value = (value << 1) | (bit_at(bit_position_ + i) ? 1ULL : 0ULL);
        }
    } else {
        for (size_t i = 0; i < bit_count; i++) {
            if (bit_at(bit_position_ + i)) value |= (1ULL << i);
        }
    }
    return value;
}

uint64_t BitStreamReader::read(size_t bit_count) {
    if (bit_count > 64) bit_count = 64;
    const uint64_t value = peek(bit_count);
    bit_position_ += bit_count;
    return value;
}

void BitStreamReader::seek_bits(size_t bit_position) {
    bit_position_ = bit_position;
}

void BitStreamReader::skip(size_t bit_count) {
    bit_position_ += bit_count;
}

/* --- Packet ----------------------------------------------------------------- */

uint32_t Packet::read(size_t start_bit, size_t length) const {
    if (length > 32) length = 32;
    uint32_t value = 0;
    for (size_t i = start_bit; i < (start_bit + length); i++) {
        value = static_cast<uint32_t>(value << 1) | static_cast<uint32_t>((*this)[i] & 1);
    }
    return value;
}

uint32_t Packet::read_byte_reversed(size_t start_bit, size_t length) const {
    if (length > 32) length = 32;
    uint32_t value = 0;
    for (size_t i = start_bit; i < (start_bit + length); i++) {
        value = static_cast<uint32_t>(value << 1) | static_cast<uint32_t>((*this)[i ^ 7] & 1);
    }
    return value;
}

std::vector<uint8_t> Packet::to_bytes() const {
    std::vector<uint8_t> out((bits_.size() + 7) / 8, 0);
    for (size_t i = 0; i < bits_.size(); i++) {
        if (bits_[i] & 1) {
            out[i / 8] = static_cast<uint8_t>(out[i / 8] | (0x80u >> (i % 8)));
        }
    }
    return out;
}

/* --- MatchedFilter ---------------------------------------------------------- */

void MatchedFilter::configure(const tap_t* taps, size_t taps_count, size_t decimation_factor) {
    if (taps == nullptr || taps_count == 0) {
        samples_.clear();
        taps_reversed_.clear();
        taps_count_ = 0;
        decimation_factor_ = 1;
        decimation_phase_ = 0;
        output_ = 0.0f;
        return;
    }

    if (decimation_factor == 0) decimation_factor = 1;
    /* The delay line is only taps_count long, and execute_once() writes into
     * its last `decimation_factor` slots. */
    decimation_factor = std::min(decimation_factor, taps_count);

    taps_count_ = taps_count;
    decimation_factor_ = decimation_factor;
    decimation_phase_ = 0;
    output_ = 0.0f;

    samples_.assign(taps_count, sample_t{0.0f, 0.0f});
    taps_reversed_.resize(taps_count);
    std::reverse_copy(&taps[0], &taps[taps_count], taps_reversed_.begin());
}

void MatchedFilter::reset() {
    std::fill(samples_.begin(), samples_.end(), sample_t{0.0f, 0.0f});
    decimation_phase_ = 0;
    output_ = 0.0f;
}

bool MatchedFilter::execute_once(const sample_t input) {
    if (taps_count_ == 0) return false;

    samples_[taps_count_ - decimation_factor_ + decimation_phase_] = input;

    advance_decimation_phase();
    if (!is_new_decimation_cycle()) return false;

    float sr_tr = 0.0f;
    float si_tr = 0.0f;
    float si_ti = 0.0f;
    float sr_ti = 0.0f;
    for (size_t n = 0; n < taps_count_; n++) {
        const sample_t sample = samples_[n];
        const tap_t tap = taps_reversed_[n];

        sr_tr += sample.real() * tap.real();
        si_ti += sample.imag() * tap.imag();
        si_tr += sample.imag() * tap.real();
        sr_ti += sample.real() * tap.imag();
    }

    /* N: correlation against the conjugated taps. P: against the taps. */
    const float r_n = sr_tr + si_ti;
    const float r_p = sr_tr - si_ti;
    const float i_n = si_tr - sr_ti;
    const float i_p = si_tr + sr_ti;

    const float mag_n = std::sqrt(r_n * r_n + i_n * i_n);
    const float mag_p = std::sqrt(r_p * r_p + i_p * i_p);
    output_ = mag_p - mag_n;

    shift_by_decimation_factor();
    return true;
}

void MatchedFilter::shift_by_decimation_factor() {
    const size_t n = taps_count_ - decimation_factor_;
    if (n == 0) return;
    std::copy(samples_.begin() + static_cast<std::ptrdiff_t>(decimation_factor_),
              samples_.begin() + static_cast<std::ptrdiff_t>(decimation_factor_ + n),
              samples_.begin());
}

std::vector<cfloat> design_matched_filter_taps(double tone_hz,
                                               double sample_rate_hz,
                                               size_t taps_count) {
    if (taps_count == 0 || sample_rate_hz <= 0.0) return {};

    const double gain = 1.0 / static_cast<double>(taps_count);
    const double w = 2.0 * kPi * tone_hz / sample_rate_hz;

    std::vector<cfloat> taps(taps_count);
    for (size_t n = 0; n < taps_count; n++) {
        const double p = w * static_cast<double>(n);
        taps[n] = cfloat{static_cast<float>(gain * std::cos(p)),
                         static_cast<float>(gain * std::sin(p))};
    }
    return taps;
}

}  // namespace dsp
