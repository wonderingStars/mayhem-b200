/*
 * mayhem-b200 — capability-driven RX sample-rate selection.
 *
 * See rate_policy.hpp for what this is for. The implementation is deliberately
 * one straight-line function: normalise the request, decide whether the caps
 * can be trusted, clamp, snap, classify. Every branch below is pinned by a case
 * in tests/test_rate_policy.cpp.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "rate_policy.hpp"

#include <algorithm>
#include <cmath>

namespace radio {

namespace {

/* Classification tolerance. Sample rates are whole numbers of Hz on every
 * device we have seen, so being a fraction of a hertz off the ideal is not a
 * degradation worth reporting. */
constexpr double kEpsHz = 1.0;

/* Slack for the division in floor/ceil-to-a-multiple, so a rate that IS an
 * exact multiple never lands one ULP below and snaps down a whole step. */
constexpr double kQuantEps = 1e-9;

bool positive_finite(double v) {
    return std::isfinite(v) && v > 0.0;
}

/* Read a preference field: anything not positive and finite means "no opinion",
 * never "zero hertz". */
double opinion(double v) {
    return positive_finite(v) ? v : 0.0;
}

double floor_multiple(double v, double m) {
    return std::floor(v / m + kQuantEps) * m;
}

double ceil_multiple(double v, double m) {
    return std::ceil(v / m - kQuantEps) * m;
}

/* Devices publish their rate grid as Range::step, counted from Range::min. A
 * value is moved DOWN onto that grid, never up, so it cannot escape the range's
 * ceiling; starting from min means it cannot fall through the floor either. */
double snap_to_step(double v, const Range& r) {
    if (!positive_finite(r.step)) return v;
    const double base = (std::isfinite(r.min) && r.min > 0.0) ? r.min : 0.0;
    if (v <= base) return base;
    const double k = std::floor((v - base) / r.step + kQuantEps);
    return base + k * r.step;
}

bool on_step_grid(double v, const Range& r) {
    if (!positive_finite(r.step)) return true;
    const double tol = std::max(1e-6, std::fabs(v) * 1e-9);
    return std::fabs(v - snap_to_step(v, r)) <= tol;
}

}  // namespace

const char* rate_outcome_to_string(RateOutcome outcome) {
    switch (outcome) {
        case RateOutcome::Ideal:        return "ideal";
        case RateOutcome::Raised:       return "above ideal";
        case RateOutcome::Reduced:      return "below ideal";
        case RateOutcome::BelowMinimum: return "below minimum";
        case RateOutcome::CapsUnusable: return "caps unusable";
    }
    return "unknown";
}

RateChoice choose_rx_rate(const DeviceCaps& caps, const RatePreference& want) {
    RateChoice out;

    /* --- 1. Normalise the request ------------------------------------------
     *
     * After this, `ideal` is the single number the outcome is measured against
     * and the other two are plain bounds. */
    double ideal = opinion(want.ideal_hz);
    const double minimum = opinion(want.minimum_hz);
    const double max_useful = opinion(want.max_useful_hz);
    const double multiple = opinion(want.prefer_multiple_of_hz);

    /* An app that states only a floor or only a ceiling still gets a sensible
     * target rather than zero. */
    if (ideal <= 0.0) ideal = (minimum > 0.0) ? minimum : max_useful;

    /* A ceiling below the ideal is the app saying the extra rate buys it
     * nothing, so the ceiling becomes the ideal. A floor above that wins in
     * turn: "cannot work below" is a functional limit, while "gains nothing
     * above" is only a CPU preference, and a contradictory pair of bounds must
     * still leave the app able to work. */
    if (max_useful > 0.0 && ideal > max_useful) ideal = max_useful;
    if (minimum > 0.0 && ideal < minimum) ideal = minimum;

    /* --- 2. Can the caps be trusted at all? --------------------------------
     *
     * An sdrlink server that omits rx_rate leaves the range all zeros
     * (network_radio.cpp range_from_json), and clamping to that would pin every
     * app at 0 Hz. Hand back the caller's own ideal and say why. */
    const Range& r = caps.rx_rate;
    const bool caps_usable = std::isfinite(r.min) && std::isfinite(r.max) &&
                             r.min >= 0.0 && r.max > 0.0 && r.max >= r.min;
    if (!caps_usable) {
        out.rate_hz = ideal; /* zero only if the caller asked for nothing */
        out.outcome = RateOutcome::CapsUnusable;
        return out;
    }

    const double lo = (r.min > 0.0) ? r.min : 0.0;
    const double hi = r.max;

    /* --- 3. Clamp to what the device can physically do ---------------------- */
    double rate = std::min(std::max(ideal, lo), hi);

    /* --- 4. Onto the device's own rate grid, if it published one ------------ */
    rate = snap_to_step(rate, r);

    /* A caller that expressed no rate at all, on a device that published no
     * floor, leaves nothing to clamp. The ceiling is the only real number in
     * play, and returning zero is not an option. */
    if (rate <= 0.0) rate = hi;

    /* --- 5. Prefer an exact multiple ---------------------------------------
     *
     * Down first: it costs less CPU and cannot break the max_useful ceiling.
     * Up only if down would fall below the device floor or the app's own
     * minimum, and only while staying inside both ceilings. If the range admits
     * no multiple at all, the preference is dropped rather than forced. */
    if (multiple > 0.0) {
        /* The device bounds are hard and take no tolerance: a rate outside them
         * is a rate the radio does not have. The app's own bounds are soft to
         * within a hertz, which is not a difference any DSP here can feel. */
        const double soft_ceiling = (max_useful > 0.0) ? max_useful : hi;

        const double down = floor_multiple(rate, multiple);
        const double up = ceil_multiple(rate, multiple);

        /* Both candidates come out of a division by `multiple`, so a degenerate
         * multiple makes them degenerate: a denormal one sends the quotient to
         * infinity and a colossal one collapses it to zero or negative zero.
         * Neither is a rate, and neither was checked — a denormal
         * prefer_multiple_of_hz used to make this function return `inf` with
         * an outcome of "above ideal", i.e. usable() true, for the caller to
         * hand straight to set_rx_rate(). That matters because DeviceCaps is
         * built from whatever JSON an sdrlink server sends
         * (network_radio.cpp range_from_json), so the degenerate input is
         * remote, not merely hypothetical.
         *
         * So each candidate must be a real positive finite rate, must lie on
         * the correct side of where it started (a floor cannot rise, a ceiling
         * cannot fall), and must be inside the device's hard bounds. For every
         * sane multiple these are all true by construction, which is why no
         * real device profile moves. */
        if (positive_finite(down) && down <= rate && down >= lo &&
            down >= minimum - kEpsHz && on_step_grid(down, r)) {
            rate = down;
        } else if (positive_finite(up) && up >= rate && up >= lo && up <= hi &&
                   up <= soft_ceiling + kEpsHz && on_step_grid(up, r)) {
            rate = up;
        }
    }

    /* --- 6. Classify -------------------------------------------------------- */
    out.rate_hz = rate;
    if (minimum > 0.0 && rate < minimum - kEpsHz) {
        out.outcome = RateOutcome::BelowMinimum;
    } else if (rate < ideal - kEpsHz) {
        out.outcome = RateOutcome::Reduced;
    } else if (rate > ideal + kEpsHz) {
        out.outcome = RateOutcome::Raised;
    } else {
        out.outcome = RateOutcome::Ideal;
    }
    return out;
}

double choose_rx_rate_hz(const DeviceCaps& caps, const RatePreference& want) {
    return choose_rx_rate(caps, want).rate_hz;
}

}  // namespace radio
