/*
 * mayhem-b200 — SAME / EAS alert header transmitter (AFSK).
 *
 * Ported from firmware/application/external/same_tx/ui_same_tx.* . SAME (the
 * Specific Area Message Encoding used by the US Emergency Alert System) sends an
 * AFSK burst: a preamble of sixteen 0xAB bytes followed by an ASCII header
 *
 *     ZCZC-ORG-EVT-PSSCCC+HHMM-JJJHHMM-LLLLLLLL-
 *
 * (upstream fixes the location padding, issue time 0010000 and station SAMETX).
 * Every byte goes out least-significant-bit first — upstream pre-reverses each
 * byte (bitrev8) because its modem clocks bits most-significant first, which is
 * the same on-air order. The bit stream is AFSK at 520.83 baud, mark 2083 Hz for
 * a 1, space 1563 Hz for a 0, and the whole header burst is sent three times.
 *
 * The tones are then FM-modulated (±5 kHz) for the 162 MHz NOAA weather band,
 * which is what the receivers listen on.
 *
 * LEGALITY: transmitting EAS/SAME alert tones is illegal in most jurisdictions
 * (in the US, 47 CFR 11.45 forbids transmitting the codes or the Attention
 * Signal except in an actual alert, authorised test, or PSA). This app is ported
 * only because Mayhem has it. Transmit starts ONLY on an explicit TX press and
 * the app shows the exact header it will send first.
 *
 * Copyright (C) 2024 HTotoo (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SAME_TX_H__
#define __MB200_UI_SAME_TX_H__

#include "../dsp/ring_buffer.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace radio {
class TransmitterModel;
}

namespace app {
namespace same_tx {

inline constexpr uint8_t kPreambleByte = 0xAB;
inline constexpr size_t kPreambleCount = 16;
inline constexpr float kMarkHz = 2083.0f;
inline constexpr float kSpaceHz = 1563.0f;
inline constexpr float kBaud = 520.833f;
inline constexpr float kDeviationHz = 5000.0f;
inline constexpr int kRepeat = 3;

const char* org_code(size_t index);   /* 0..3: WXR EAS CIV PEP */
const char* evt_code(size_t index);   /* 0..25: RWT RMT ... TOR */
size_t org_count();
size_t evt_count();

/* The SAME header string, upstream layout:
 *   "ZCZC-ORG-EVT-SS0CCC+HHMM-0010000-SAMETX--"
 * `ss` is the 2-digit FIPS state, `ccc` the 3-digit county, `dh`/`dm` the
 * purge-time hours/minutes. */
std::string build_message(size_t org_idx, size_t evt_idx, int ss, int ccc, int dh, int dm);

/* The transmitted byte stream for one header burst: 16 preamble bytes (0xAB)
 * then the ASCII header. */
std::vector<uint8_t> build_bytes(const std::string& message);

/* The on-air bit stream (each byte least-significant-bit first) for one burst. */
std::vector<uint8_t> build_bits(const std::string& message);

/* --- View ----------------------------------------------------------------- */

class SameTxView : public ui::View {
   public:
    SameTxView();
    ~SameTxView() override;

    SameTxView(const SameTxView&) = delete;
    SameTxView& operator=(const SameTxView&) = delete;

    std::string title() const override { return "SAME TX"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void update_preview();
    void start_tx();
    void stop_tx();

    radio::TransmitterModel* transmitter_{nullptr};
    bool transmitting_{false};

    static constexpr float kAudioRate = 48'000.0f;

    dsp::RingBuffer<float> audio_ring_{1u << 19};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{0, 24}, "Org", ui::Color::light_grey()},
        {{100, 24}, "Evt", ui::Color::light_grey()},
        {{0, 46}, "FIPS", ui::Color::light_grey()},
        {{0, 68}, "Dur h/m", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};

    ui::OptionsField field_org_{
        {40, 24}, 3, {{"WXR", 0}, {"EAS", 1}, {"CIV", 2}, {"PEP", 3}}};
    ui::NumberField field_evt_{{140, 24}, 2, {0, 25}, 1, '0'};
    ui::Text text_evt_{{170, 24, 3 * 8, 16}, "RWT"};

    ui::NumberField field_state_{{48, 46}, 2, {0, 99}, 1, '0'};
    ui::NumberField field_county_{{80, 46}, 3, {0, 999}, 1, '0'};

    ui::NumberField field_dur_h_{{72, 68}, 2, {0, 99}, 1, '0'};
    ui::NumberField field_dur_m_{{104, 68}, 2, {0, 59}, 1, '0'};

    ui::Button button_tx_{{160, 60, 72, 28}, "TX"};

    ui::Text text_msg_{{0, 92, 240, 16}, ""};
    ui::Text text_status_{{0, 110, 240, 16}, "Idle"};

    ui::Labels notes_{
        {{0, 250}, "EAS ALERT TONES.", ui::Color::red()},
        {{0, 266}, "Illegal to transmit in most", ui::Color::yellow()},
        {{0, 282}, "places. TX only on TX press.", ui::Color::yellow()},
    };
};

}  // namespace same_tx
}  // namespace app

#endif /*__MB200_UI_SAME_TX_H__*/
