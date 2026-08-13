/*
 * mayhem-b200 — capability-driven RX sample-rate selection.
 *
 * Every decoder app in this tree picks its capture rate from a constant, and
 * every one of those constants was sized for a PortaPack/HackRF. That throws
 * away most of what a connected radio can do in one direction and asks for the
 * impossible in the other: ADS-B asking for 2 Msps gets two samples per bit on
 * a B200 that could comfortably give it eight, while the same constant is above
 * what some RTL-SDR dongles will stream at all.
 *
 * The fix is not "ask for the maximum". Rate is a trade with several sides, and
 * an app knows its own side of it far better than any central table does:
 *
 *   - Below some rate the DSP simply cannot work (fewer samples than the
 *     symbol rate needs).
 *   - Above some rate it gains nothing and only burns CPU and USB bandwidth.
 *   - Some apps want the LOWEST rate the device offers, not the highest —
 *     src/apps/ui_vor_rx.cpp does, because a wider band makes its bursts
 *     shorter. Direction is expressed by where the ideal sits, not by a flag.
 *   - A resampler running at an exact integer ratio is cheaper and cleaner than
 *     one at 2.4/2.0, so an app that resamples has a preferred grid.
 *
 * So an app states its constraints and this picks a rate for the radio that is
 * actually attached. Pure logic: it reads a DeviceCaps and returns a number. No
 * hardware, no I/O, nothing from ReceiverModel — which is also what makes it
 * exhaustively testable against device profiles nobody here owns.
 *
 * With no radio attached, pass DeviceCaps{}: the result is the caller's own
 * ideal, flagged RateOutcome::CapsUnusable, so a call site needs no null dance.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_RATE_POLICY_H__
#define __MB200_RATE_POLICY_H__

#include "radio_device.hpp"

namespace radio {

/* What an app needs from the sample rate, as constraints rather than a
 * constant. Every field is in Hz and every field is optional: a zero, negative
 * or non-finite value means "no opinion", never "zero Hz".
 *
 * Example — ADS-B is 1 Mbit/s PPM, so it needs at least 2 samples per bit to
 * decode at all and gains little past 8:
 *
 *     RatePreference want;
 *     want.ideal_hz              = 8e6;
 *     want.minimum_hz            = 2e6;
 *     want.max_useful_hz         = 8e6;
 *     want.prefer_multiple_of_hz = 2e6;   // the demodulator's native rate
 */
struct RatePreference {
    /* What the DSP would run at if the hardware allowed anything. */
    double ideal_hz{0.0};

    /* Below this the app cannot work at all, only pretend to. */
    double minimum_hz{0.0};

    /* Above this the app gains nothing, so the extra CPU and bus bandwidth is
     * pure cost. Honoured even on a radio that could go much higher. */
    double max_useful_hz{0.0};

    /* Preferred grid, honoured where the device's range allows it. Usually the
     * app's native DSP rate, so its resampler runs at an exact integer ratio. */
    double prefer_multiple_of_hz{0.0};
};

/* How the chosen rate relates to what was asked for. The caller must be able to
 * tell "degraded but usable" from "cannot work", so this is not a bool.
 *
 * Ideal/Raised/Reduced are measured against the ideal AFTER max_useful_hz and
 * minimum_hz have been applied to it: if an app says it gains nothing above
 * 8 Msps, then 8 Msps IS its ideal and is reported as such. */
enum class RateOutcome {
    Ideal,         /* got the ideal rate */
    Raised,        /* above the ideal: the device could not go lower */
    Reduced,       /* below the ideal but at or above the minimum — degraded */
    BelowMinimum,  /* the device cannot reach minimum_hz: this cannot work */
    CapsUnusable,  /* no usable rx_rate range; the caller's ideal is returned */
};

const char* rate_outcome_to_string(RateOutcome outcome);

struct RateChoice {
    /* The rate to ask the device for. Never NaN. Zero only when the caller
     * expressed no rate at all AND the caps carried none either — check
     * usable() rather than assuming. */
    double rate_hz{0.0};

    RateOutcome outcome{RateOutcome::Ideal};

    /* True when the app can actually run at this rate. False means the attached
     * radio cannot reach the app's minimum; rate_hz is then the closest the
     * device can get, for a caller that wants to show a spectrum anyway. */
    bool usable() const {
        return rate_hz > 0.0 && outcome != RateOutcome::BelowMinimum;
    }

    /* False when the device published no usable rx_rate range, so the rate is
     * the caller's own ideal and the hardware may reject or clamp it. */
    bool from_caps() const { return outcome != RateOutcome::CapsUnusable; }

    const char* text() const { return rate_outcome_to_string(outcome); }
};

/* Pick the best RX rate the described device supports.
 *
 * Guarantees, in priority order:
 *  1. The result is inside [caps.rx_rate.min, caps.rx_rate.max] whenever that
 *     range is usable — a physical limit beats every preference. (A published
 *     step is honoured too, where one is given.)
 *  2. The result never exceeds max_useful_hz unless the device's own minimum
 *     already does.
 *  3. The result is a multiple of prefer_multiple_of_hz where the range admits
 *     one without breaking 1 or 2.
 *  4. Otherwise the result is as close to ideal_hz as those allow.
 *  5. The result is never NaN, and never zero unless there was no information
 *     at all to work from. */
RateChoice choose_rx_rate(const DeviceCaps& caps, const RatePreference& want);

/* For a caller that has already decided it will run whatever happens. Prefer
 * choose_rx_rate() — this cannot tell you the device fell short. */
double choose_rx_rate_hz(const DeviceCaps& caps, const RatePreference& want);

}  // namespace radio

#endif /*__MB200_RATE_POLICY_H__*/
