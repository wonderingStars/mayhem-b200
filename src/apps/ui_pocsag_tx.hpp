/*
 * mayhem-b200 — POCSAG pager transmitter.
 *
 * Host port of the PortaPack firmware app
 *   application/external/pocsag_tx/ui_pocsag_tx.*
 * and the codeword/BCH encoder it drives
 *   common/pocsag.cpp  (pocsag_encode, insert_BCH via BCH(31,21)+parity).
 *
 * The encoder itself is not re-implemented here: Phase A already vendored it
 * verbatim in pocsag_app.hpp (pocsag::pocsag_encode, pocsag::EccContainer,
 * pocsag::get_digit_code), so this app reuses those functions and only adds the
 * transmit-specific parts — CCIR Rec. 584 polarity inversion, packing the
 * 32-bit codewords MSB-first into a byte stream, and 2FSK modulation via the
 * Phase A dsp::FskKeyer at 512 / 1200 / 2400 bps, +/-4.5 kHz deviation.
 *
 * POCSAG on the air (see pocsag_app.hpp header for the full description):
 *   - 576-bit preamble of alternating 1010... (16 codewords of 0xAAAAAAAA).
 *   - Frame synchronisation codeword 0x7CD215D8, then 16 codewords (8 frames).
 *   - Address codeword: flag bit 0, RIC bits 21..3 at bits 30..13, a 2-bit
 *     function, BCH(31,21) parity in bits 10..1 and even parity in bit 0. The
 *     low 3 bits of the RIC pick which frame the address sits in.
 *   - Message codewords: flag bit 1, 20 payload bits carrying 7-bit ASCII sent
 *     least-significant-bit first (alphanumeric) or 4-bit BCD (numeric).
 *
 * The firmware's FSK processor (baseband/proc_fsk.cpp) maps a 1 bit to POSITIVE
 * deviation. Standard POCSAG (CCIR Rec. 584) needs a 1 bit at NEGATIVE
 * deviation, so "Standard" polarity bitwise-inverts every codeword before it
 * reaches the modulator; "Inverted" sends them as-is. dsp::FskKeyer uses the
 * same 1 -> +deviation convention, so the inversion rule is identical here.
 *
 * Nothing transmits until the user presses Start. A B200 radiating a pager page
 * on a real paging channel is illegal almost everywhere; the view shows a short
 * warning.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_POCSAG_TX_H__
#define __MB200_UI_POCSAG_TX_H__

#include "ui.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"
#include "ui_freq_field.hpp"

#include "pocsag_app.hpp"  /* pocsag::pocsag_encode, EccContainer, MessageType */

#include "../dsp/modulate.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace app::pocsag_tx {

/* ===========================================================================
 * Transmit-side helpers (tested in tests/test_pocsag_tx.cpp)
 *
 * These wrap the Phase A encoder and add the two things the TX app needs that
 * the shared encoder does not: optional polarity inversion and MSB-first byte
 * packing. They are the byte stream proc_fsk.cpp would have read from
 * shared_memory.bb_data on the firmware.
 * ===========================================================================*/

using pocsag::MessageType;

/* Preamble + sync + address/message batches for one page, as common/pocsag.cpp
 * builds them. Thin wrapper so the test can get the codewords directly. */
inline std::vector<uint32_t> encode_codewords(MessageType type,
                                              const pocsag::EccContainer& ecc,
                                              uint32_t function,
                                              const std::string& message,
                                              uint32_t address) {
    std::vector<uint32_t> codewords;
    pocsag::pocsag_encode(type, ecc, function, message, address, codewords);
    return codewords;
}

/* Pack 32-bit codewords big-endian (MSB first) into a byte buffer, optionally
 * bitwise-inverting each one for CCIR Rec. 584 ("Standard") polarity. The
 * modulator reads this buffer bit-by-bit, MSB of byte 0 first. */
