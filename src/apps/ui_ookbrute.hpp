/*
 * mayhem-b200 — OOK Brute: brute-force an OOK remote-control code space.
 *
 * Port of the firmware's application/external/ookbrute/*. It walks every code
 * from a start to a stop value and transmits each one framed for the chosen
 * fixed-code remote protocol (CAME 12/24-bit, Nice 12/24-bit, Holtek HT12, and
 * Princeton 24-bit). Each protocol's preamble, per-bit "zero"/"one" fragment
 * patterns, symbol timing and repeat count are copied verbatim from upstream's
 * generate_packet(), so a brute run produces the same waveforms a real fob does.
 *
 * (The task brief calls this "de Bruijn brute-force"; upstream's ookbrute app is
 * in fact a plain incrementing-counter sweep, one full frame per code. The de
 * Bruijn code-space walk lives in the OOK TX app's "de Bruijn" tab — see
 * ui_encoders.* — where the firmware actually implements it. Both are ported.)
 *
 * How it differs from the firmware:
 *
 *  - The M4 OOK image is replaced by dsp::OokKeyer streaming into
 *    radio::TransmitterModel in Raw mode. samples_per_bit is computed with the
 *    same expressions upstream uses, so the symbol timing matches.
 *  - Advancing to the next code happens in on_frame_sync() when the current
 *    burst finishes, replacing the firmware's TXProgress-message callback.
 *
 * generate_packet() and the fragment packing are in namespace app::ookbrute with
 * no UI or radio dependency, so tests/test_ookbrute.cpp can assert a code's
 * exact fragment string and timing against the upstream template.
 *
 * LEGALITY: brute-forcing a remote transmits a long series of real gate and
 * garage codes. This is illegal in most jurisdictions and is your
 * responsibility. It runs only when you press Start and shows a warning on
 * screen.
 *
 * Copyright (C) 2024 HTotoo
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_OOKBRUTE_H__
#define __MB200_UI_OOKBRUTE_H__

#include "../dsp/modulate.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace app {
namespace ookbrute {

constexpr uint32_t kOokSampleRate = 2'280'000U;

enum Protocol : uint32_t {
    Came12 = 0,
    Came24 = 1,
    Nice12 = 2,
    Nice24 = 3,
    Holtek12 = 4,
    Princeton24 = 5,
};

struct BrutePacket {
    std::string fragments{};    // the framed '0'/'1' waveform for one code
    uint32_t samples_per_bit{0};
    uint16_t repeat{1};
    uint16_t pause_symbols{0};
    uint16_t databits{0};
};

/* Number of code bits for a protocol (12 or 24). */
uint8_t protocol_bits(uint32_t protocol);

/* Highest code value for a protocol: (1 << bits) - 1. */
uint32_t protocol_max_code(uint32_t protocol);

/* Build the framed fragment string and timing for one code, exactly as
 * upstream's OOKBruteView::generate_packet() does. */
BrutePacket generate_packet(uint32_t protocol, uint32_t code);

/* Pack a '0'/'1' fragment string into bytes, MSB-first. */
size_t pack_fragments(const std::string& fragments, std::vector<uint8_t>& out);

}  // namespace ookbrute

class OOKBruteView : public ui::View {
   public:
    OOKBruteView();
    ~OOKBruteView() override;

    OOKBruteView(const OOKBruteView&) = delete;
    OOKBruteView& operator=(const OOKBruteView&) = delete;

    std::string title() const override { return "OOKBrute"; }

    void focus() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void start();
    void stop();
    void arm_current();
    void update_start_stop(uint32_t protocol);
    void validate_start_stop();

    bool is_running{false};
    uint32_t counter_{0};
    uint32_t start_code_{0};
    uint32_t stop_code_{0};
    std::vector<uint8_t> bits_{};
    std::shared_ptr<dsp::OokKeyer> keyer_{};

    ui::FrequencyField field_frequency_{{0, 0}};

    ui::Labels labels_{
        {{0, 2 * 16}, "Start code:", ui::Color::light_grey()},
        {{0, 5 * 16}, "Stop code:", ui::Color::light_grey()},
        {{0, 8 * 16}, "Encoder:", ui::Color::light_grey()}};

    ui::NumberField field_start_{{0, 3 * 16}, 8, {0, 16777215}, 1, ' ', true};
    ui::NumberField field_stop_{{0, 6 * 16}, 8, {0, 16777215}, 1, ' ', true};

    ui::OptionsField options_atkmode_{
        {0, 9 * 16},
        12,
        {{"Came12", ookbrute::Came12},
         {"Came24", ookbrute::Came24},
         {"Nice12", ookbrute::Nice12},
         {"Nice24", ookbrute::Nice24},
         {"Holtek12", ookbrute::Holtek12},
         {"Princeton24", ookbrute::Princeton24}}};

    ui::Text text_status_{{0, 11 * 16, ui::screen_width, 16}, "Ready"};
    ui::ProgressBar progressbar_{{0, 12 * 16, ui::screen_width, 16}};

    ui::Text text_warning_{{0, 13 * 16, ui::screen_width, 16},
                           "Illegal in most places - your call"};

    ui::Button button_startstop_{{4 * 8, 15 * 16, 16 * 8, 28}, "Start"};
};

}  // namespace app

#endif /*__MB200_UI_OOKBRUTE_H__*/
