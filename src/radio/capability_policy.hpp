/*
 * mayhem-b200 — capability-driven front end: analog bandwidth and gain.
 *
 * rate_policy.hpp does this for the sample rate. This file is the rest of the
 * front end — the other two numbers ReceiverModel and TransmitterModel push
 * into a radio whose limits they do not know — and it exists for the same
 * reason: the adaptation belongs in ONE place that can be tested against
 * devices nobody here owns, not spread across ~94 apps.
 *
 * Everything here is pure logic. It reads a DeviceCaps (or a single Range) and
 * returns a number plus an outcome saying how that number relates to what was
 * asked for. No hardware, no I/O, nothing from ReceiverModel.
 *
 * ===========================================================================
 * 1. Analog RX bandwidth
 * ===========================================================================
 *
 * A radio has two bandwidths and only one of them is chosen by the sample
 * rate. The digital one is Nyquist: complex baseband sampling at R samples per
 * second covers -R/2 .. +R/2, so the usable span is R hertz. The analog one is
 * a real filter in front of the ADC, and it stays wherever it was last set.
 *
 * Leaving it there is not free. A B200's front end is adjustable from 200 kHz
 * to 56 MHz (Ettus UHD manual, "USRP B2x0 Series": the analog frontend "has a
 * seamlessly adjustable bandwidth of 200 kHz to 56 MHz"). That same page caps
 * the master clock at 61.44 MHz and notes that rates above 56 MHz are
 * "possible, but not recommended" — the two figures are the same quantity, and
 * they can only be the same quantity on the two-sided reading. So the device's
 * bandwidth control names the total complex span, directly comparable to a
 * sample rate, and an app sampling 2 Msps on a front end still open to 56 MHz
 * is folding 28 MHz of neighbouring spectrum into its 2 MHz window. Nothing
 * downstream can undo a fold: once two frequencies land on the same bin they
 * are one signal.
 *
 * So the analog filter must follow the rate. What it must NOT do is follow it
 * too closely in either direction:
 *
 *  - Too narrow and the -3 dB corner moves inside the band the receiver
 *    actually uses. ReceiverModel::retune_if_needed() deliberately leaves the
 *    LO alone while the wanted signal is anywhere inside +/-0.4 * R, mixing it
 *    down with the NCO instead, because retuning an AD936x costs milliseconds
 *    and a settling transient. Setting the filter to 0.8 * R — which is what
 *    this code did before this policy existed — puts the corner at exactly
 *    +/-0.4 * R, so a signal parked at the edge of that tuning window arrives
 *    3 dB down, and every waterfall droops at both ends of the span it draws.
 *
 *  - Too wide and the fold above comes back in proportion.
 *
 * kRxAnalogBandwidthRatio = 1.0 is the balance point: the analog corner sits
 * at Nyquist, so the wanted signal — at worst +/-0.4 * R — is at 0.8 of the
 * corner frequency rather than on it, while everything past Nyquist that would
 * fold is already on the filter's skirt and falling. It is also what the rest
 * of the SDR world does; "set the analog bandwidth to the sample rate" is the
 * default in UHD's own examples and in gr-osmosdr.
 *
 * When the device publishes no usable rx_bandwidth range, this does NOT guess.
 * A Range of all zeros means "unknown" throughout this codebase (see
 * Range::clamp in radio_device.cpp, which passes a value straight through
 * rather than clamping it to zero), and it is exactly what an sdrlink server
 * that omits the field leaves behind (network_radio.cpp range_from_json). The
 * alternative — send the rate anyway and let the far side sort it out — was
 * considered and rejected: an unvalidated bandwidth on a device of unknown
 * shape is a guess that can be rejected outright, and the operator has no way
 * to tell it happened. Instead the outcome says CapsUnusable, the caller skips
 * the write, and the analog filter keeps whatever the device chose for itself.
 *
 * The published Range::step is deliberately ignored, unlike in choose_rx_rate.
 * A sample rate is a clock the streamer must lock to, so landing off the grid
 * is a real failure; an analog filter corner is a continuous tune that every
 * backend already rounds for us and reports back (UsrpRadio::set_rx_bandwidth
 * returns get_rx_bandwidth()). The B200 publishes a 1 Hz step, which is to say
 * none at all.
 *
 * ===========================================================================
 * 2. Gain
 * ===========================================================================
 *
 * ReceiverModel::set_gain(db) went straight to RadioDevice::set_rx_gain(db),
 * and what happened to an out-of-range or nonsense request depended entirely
 * on which backend was underneath:
 *
 *   UsrpRadio     clamps against caps_.rx_gain, always.
 *   NetworkRadio  clamps only while the link is DOWN. With the link up it
 *                 formats the request into JSON and sends it. A NaN there
 *                 becomes the token `nan` (network_radio.cpp
 *                 format_json_number falls through to "%.17g"), which is not
 *                 JSON at all and which the far side cannot parse.
 *
 * That is the hazard this closes: validate once, in the shared layer, before
 * any backend sees the number. A non-finite request is refused outright and
 * the radio is not touched — there is no sensible gain to substitute, and
 * silently picking one would hide the bug that produced the NaN. A finite
 * request is clamped into the published range and the caller is told which end
 * it hit, so a UI can say "76 dB (device maximum)" instead of showing a number
 * the hardware quietly ignored.
 *
 * When the gain range itself is unusable the request passes through untouched,
 * flagged Unvalidated. This is the opposite of the bandwidth decision above,
 * and deliberately so: an unset gain leaves the receiver deaf or saturated,
 * whereas an unset analog filter merely leaves it wide. Passing through also
 * preserves what every backend does today, since each one clamps against its
 * own caps as a second line of defence.
 *
 * A gain range is treated as usable when max > min, matching Range::clamp's
 * own convention. Note that min is NOT required to be non-negative: several
 * dongles publish genuinely negative gain (attenuation) figures, so the
 * validation here checks finiteness and ordering only.
 *
 * What was NOT found, having gone looking: no HackRF-shaped gain assumption
 * survives in shared code. ReceiverModel exposes one continuous gain rather
 * than the PortaPack's LNA/VGA/AMP trio, and all ~30 apps with a gain field
 * already size it from caps.rx_gain. Two hard-coded 40s remain (main.cpp's
 * startup gain and Settings::rx_gain's default); both are inside a B200's
 * 0-76 dB and, after this change, are clamped in one place on a device where
 * they are not. Two cosmetic bugs are worth recording for whoever touches
 * those files next, since neither is fixable from here without editing apps:
 * device_app.cpp prints the gain range as "0 - <max>", hard-coding a minimum
 * that a negative-gain device does not have, and every app builds its gain
 * field with static_cast<int32_t>(caps.rx_gain.min/max), which is undefined
 * behaviour rather than a clamp if a remote server ever publishes a non-finite
 * gain range.
 *
 * ===========================================================================
 * 3. Antennas, full duplex, channels: investigated, deliberately not done
 * ===========================================================================
 *
 * These are the remaining DeviceCaps fields with no policy attached. Each was
 * traced; none earns code on a single-channel receive path, and the reasoning
 * is written down here so the next person does not re-derive it.
 *
 * ANTENNAS (caps.rx_antennas / tx_antennas). Read in exactly one place,
 * device_app.cpp, which builds its selector from the published list and hands
 * back an element of that same list — so an invalid name cannot be produced
 * and a validating helper would have no caller. There IS a real trap here, and
 * it is not one a policy can fix: UHD leaves a fresh B200 on whichever RX port
 * it defaults to, and an antenna screwed into the other one receives nothing
 * at all while every capability, rate and gain reads perfectly healthy. No
 * amount of capability data says which port has the antenna on it, so guessing
 * would be a policy that is wrong half the time and confidently silent when it
 * is. The honest fix is a UI one — surface the selected port next to the
 * signal level, where an operator seeing a dead band will look — which belongs
 * in device_app/the status bar, not here.
 *
 * FULL DUPLEX (caps.full_duplex). Only ever constrains the receive path
 * through the transmit path: on a half-duplex radio, starting TX while RX runs
 * is what breaks, and the shape of the fix is TransmitterModel::start()
 * stopping the receiver first (or refusing) rather than anything the receiver
 * decides on its own. Left alone here for one reason and it is not effort:
 * this port's transmit path is still unverified on real hardware, and adding
 * an untested TX state machine underneath an untested TX path buys nothing but
 * a second suspect. caps.has_tx is already honoured where it currently
 * matters — app_bridge.cpp publishes can_transmit so the portal locks the TX
 * apps on a receive-only SDR.
 *
 * CHANNELS (caps.rx_channels / tx_channels). Unreachable by construction, in
 * both backends and for different reasons: UsrpRadio pins itself to channel 0
 * (kChannel) and even forces a single-channel subdev spec on open, and the
 * sdrlink protocol has no per-channel concept at all — every command addresses
 * "the" opened device (network_radio.cpp caps_from_json therefore reports 1).
 * Reaching a B210's second channel means widening RadioDevice's entire
 * interface, both backends, both models and the app model's assumption that
 * there is one receiver. That is a project, not a policy, and the caps field
 * is currently informational only.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_CAPABILITY_POLICY_H__
#define __MB200_CAPABILITY_POLICY_H__

#include "radio_device.hpp"

namespace radio {

/* --- Analog RX bandwidth --------------------------------------------------- */

