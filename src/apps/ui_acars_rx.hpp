/*
 * mayhem-b200 — ACARS receiver (aircraft VHF character-oriented data link).
 *
 * Ported from the PortaPack firmware:
 *
 *   application/external/acars_rx/acars_app.{hpp,cpp}
 *       -> AcarsDecoded, acars_decode(), acars_format(), the console view
 *   baseband/proc_acars.{hpp,cpp}
 *       -> the block framing state machine (WSYN/SYN2/SOH1/TXT/CRC1/CRC2/END),
 *          the SYN/SOH/STX/ETX/ETB/DLE constants, the per-character parity
 *          budget, the DLE "hack the path" shortcut, the 240-byte length cap
 *   common/crc.hpp
 *       -> ParityCheck::parity_check (odd parity over the whole character)
 *
 * The framing, the constants, the field offsets and the CRC parameters are
 * upstream's, byte for byte. Three things are not, each marked at the point of
 * use and repeated here so they are impossible to miss:
 *
 *   1. CHARACTER BIT ORDER. Upstream assembles each character MSB-first
 *      (`decode_data = decode_data << 1 | bit`). Its own framing constants say
 *      otherwise: SOH 0x01, STX 0x02, SYN 0x16, ETX 0x83, ETB 0x97 and DLE 0x7F
 *      are the 7-bit ASCII codes with an ODD parity bit in bit 7 (0x03|0x80 =
 *      0x83, 0x17|0x80 = 0x97), and ARINC 618 sends each character least
 *      significant data bit first with that parity bit last. Assembling
 *      MSB-first puts the parity bit at bit 0 and reverses the data, so those
 *      constants can never match a real transmission. AcarsBitDecoder therefore
 *      defaults to LsbFirst and keeps MsbFirst selectable, so upstream's exact
 *      behaviour is still reachable (and is tested).
 *
 *   2. WHAT THE CRC IS COMPUTED OVER. Upstream's state machine stores the two
 *      CRC bytes in ACARSPacketMessage::crc and hands the app only
 *      message[0..msg_len), then acars_decode() treats the last two bytes of
 *      *that* string as the CRC — so it checksums the tail of the text against
 *      itself and crc_ok is meaningless. acars_decode() is ported verbatim; the
 *      block it is fed here is message||crc (AcarsBlock::raw()), which is
 *      exactly the ARINC 618 BCS coverage: everything after SOH through ETX,
 *      followed by the two-byte BCS.
 *
 *   3. THE DEMODULATOR. Upstream's proc_acars runs a 16-tap matched filter
 *      whose taps are exp(j*2*pi*4800*n/38400) — a discriminator between
 *      +4800 Hz and -4800 Hz. ACARS is MSK at 2400 bit/s amplitude-modulated on
 *      the VHF carrier, so its tones are 2400 Hz (a one) and 1200 Hz (a zero)
 *      around an 1800 Hz centre; nothing in the signal sits at +/-4800 Hz.
 *      Upstream's own header comment reads "Deviation: ???". The host AM-detects
 *      the channel and runs the Phase A AfskDemod on the real tone pair, which
 *      at a full-bit correlator delay is the classic one-bit differential
 *      detector for MSK. The bit-level output is identical in kind: one 0/1 per
 *      symbol, fed to the same state machine.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2018 Furrtek
 * Copyright (C) 2023 Bernd Herzog
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_ACARS_RX_H__
#define __MB200_UI_ACARS_RX_H__

#include "../dsp/demod.hpp"
#include "../dsp/demod_digital.hpp"
#include "../dsp/fir.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <bit>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace app {

/* ===========================================================================
 * Protocol constants — proc_acars.cpp
 * ===========================================================================*/

inline constexpr uint8_t kAcarsSyn = 0x16; /* SYN, odd parity already even -> 0x16 */
inline constexpr uint8_t kAcarsSoh = 0x01; /* SOH */
inline constexpr uint8_t kAcarsStx = 0x02; /* STX */
inline constexpr uint8_t kAcarsEtx = 0x83; /* ETX (0x03) + odd parity bit       */
inline constexpr uint8_t kAcarsEtb = 0x97; /* ETB (0x17) + odd parity bit       */
inline constexpr uint8_t kAcarsDle = 0x7F; /* DEL, the block terminator         */

/* Physical layer, ARINC 618: MSK at 2400 bit/s, a one is the 2400 Hz tone and a
 * zero the 1200 Hz tone, amplitude-modulated onto the VHF carrier. */
inline constexpr float kAcarsBaud = 2400.0f;
inline constexpr float kAcarsMarkHz = 2400.0f;
inline constexpr float kAcarsSpaceHz = 1200.0f;

