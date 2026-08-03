/*
 * mayhem-b200 — VOR TX.
 *
 * Ported from firmware/application/external/vor_tx/ (ui_vor_tx.*) and its
 * baseband processor firmware/baseband/proc_vor_tx.* .
 *
 * What upstream's VorTxProcessor::execute() builds, sample by sample, is a
 * conventional VOR composite signal expressed directly as DSB-AM with the
 * carrier held at the LO (Q always 0):
 *
 *   - a 30 Hz "variable" tone, amplitude-modulated onto the carrier, whose
 *     PHASE is the bearing being encoded (var = sin(ref_phase - radial));
 *   - a 9960 Hz subcarrier, itself amplitude-modulated onto the carrier, and
 *     itself FM-modulated +/-480 Hz by an undelayed 30 Hz "reference" tone;
 *   - an optional 1020 Hz CW identification tone, keyed in Morse at 7 WPM on
 *     a fixed 10 s repeat cycle, amplitude-modulated onto the carrier too.
 *
 *   envelope(t) = carrier_level
 *               + var_depth * sin(ref_phase(t) - radial_offset)
 *               + sub_depth * sin(sub_phase(t))
 *               + id_depth * sin(id_phase(t)) * ident_active(t)
 *
 *   sub_phase'(t) = 2*pi*9960 + 2*pi*480 * sin(ref_phase(t))     [instantaneous
 *                                                                 frequency]
 *
 * and the receiving VOR (or this project's ui_vor_rx.cpp) recovers the
 * bearing as arg(REFERENCE) - arg(VARIABLE), i.e. exactly `radial_offset`.
 * Levels are upstream's int8 ratios (carrier 60, var 18, sub 18, id 6, all
 * out of 127) carried over unchanged as fractions of full scale.
 *
 * Two deliberate host departures, matching this project's established
 * convention (see dsp/modulate.hpp's file header for the same trade):
 *
 *   1. The 32-bit fixed-point phase accumulator indexing a 256-entry int8
 *      sine table is replaced with double-precision phase accumulators and
 *      std::sin/cos. That quantisation was a cost of the M4's lack of an
 *      FPU, not part of the VOR specification.
 *   2. Output is normalised to [0, 1] rather than upstream's int8 [0, 127]
 *      envelope; the transmit chain's own scaling handles headroom.
 *
 * The CW identifier reuses this project's already-ported ITU Morse encoder
 * (app::morse_tx::morse_encode(), ui_morse_tx.hpp) instead of re-deriving the
 * code table, so the two apps agree on the exact same dot/dash timing.
 *
 * LEGALITY: VOR is an aviation navigation band. Transmitting on it interferes
 * with real air-navigation receivers; upstream requires an explicit
 * acknowledgement before the first transmission and this port does the same.
 * No hardware is attached during development, so actual radiation is
 * unverified — say so on screen.
 *
 * Copyright (C) 2026 PortaPack Mayhem (original app and baseband)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_VOR_TX_H__
#define __MB200_UI_VOR_TX_H__

#include "../dsp/demod.hpp" /* dsp::cfloat */
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace app {
namespace vor_tx {

/* Protocol constants, verbatim from proc_vor_tx.hpp. */
constexpr double kToneHz = 30.0;
constexpr double kSubcarrierHz = 9960.0;
constexpr double kSubcarrierDeviationHz = 480.0;
constexpr double kIdentToneHz = 1020.0;

/* AM levels, upstream's int8 ratios (carrier_level=60, var_depth=18,
 * sub_depth=18, id_depth=6, all out of 127) as fractions of full scale. */
constexpr double kCarrierLevel = 60.0 / 127.0;
constexpr double kVarDepth = 18.0 / 127.0;
constexpr double kSubDepth = 18.0 / 127.0;
constexpr double kIdDepth = 6.0 / 127.0;

constexpr uint32_t kIdentWpm = 7;
constexpr double kIdentPeriodSeconds = 10.0;

/* Upstream's radial_offset computation (VorTxProcessor::vor_tx_config): the
 * bearing, wrapped to [0, 360), converted to radians. Real VOR radials
 * increase clockwise and the variable tone LAGS the reference by the
 * bearing, so this offset is subtracted from the reference phase, not added
 * — see VorTxGenerator::process(). Pure and free of any DSP state so the
 * bearing -> phase-offset mapping can be tested directly. */
double vor_radial_to_offset_rad(int32_t radial_deg);

/* One CW keying interval: on (tone sounding) or off, for `length_samples`
 * samples at the generator's configured rate. Host counterpart of upstream's
 * IdentSegment (proc_vor_tx.hpp), sized in samples instead of being replayed
 * against a fixed 1.536 MHz baseband. */
struct IdentSegment {
    bool on{false};
    uint64_t length_samples{0};
};

/* Continuous VOR composite generator. Feed it to
 * radio::TransmitterModel::set_iq_source() via Mode::Raw: this class *is* the
 * whole baseband, envelope on I and 0 on Q exactly as
 * VorTxProcessor::execute() emits {env, 0}.
 *
 * Thread-safety: process() and every setter take the same mutex, held for
 * the whole call rather than per sample — setters run on the UI thread and
 * are rare (a radial nudge, an ident-text edit), so one lock per DSP block
 * costs nothing and avoids torn reads of the phase/ident state. */
class VorTxGenerator {
   public:
    void configure(double sample_rate_hz);
    void reset();

    void set_radial(int32_t radial_deg);
    int32_t radial() const;

    void set_ident_enabled(bool enabled);
    bool ident_enabled() const;

    /* Rebuilds the keying schedule from `text` (up to 7 characters, as
     * upstream's ident_text[8] with a trailing NUL). */
    void set_ident_text(const std::string& text);
    const std::string& ident_text() const;

    /* Appends `count` complex samples (envelope on I, 0 on Q) to `out`,
     * which must have room for `count` more entries starting at `out`. */
    void process(dsp::cfloat* out, size_t count);

    double sample_rate() const { return sample_rate_; }

   private:
    void rebuild_ident_schedule_locked();
    bool ident_tone_active_locked();

    mutable std::mutex mutex_;

    double sample_rate_{48000.0};
    double ref_phase_{0.0};
    double sub_phase_{0.0};
    double id_phase_{0.0};

    int32_t radial_deg_{0};
    double radial_offset_rad_{0.0};

    bool ident_enabled_{true};
    std::string ident_text_{"VOR"};
    std::vector<IdentSegment> ident_segments_{};
    size_t ident_index_{0};
    uint64_t ident_remaining_{0};
};

}  // namespace vor_tx

class VorTxView : public ui::View {
   public:
    VorTxView();
    ~VorTxView() override;

    VorTxView(const VorTxView&) = delete;
    VorTxView& operator=(const VorTxView&) = delete;

    std::string title() const override { return "VOR TX"; }

    void focus() override;
    void on_frame_sync() override;

   private:
    void start_tx();
    void stop_tx();
    void request_start();

    vor_tx::VorTxGenerator generator_{};
    bool transmitting_{false};
    bool tx_acknowledged_{false};
    std::string ident_edit_buffer_{};

    ui::Labels labels_{
        {{0, 0}, "Radial", ui::Color::light_grey()},
        {{0, 20}, "Freq", ui::Color::light_grey()},
        {{0, 60}, "Status", ui::Color::light_grey()},
    };

    ui::NumberField field_radial_{{56, 0}, 3, {0, 359}, 1, '0'};
    ui::Text text_radial_unit_{{92, 0, 32, 16}, "deg"};

    ui::FrequencyField field_freq_{{56, 20}};
    ui::FrequencyStepView step_view_{{164, 20}, field_freq_};

    ui::Checkbox check_ident_{{0, 40}, 14, "CW identifier", true};
    ui::Button button_ident_text_{{160, 36, 72, 24}, "VOR"};

    ui::Text text_status_{{56, 60, 176, 16}, "Idle"};

    ui::Labels warning_{
        {{0, 84}, "Aviation nav band. TX only", ui::Color::yellow()},
        {{0, 100}, "into a dummy load / shielded", ui::Color::yellow()},
        {{0, 116}, "test setup.", ui::Color::yellow()},
        {{0, 140}, "No hardware attached: RF is", ui::Color::grey()},
        {{0, 156}, "unverified without a B200.", ui::Color::grey()},
    };

    ui::Button button_tx_{{0, 260, 240, 32}, "Start"};
};

}  // namespace app

#endif /*__MB200_UI_VOR_TX_H__*/
