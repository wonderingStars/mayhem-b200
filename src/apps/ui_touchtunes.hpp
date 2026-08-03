/*
 * mayhem-b200 — TouchTunes jukebox remote (OOK, 433.92 MHz).
 *
 * Ported from firmware/application/apps/ui_touchtunes.* . The remote's frame is
 * a NEC-like OOK burst at 1766 baud: a sync byte 0x5D, the PIN (least
 * significant bit first), then an 8-bit button code followed by its complement.
 * Each data bit becomes a short or long carrier gap — upstream emits the symbol
 * fragment "10" for a 0 bit and "1000" for a 1 bit, most significant bit first,
 * wraps the whole thing in a "111111111111111100000000" lead-in and a "1000"
 * terminating pulse, and keys the carrier on/off through the OOK modulator.
 *
 * The frame word and the fragment stream are the deliverable and are checked in
 * tests/test_touchtunes.cpp against the worked examples in the upstream header.
 *
 * FREQUENCY / LEGALITY: 433.92 MHz is a licence-free ISM band in much of the
 * world but not everywhere, and operating someone else's jukebox without
 * permission may be unlawful regardless of band. Transmit starts only on an
 * explicit TX press. Upstream's "EW mode" (a continuous jamming carrier) is a
 * jammer and is deliberately NOT ported.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2022 NotPike (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_TOUCHTUNES_H__
#define __MB200_UI_TOUCHTUNES_H__

#include "../dsp/ring_buffer.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace radio {
class TransmitterModel;
}

namespace app {
namespace touchtunes {

inline constexpr uint8_t kSyncWord = 0x5D;
inline constexpr size_t kButtonCount = 32;
inline constexpr float kBaud = 1766.0f;   /* 560 us per fragment */
inline constexpr uint32_t kRepeats = 4;

/* The 32 button codes, upstream order (each on air is the code then its
 * complement). Index 0 is Pause, 1 is On/Off, ... */
uint8_t button_code(size_t index);
const char* button_name(size_t index);

/* The 32-bit frame word: sync 0x5D in the top byte, then the PIN (bit-reversed,
 * i.e. inserted least significant bit first), then the button code. The final
 * code-complement byte is added when the fragments are built. */
uint32_t frame_word(uint32_t pin, size_t button_index);

/* The full OOK symbol ("fragment") stream: lead-in, 32 data bits expanded to
 * "10"/"1000", and the terminating pulse. Each character is one 1766-baud OOK
 * symbol (carrier on for '1', off for '0'). */
std::string build_fragments(uint32_t pin, size_t button_index);

/* --- View ----------------------------------------------------------------- */

class TouchTunesView : public ui::View {
   public:
    TouchTunesView();
    ~TouchTunesView() override;

    TouchTunesView(const TouchTunesView&) = delete;
    TouchTunesView& operator=(const TouchTunesView&) = delete;

    std::string title() const override { return "TouchTunes"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void start_tx();
    void stop_tx();

    radio::TransmitterModel* transmitter_{nullptr};
    bool transmitting_{false};
    std::vector<std::complex<float>> burst_{};

    static constexpr double kSampleRate = 2'000'000.0;

    dsp::RingBuffer<std::complex<float>> ring_{1u << 20};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{0, 24}, "PIN", ui::Color::light_grey()},
        {{0, 46}, "Button", ui::Color::light_grey()},
        {{0, 68}, "Gain", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};

    ui::NumberField field_pin_{{48, 24}, 3, {0, 255}, 1, '0'};

    ui::OptionsField options_button_{
        {56, 46},
        14,
        {{"Pause", 0}, {"On/Off", 1}, {"P1", 2}, {"P2", 3}, {"P3", 4},
         {"F1", 5}, {"Up", 6}, {"F2", 7}, {"Left", 8}, {"OK", 9},
         {"Right", 10}, {"F3", 11}, {"Down", 12}, {"F4", 13},
         {"1", 14}, {"2", 15}, {"3", 16}, {"4", 17}, {"5", 18}, {"6", 19},
         {"7", 20}, {"8", 21}, {"9", 22}, {"Music/Kar", 23}, {"0", 24},
         {"Lock/Que", 25}, {"Z1 Vol+", 26}, {"Z2 Vol+", 27}, {"Z3 Vol+", 28},
         {"Z1 Vol-", 29}, {"Z2 Vol-", 30}, {"Z3 Vol-", 31}}};

    ui::NumberField field_gain_{{48, 68}, 3, {0, 89}, 1, ' '};

    ui::Button button_tx_{{150, 40, 80, 28}, "TX"};

    ui::Text text_status_{{0, 92, 240, 16}, "Idle"};
    ui::Console console_{{0, 112, 240, 120}};

    ui::Labels notes_{
        {{0, 268}, "TX only on TX press.", ui::Color::yellow()},
        {{0, 284}, "433.92 MHz ISM - check local law.", ui::Color::yellow()},
    };
};

}  // namespace touchtunes
}  // namespace app

#endif /*__MB200_UI_TOUCHTUNES_H__*/