/* The VHF ACARS channel upstream tunes by default (131.825 MHz, Europe). */
inline constexpr uint64_t kAcarsDefaultFrequency = 131'825'000ull;

/* Upstream caps the message buffer at 250 bytes and resets past 240. */
inline constexpr size_t kAcarsMessageCapacity = 250;
inline constexpr size_t kAcarsMessageResetLength = 240;
/* Upstream drops the block after more than four bad characters. */
inline constexpr uint8_t kAcarsMaxParityErrors = 4;

/* ===========================================================================
 * Per-character parity — common/crc.hpp, ParityCheck::parity_check
 *
 * Upstream indexes a 256-entry popcount table and tests bit 0 of the count, so
 * the check passes when the character has an ODD number of set bits — 7 data
 * bits plus an odd-parity bit in bit 7. std::popcount is the same function.
 * ===========================================================================*/

inline bool acars_parity_ok(uint8_t ch) {
    return (std::popcount(static_cast<unsigned>(ch)) & 1) != 0;
}

/* Sets bit 7 of a 7-bit character so the whole byte carries odd parity — the
 * transmit-side inverse, used to build test vectors and to show what upstream's
 * ETX/ETB constants are. */
inline uint8_t acars_add_parity(uint8_t seven_bit_char) {
    const uint8_t data = static_cast<uint8_t>(seven_bit_char & 0x7F);
    return acars_parity_ok(data) ? data : static_cast<uint8_t>(data | 0x80);
}

/* ===========================================================================
 * CRC — acars_app.cpp, acars_crc16()
 *
 * CRC-16/CCITT as ACARS uses it: poly 0x1021, init 0x0000, no reflection, no
 * final XOR. That is the CRC-16/XMODEM parameter set, whose Rocksoft check
 * value (the CRC of "123456789") is 0x31C3 — asserted in the tests.
 * ===========================================================================*/

inline uint16_t acars_crc16(const uint8_t* data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(static_cast<uint16_t>(data[i]) << 8);
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x8000)
                crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
            else
                crc = static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

inline uint16_t acars_crc16(const std::string& data, std::string::size_type len) {
    return acars_crc16(reinterpret_cast<const uint8_t*>(data.data()), len);
}

/* ===========================================================================
 * Frame parse — acars_app.cpp, verbatim
 *
 * ACARS frame field layout (0-based byte offsets), upstream's comment kept:
 *   [0]          SOH framing byte   (skipped)
 *   [1..7]       Aircraft registration (7 chars)
 *   [8]          STX framing byte   (skipped)
 *   [9..10]      Label              (2 chars)
 *   [11]         Block ID           (1 char)
 *   [12..14]     Message number     (3 chars)
 *   [15..20]     Flight ID          (6 chars)
 *   [21..N-3]    Free-text payload  (variable)
 *   [N-2..N-1]   CRC-16/CCITT       (2 bytes, MSB first, NOT part of payload)
 * ===========================================================================*/

inline constexpr std::string::size_type kAcarsCrcLen = 2;
inline constexpr std::string::size_type kAcarsHeaderLen = 21;
inline constexpr std::string::size_type kAcarsMinLen = kAcarsHeaderLen + kAcarsCrcLen;

struct AcarsDecoded {
    bool crc_ok{false};
    std::string reg{};
    std::string label{};
    std::string flight_id{};
    std::string msg_num{};
    char block_id{'\0'};
    std::string txt{};
};

inline AcarsDecoded acars_decode(const std::string& raw) {
    AcarsDecoded result;
    if (raw.size() < kAcarsMinLen) {
        result.txt = "ACARS message too short (" + std::to_string(raw.size()) +
                     " bytes, need " + std::to_string(kAcarsMinLen) + ")";
        return result;
    }

    /* Verify CRC: computed over everything except the last 2 bytes, then
     * compared against those 2 bytes (MSB first). */
    const std::string::size_type payload_len = raw.size() - kAcarsCrcLen;
    const uint16_t computed = acars_crc16(raw, payload_len);
    const uint16_t received =
        static_cast<uint16_t>((static_cast<uint16_t>(static_cast<uint8_t>(raw[raw.size() - 2])) << 8) |
                              static_cast<uint16_t>(static_cast<uint8_t>(raw[raw.size() - 1])));
    result.crc_ok = (computed == received);

    result.reg = raw.substr(1, 7);
    result.label = raw.substr(9, 2);
    result.block_id = raw[11];
    result.msg_num = raw.substr(12, 3);
    result.flight_id = raw.substr(15, 6);
    /* Payload sits between end of fixed header and the 2 trailing CRC bytes. */
    if (payload_len > kAcarsHeaderLen)
        result.txt = raw.substr(kAcarsHeaderLen, payload_len - kAcarsHeaderLen);
    return result;
}