inline std::vector<uint8_t> codewords_to_bytes(const std::vector<uint32_t>& codewords,
                                               bool invert) {
    std::vector<uint8_t> out;
    out.reserve(codewords.size() * 4);
    for (uint32_t c : codewords) {
        const uint32_t v = invert ? ~c : c;
        out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(v & 0xFF));
    }
    return out;
}

/* Legal RIC (capcode) range: 21 bits. */
constexpr uint32_t max_address = 0x1FFFFFu;

/* Firmware's POCSAG TX chain runs at 2.28 Msps with +/-4.5 kHz deviation. */
constexpr double sample_rate_hz = 2'280'000.0;
constexpr float deviation_hz = 4500.0f;

/* ===========================================================================
 * View
 * ===========================================================================*/

class POCSAGTXView : public ui::View {
   public:
    POCSAGTXView();
    ~POCSAGTXView() override;

    POCSAGTXView(const POCSAGTXView&) = delete;
    POCSAGTXView& operator=(const POCSAGTXView&) = delete;

    std::string title() const override { return "POCSAG TX"; }

    void focus() override;
    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void set_message();
    void update_message_text();
    bool start_tx();
    void stop_tx();

    /* Selected bitrate in bps (512 / 1200 / 2400). */
    uint32_t selected_bitrate() const;

    pocsag::EccContainer ecc_{};

    std::string message_{"PORTAPACK"};

    /* Transmit state. tx_bits_ outlives the DSP callback because stop() joins
     * the DSP thread before it is touched again. */
    dsp::FskKeyer fsk_{};
    std::vector<uint8_t> tx_bytes_{};
    bool transmitting_{false};
    std::atomic<bool> tx_done_{false};
    std::atomic<uint64_t> produced_samples_{0};
    uint64_t total_samples_{0};

    ui::Labels labels_{
        {{4, 24}, "Freq", ui::Color::light_grey()},
        {{4, 48}, "Rate", ui::Color::light_grey()},
        {{4, 72}, "RIC", ui::Color::light_grey()},
        {{4, 96}, "Type", ui::Color::light_grey()},
        {{4, 120}, "Func", ui::Color::light_grey()},
        {{4, 144}, "Pol", ui::Color::light_grey()},
        {{4, 168}, "Message", ui::Color::light_grey()},
    };

    ui::FrequencyField field_freq_{{56, 24}};

    ui::OptionsField options_bitrate_{
        {56, 48},
        8,
        {{"512 bps ", 512},
         {"1200 bps", 1200},
         {"2400 bps", 2400}}};

    /* 7-digit decimal RIC entry (max 2097151). */
    ui::SymField field_address_{{56, 72}, 7, ui::SymField::Type::Dec};

    ui::OptionsField options_type_{
        {136, 72},
        12,
        {{"Address only", pocsag::ADDRESS_ONLY},
         {"Numeric only", pocsag::NUMERIC_ONLY},
         {"Alphanumeric", pocsag::ALPHANUMERIC}}};

    ui::OptionsField options_function_{
        {56, 96},
        1,
        {{"A", 0}, {"B", 1}, {"C", 2}, {"D", 3}}};

    ui::OptionsField options_polarity_{
        {56, 120},
        8,
        {{"Standard", 0}, {"Inverted", 1}}};

    ui::Text text_message_{{4, 184, 232, 16}, ""};
    ui::Text text_message_l2_{{4, 200, 232, 16}, ""};

    ui::Button button_message_{{4, 220, 112, 28}, "Set message"};
    ui::Button button_tx_{{124, 220, 112, 28}, "Start TX"};

    ui::ProgressBar progressbar_{{4, 254, 232, 12}};

    ui::Text text_warning_{
        {0, 272, 240, 16},
        "TX of pager pages is illegal in most areas"};

    ui::Text text_status_{{0, 288, 240, 16}, ""};
};

}  // namespace app::pocsag_tx

#endif /*__MB200_UI_POCSAG_TX_H__*/