/* Analog filter width as a multiple of the complex sample rate. See the
 * reasoning in section 1 above: 1.0 puts the -3 dB corner at Nyquist, which is
 * outside the +/-0.4 * rate window ReceiverModel tunes within and inside the
 * region where out-of-band energy would fold. */
inline constexpr double kRxAnalogBandwidthRatio = 1.0;

/* How the chosen analog bandwidth relates to the sample rate it should follow.
 * Two of these mean "do not touch the radio", which is why this is not a
 * bool — see BandwidthChoice::should_apply(). */
enum class BandwidthOutcome {
    Matched,       /* the filter can be set to exactly what the rate wants */
    Widened,       /* the device's narrowest filter is wider than that */
    Narrowed,      /* the device's widest filter is narrower than that */
    NoRate,        /* the rate is not a usable number: nothing to follow */
    CapsUnusable,  /* no usable rx_bandwidth range: leave the filter alone */
};

const char* bandwidth_outcome_to_string(BandwidthOutcome outcome);

struct BandwidthChoice {
    /* The bandwidth to ask the device for, in Hz. Never NaN. Meaningless — and
     * zero — unless should_apply() is true. */
    double bandwidth_hz{0.0};

    BandwidthOutcome outcome{BandwidthOutcome::CapsUnusable};