inline std::string acars_format(const AcarsDecoded& msg) {
    return std::string("ACARS Decoded Result\nCRC: ") + (msg.crc_ok ? "OK" : "FAIL") +
           "\nRegistration: " + msg.reg +
           "\nLabel: " + msg.label +
           "\nBlockID: " + msg.block_id +
           "\nMsgNum: " + msg.msg_num +
           "\nFlightID: " + msg.flight_id +
           "\nMessage: " + msg.txt;
}

/* Same fields on one console line, which is what a 30-column screen can hold. */
inline std::string acars_format_line(const AcarsDecoded& msg) {
    std::string s = msg.crc_ok ? std::string{"CRC OK  "} : std::string{"CRC BAD "};
    s += msg.reg;
    s += " ";
    s += msg.label;
    s += " ";
    s += msg.block_id;
    s += " ";
    s += msg.flight_id;
    return s;
}

/* ===========================================================================
 * Block framing state machine — proc_acars.cpp, ACARSProcessor::consume_symbol
 * ===========================================================================*/

enum class AcarsState : uint8_t {
    WaitSyn = 0, /* upstream WSYN */
    Syn2 = 1,
    Soh1 = 2,
    Text = 3, /* upstream TXT  */
    Crc1 = 4,
    Crc2 = 5,
    End = 6,
};

inline const char* acars_state_name(AcarsState s) {
    switch (s) {
        case AcarsState::WaitSyn: return "WSYN";
        case AcarsState::Syn2: return "SYN2";
        case AcarsState::Soh1: return "SOH1";
        case AcarsState::Text: return "TXT";
        case AcarsState::Crc1: return "CRC1";
        case AcarsState::Crc2: return "CRC2";
        case AcarsState::End: return "END";
    }
    return "?";
}

struct AcarsBlock {
    /* Everything after SOH up to and including ETX/ETB — the bytes the ARINC
     * 618 block check sequence covers. */
    std::vector<uint8_t> message{};
    uint8_t crc_high{0}; /* upstream message.crc[0], the first CRC byte on air */
    uint8_t crc_low{0};  /* upstream message.crc[1]                            */
    uint8_t parity_errors{0};

    /* message||crc — the string acars_decode() expects. */
    std::string raw() const {
        std::string s{message.begin(), message.end()};
        s.push_back(static_cast<char>(crc_high));
        s.push_back(static_cast<char>(crc_low));
        return s;
    }
};

/* Consumes one recovered bit (or one soft symbol) at a time and emits blocks.
 *
 * This is a line-for-line port of ACARSProcessor::consume_symbol(), including
 * the sequential — not mutually exclusive — state tests, so a state entered
 * part-way through a call is re-examined in that same call exactly as upstream
 * does (that is what makes the DLE shortcut below work). */
class AcarsBitDecoder {
   public:
    /* Order in which the eight bits of a character arrive on the wire. See
     * departure 1 in the file header. */
    enum class BitOrder : uint8_t { LsbFirst = 0, MsbFirst = 1 };

    void set_bit_order(BitOrder order) {
        bit_order_ = order;
        reset();
    }
    BitOrder bit_order() const { return bit_order_; }

    /* upstream ACARSProcessor::reset() */
    void reset() {
        decode_data_ = 0;
        bit_count_ = 0;
        state_ = AcarsState::WaitSyn;
        message_.clear();
        crc_high_ = 0;
        crc_low_ = 0;
        parity_errors_ = 0;
    }

    /* upstream consume_symbol(): slice at zero, then feed the bit. */
    void feed_symbol(float raw_symbol) {
        feed_bit((raw_symbol >= 0.0f) ? uint8_t{1} : uint8_t{0});
    }

    void feed_bits(const std::vector<uint8_t>& bits) {
        for (const uint8_t b : bits) feed_bit(b);
    }

    void feed_bit(uint8_t bit);

    /* Fired for every completed block (upstream payload_handler, state 255). */
    std::function<void(const AcarsBlock&)> on_block{};
    /* Fired wherever upstream calls sendDebug(). The byte is the character just
     * assembled; upstream memsets its buffer before sending it, so the value it
     * reports is always zero — that is a bug, not a design, and is not copied. */
    std::function<void(AcarsState, uint8_t)> on_state{};

