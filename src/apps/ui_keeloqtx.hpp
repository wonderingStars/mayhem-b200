/*
 * mayhem-b200 — KeeLoq TX: rolling-code garage/gate remote transmitter.
 *
 * Ported from firmware/application/external/keeloqtx/ui_keeloqtx.* and the
 * KeeLoq cipher in firmware/application/keeloq_common.cpp. The cipher, the
 * per-manufacturer hop-code layout, the fix/hop → payload assembly and the OOK
 * PWM framing (33-symbol header, 3-symbol-per-bit "110"/"100" fragments, "1001"
 * stop) are reproduced exactly so the emitted waveform matches upstream.
 *
 * Upstream loads a remote from a .KEELOQ file and its manufacturer key from an
 * SD-card MFCODES keystore. The B200 has neither, so this host port lets the
 * user enter the 64-bit manufacturer key and the serial directly and pick the
 * hop layout; the encoder itself is byte-identical.
 *
 * LEGALITY: transmitting a rolling code impersonates someone's remote and is
 * illegal to radiate in most places. The app transmits only on an explicit
 * Start press and shows a warning; RF output needs a USRP B200 and is
 * unverified without one.
 *
 * Copyright (C) 2026 lifegame1lu111 (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_KEELOQTX_H__
#define __MB200_UI_KEELOQTX_H__

#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <atomic>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* --- KeeLoq cipher (firmware/application/keeloq_common.cpp, verbatim) -------
 * 528-round single-bit NLFSR, 32-bit block, 64-bit key. Verified against the
 * hadipourh/KeeLoq reference vectors in tests/test_keeloqtx.cpp. */
uint32_t keeloq_encrypt(uint32_t data, uint64_t key);
uint32_t keeloq_decrypt(uint32_t data, uint64_t key);

/* Derives the device key for "normal learning" from the fixed part and the
 * manufacturer key: two decryptions of the serial with the 0x2/0x6 seed bits. */
uint64_t keeloq_normal_learning(uint32_t data, uint64_t key);

/* Reverses the low `bit_count` bits — the display-only transform upstream calls
 * subghz_protocol_blocks_reverse_key. */
uint64_t keeloq_reverse_key(uint64_t key, uint8_t bit_count);

/* The per-manufacturer hop-code serial layouts from update_hop(). */
enum class KeeloqHop : uint8_t {
    Generic = 0,  /* serial & 0x3FF (10 bits) */
    Serial12,     /* serial & 0xFFF (DTM/FAAC/Came/Genius/GSN/Rossi/...) */
    Serial8,      /* serial & 0xFF  (NICE Smilo/MHOUSE, JCM Tech) */
    Aprimatic,    /* 10-bit serial with even-parity fill */
    Merlin,       /* fixed 0x000 */
    Centurion,    /* fixed 0x1CE */
    Monarch,      /* fixed 0x100 */
    DeaMio,       /* disc-number remap */
};

enum class KeeloqLearning : uint8_t { Simple = 0, Normal = 1 };

/* hop = (btn << 28) | (serial-derived 12 bits << 16) | counter. */
uint32_t keeloq_build_hop(KeeloqHop type, uint8_t btn, uint32_t serial, uint16_t counter);

/* The full OOK symbol string: 33-symbol header, then per payload bit (LSB
 * first) "110" for a 0 and "100" for a 1, then the "1001" stop. Each symbol is
 * one te (400 us on air). */
std::string keeloq_encode_fragments(uint64_t payload);

class KeeloqTXView : public ui::View {
   public:
    KeeloqTXView();
    ~KeeloqTXView() override;

    KeeloqTXView(const KeeloqTXView&) = delete;
    KeeloqTXView& operator=(const KeeloqTXView&) = delete;

    std::string title() const override { return "KeeLoqTX"; }
    void focus() override;
    void on_frame_sync() override;

   private:
    void rebuild();
    void start_tx();
    void stop_tx();

    KeeloqHop hop_type_{KeeloqHop::Generic};
    KeeloqLearning learning_{KeeloqLearning::Simple};
    uint64_t key_{0};
    uint32_t serial_{0};
    uint8_t button_{1};
    uint16_t counter_{0};
    uint32_t repeat_{4};

    uint32_t fix_{0};
    uint32_t hop_{0};
    uint64_t payload_{0};

    bool transmitting_{false};
    std::vector<std::complex<float>> tx_iq_{};
    std::atomic<size_t> tx_pos_{0};

    ui::Labels labels_{
        {{1 * 8, 1 * 16}, "Maker:", ui::Color::light_grey()},
        {{1 * 8, 2 * 16}, "Learn:", ui::Color::light_grey()},
        {{1 * 8, 3 * 16}, "Key:", ui::Color::light_grey()},
        {{1 * 8, 4 * 16}, "Serial:", ui::Color::light_grey()},
        {{1 * 8, 5 * 16}, "Button:", ui::Color::light_grey()},
        {{1 * 8, 6 * 16}, "Counter:", ui::Color::light_grey()},
        {{1 * 8, 7 * 16}, "Repeat:", ui::Color::light_grey()},
        {{1 * 8, 8 * 16}, "Freq:", ui::Color::light_grey()},
        {{1 * 8, 9 * 16}, "Payload:", ui::Color::light_grey()},
    };

    ui::OptionsField field_maker_{
        {8 * 8, 1 * 16},
        10,
        {{"Generic", 0},
         {"Serial12", 1},
         {"Serial8", 2},
         {"Aprimatic", 3},
         {"Merlin", 4},
         {"Centurion", 5},
         {"Monarch", 6},
         {"Dea Mio", 7}}};

    ui::OptionsField field_learn_{
        {8 * 8, 2 * 16},
        8,
        {{"Simple", 0}, {"Normal", 1}}};

    ui::SymField field_key_{{8 * 8, 3 * 16}, 16, ui::SymField::Type::Hex};
    ui::SymField field_serial_{{8 * 8, 4 * 16}, 7, ui::SymField::Type::Hex};
    ui::NumberField field_button_{{9 * 8, 5 * 16}, 2, {0, 15}, 1, ' '};
    ui::NumberField field_counter_{{9 * 8, 6 * 16}, 5, {0, 65535}, 1, ' '};
    ui::NumberField field_repeat_{{9 * 8, 7 * 16}, 3, {1, 100}, 1, ' '};

    ui::FrequencyField field_freq_{{8 * 8, 8 * 16}};
    ui::Text text_payload_{{9 * 8, 9 * 16, 16 * 8, 16}, "0000000000000000"};

    ui::Console console_{{0, 11 * 16, 240, 5 * 16}};

    ui::Button button_tx_{{2 * 8, 17 * 16, 26 * 8, 2 * 16}, "Start"};
};

}  // namespace app

#endif /*__MB200_UI_KEELOQTX_H__*/