    /* True when the caller should write this to the radio. False means the
     * capability data does not support a decision and the analog filter must be
     * left exactly as the device set it. */
    bool should_apply() const {
        return bandwidth_hz > 0.0 && outcome != BandwidthOutcome::NoRate &&
               outcome != BandwidthOutcome::CapsUnusable;
    }

    /* False when the device could not give the bandwidth the rate wanted, so
     * the captured band is either rolled off at the edges (Narrowed) or wider
     * than Nyquist and folding (Widened). */
    bool exact() const { return outcome == BandwidthOutcome::Matched; }

    const char* text() const { return bandwidth_outcome_to_string(outcome); }
};

/* Pick the analog RX filter width that suits `rate_hz` on the described device.
 *
 * Guarantees, in priority order:
 *  1. should_apply() is false — and nothing should be written to the radio —
 *     unless caps.rx_bandwidth is a usable range AND rate_hz is a positive
 *     finite number.
 *  2. Otherwise the result is inside [caps.rx_bandwidth.min,
 *     caps.rx_bandwidth.max]; a physical limit beats the rate.
 *  3. Otherwise the result is kRxAnalogBandwidthRatio * rate_hz.
 *  4. The result is never NaN and never negative. */
BandwidthChoice choose_rx_bandwidth(const DeviceCaps& caps, double rate_hz);

/* --- Gain ------------------------------------------------------------------ */

/* How the gain actually requested of the device relates to what was asked for.
 * Invalid means "do not touch the radio at all"; see GainChoice::should_apply(). */
enum class GainOutcome {
    Applied,      /* the request is inside the published range */
    ClampedLow,   /* below the device minimum: raised to it */
    ClampedHigh,  /* above the device maximum: lowered to it */
    Unvalidated,  /* no usable gain range: the request passes through as-is */
    Invalid,      /* the request is not a finite number: refuse it */
};

const char* gain_outcome_to_string(GainOutcome outcome);

struct GainChoice {
    /* The gain to ask the device for, in dB. Never NaN. Zero and meaningless
     * when should_apply() is false. */
    double gain_db{0.0};

    GainOutcome outcome{GainOutcome::Invalid};

    /* True when the caller should write this to the radio. Only a request that
     * is not a number is refused: a request that is merely out of range has
     * been clamped into one that is. */
    bool should_apply() const { return outcome != GainOutcome::Invalid; }

    /* True when the published range is what produced this value, so a UI may
     * describe it as a device limit. False under Unvalidated, where the number
     * is the caller's own and the backend is the only thing that will check it. */
    bool from_caps() const {
        return outcome == GainOutcome::Applied || outcome == GainOutcome::ClampedLow ||
               outcome == GainOutcome::ClampedHigh;
    }

    /* True when the device could not give the gain that was asked for. */
    bool clamped() const {
        return outcome == GainOutcome::ClampedLow || outcome == GainOutcome::ClampedHigh;
    }

    const char* text() const { return gain_outcome_to_string(outcome); }
};

/* Validate and clamp a gain request against one published gain range.
 *
 * Guarantees:
 *  1. A non-finite request is refused: should_apply() is false and the caller
 *     must leave the radio alone.
 *  2. With a usable range (finite, max > min) the result is inside it.
 *  3. With an unusable range the finite request is returned untouched, flagged
 *     Unvalidated.
 *  4. The result is never NaN. */
GainChoice choose_gain(const Range& range, double requested_db);

/* The same, naming the range so call sites read as what they are. */
GainChoice choose_rx_gain(const DeviceCaps& caps, double requested_db);
GainChoice choose_tx_gain(const DeviceCaps& caps, double requested_db);

}  // namespace radio

#endif /*__MB200_CAPABILITY_POLICY_H__*/