    AcarsState state() const { return state_; }
    size_t message_length() const { return message_.size(); }
    uint8_t parity_errors() const { return parity_errors_; }
    size_t blocks_emitted() const { return blocks_emitted_; }
    size_t bits_fed() const { return bits_fed_; }

   private:
    void add_bit(uint8_t bit) {
        if (bit_order_ == BitOrder::MsbFirst) {
            decode_data_ = ((decode_data_ << 1) | (bit & 1u)) & 0xFFu;
        } else {
            decode_data_ = ((decode_data_ >> 1) | ((bit & 1u) << 7)) & 0xFFu;
        }
        bit_count_++;
    }

    uint8_t byte_value() const { return static_cast<uint8_t>(decode_data_ & 0xFFu); }

    void clear_byte() {
        decode_data_ = 0;
        bit_count_ = 0;
    }

    void notify(uint8_t last_byte) {
        if (on_state) on_state(state_, last_byte);
    }

    void emit_block();

    BitOrder bit_order_{BitOrder::LsbFirst};
    AcarsState state_{AcarsState::WaitSyn};
    uint32_t decode_data_{0};
    uint8_t bit_count_{0};
    std::vector<uint8_t> message_{};
    uint8_t crc_high_{0};
    uint8_t crc_low_{0};
    uint8_t parity_errors_{0};
    size_t blocks_emitted_{0};
    size_t bits_fed_{0};
};

/* --- AcarsBitDecoder, out of line ------------------------------------------
 *
 * Line-for-line port of ACARSProcessor::consume_symbol(). The sequential `if`s
 * are upstream's and are deliberately not mutually exclusive: a state entered
 * inside this call is re-tested before the call returns, which is what makes
 * the DLE shortcut below work. Kept inline in the header so the decoder can be
 * tested without linking the view. */

inline void AcarsBitDecoder::emit_block() {
    AcarsBlock block;
    block.message = message_;
    block.crc_high = crc_high_;
    block.crc_low = crc_low_;
    block.parity_errors = parity_errors_;
    blocks_emitted_++;
    if (on_block) on_block(block);
}

inline void AcarsBitDecoder::feed_bit(uint8_t bit) {
    bits_fed_++;
    add_bit(bit);

    if (state_ == AcarsState::WaitSyn && bit_count_ == 8) {
        if (byte_value() == kAcarsSyn) {
            state_ = AcarsState::Syn2;
            clear_byte();
        } else {
            /* Upstream: "just drop the first bit". The register already holds
             * the newest eight bits, so decrementing the count turns the
             * fixed-width compare into a one-bit sliding window. */
            bit_count_ -= 1;
        }
        return;
    }

    if (state_ == AcarsState::Syn2 && bit_count_ == 8) {
        if (byte_value() == kAcarsSyn) {
            state_ = AcarsState::Soh1;
            clear_byte();
            notify(kAcarsSyn);
            return;
        }
        /* Wrong second SYN: upstream throws the whole thing away. */
        reset();
    }

    if (state_ == AcarsState::Soh1 && bit_count_ == 8) {
        const uint8_t ch = byte_value();
        if (ch == kAcarsSoh) {
            reset();
            state_ = AcarsState::Text;
            notify(kAcarsSoh);
            return;
        }
        reset();
        notify(ch);
    }

    if (state_ == AcarsState::Text && bit_count_ == 8) {
        const uint8_t ch = byte_value();

        if (message_.size() < kAcarsMessageCapacity) message_.push_back(ch);

        if (!acars_parity_ok(ch)) {
            parity_errors_++;
            if (parity_errors_ > kAcarsMaxParityErrors) {
                /* Too many bad characters — abandon the block. */
                reset();
                notify(ch);
                return;
            }
        }

        if (ch == kAcarsEtx || ch == kAcarsEtb) {
            state_ = AcarsState::Crc1;
            notify(ch);
            clear_byte();
            return;
        }

        if (message_.size() > kAcarsMessageResetLength) {
            reset();
            notify(ch);
        }

        if (message_.size() > 20 && ch == kAcarsDle) {
            /* A block that ends with DEL carries its CRC in the two characters
             * immediately before it. Upstream rewinds msg_len by three, lifts
             * those two bytes out, and then "hacks the path" by loading the
             * second CRC byte back into the bit register so the CRC2 test below
             * fires in this same call with the bit count still at eight. */
            const size_t n = message_.size() - 3;
            crc_high_ = message_[n];
            crc_low_ = message_[n + 1];
            message_.resize(n);
            state_ = AcarsState::Crc2;
            notify(ch);
            decode_data_ = crc_low_;
        } else {
            clear_byte();
            return;
        }
    }

    if (state_ == AcarsState::Crc1 && bit_count_ == 8) {
        crc_high_ = byte_value();
        state_ = AcarsState::Crc2;
        clear_byte();
        notify(crc_high_);
    }

    if (state_ == AcarsState::Crc2 && bit_count_ == 8) {
        crc_low_ = byte_value();
        emit_block();
        reset();
        state_ = AcarsState::End;
        clear_byte();
        notify(crc_low_);
    }

    if (state_ == AcarsState::End && bit_count_ == 8) {
        const uint8_t ch = byte_value();
        reset();
        notify(ch);
    }
}

/* ===========================================================================
 * Audio / baseband front end
 *
 * AM-detect the channel, run the MSK tone pair through AfskDemod, feed the
 * recovered bits to AcarsBitDecoder. Replaces proc_acars's
 * decimate -> matched filter -> ClockRecovery chain; see departure 3.
 * ===========================================================================*/

class AcarsAudioDecoder {
   public:
    /* `audio_rate_hz` is the rate of the AM-detected audio. An exact multiple of
     * 2400 is worth arranging: the correlator delay that best separates the two
     * MSK tones is one whole bit period, and an integer number of samples per
     * bit makes that delay exact. */
    void configure(float audio_rate_hz) {
        audio_rate_hz_ = audio_rate_hz;
        am_.configure(audio_rate_hz);
        demod_.configure(audio_rate_hz, kAcarsMarkHz, kAcarsSpaceHz, kAcarsBaud);
        reset();
    }

    void reset() {
        am_.reset();
        demod_.reset();
        decoder_.reset();
        bits_.clear();
        audio_.clear();
    }

    /* Already-detected audio (the output of an AM envelope detector). */
    void process_audio(const float* in, size_t count) {
        bits_.clear();
        demod_.process_audio(in, count, bits_);
        decoder_.feed_bits(bits_);
    }

    /* Complex baseband centred on the ACARS carrier. */
    void process_baseband(const dsp::cfloat* in, size_t count) {
        audio_.clear();
        am_.process(in, count, audio_);
        if (!audio_.empty()) process_audio(audio_.data(), audio_.size());
    }

    AcarsBitDecoder& decoder() { return decoder_; }
    const AcarsBitDecoder& decoder() const { return decoder_; }
    dsp::AfskDemod& demod() { return demod_; }
    float audio_rate_hz() const { return audio_rate_hz_; }
    size_t last_bit_count() const { return bits_.size(); }

   private:
    dsp::AmDemod am_{};
    dsp::AfskDemod demod_{};
    AcarsBitDecoder decoder_{};
    std::vector<uint8_t> bits_{};
    std::vector<float> audio_{};
    float audio_rate_hz_{24000.0f};
};

/* ===========================================================================
 * View
 * ===========================================================================*/

class AcarsRxView : public ui::View {
   public:
    AcarsRxView();
    ~AcarsRxView() override;

    AcarsRxView(const AcarsRxView&) = delete;
    AcarsRxView& operator=(const AcarsRxView&) = delete;

    std::string title() const override { return "ACARS"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void rebuild_chain();
    void pump_samples();
    void on_block(const AcarsBlock& block);
    void log_line(const std::string& line);
    void update_status();

    radio::ReceiverModel& receiver_;

    /* Channel path. The receiver only publishes a wideband, most-recent-block
     * snapshot (take_spectrum_samples), so the mix-and-decimate that a PortaPack
     * does on the M4 happens here. */
    dsp::Nco nco_{};
    dsp::FirDecimateC decim1_{};
    dsp::FirDecimateC decim2_{};
    std::vector<dsp::cfloat> raw_{};
    std::vector<dsp::cfloat> stage1_{};
    std::vector<dsp::cfloat> channel_{};

    AcarsAudioDecoder decoder_{};

    double configured_rate_{0.0};
    double configured_lo_{0.0};
    double channel_rate_{0.0};
    uint32_t frame_counter_{0};
    uint64_t samples_seen_{0};
    size_t blocks_{0};
    size_t crc_ok_{0};

    std::ofstream log_{};
    bool logging_{false};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{152, 2}, "Gain", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};
    ui::NumberField field_gain_{{188, 2}, 3, {0, 76}, 1, ' '};

    ui::Checkbox check_log_{{0, 20}, 3, "LOG", true};
    ui::Text text_counts_{{80, 22, 160, 16}, ""};

    ui::Text text_status_{{0, 42, 240, 16}, ""};
    ui::Text text_tap_{{0, 58, 240, 16}, ""};

    ui::Console console_{{0, 78, 240, 222}};
};

}  // namespace app

#endif /*__MB200_UI_ACARS_RX_H__*/
