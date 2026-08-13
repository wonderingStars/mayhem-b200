/*
 * mayhem-b200 — capability-driven front end: analog bandwidth and gain.
 *
 * See capability_policy.hpp for what this is for and why each decision was
 * made. Both functions below are deliberately straight-line: validate the
 * request, decide whether the caps can be trusted, clamp, classify. Every
 * branch is pinned by a case in tests/test_capability_policy.cpp.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "capability_policy.hpp"

#include <algorithm>
#include <cmath>

namespace radio {

namespace {

/* Classification tolerance for bandwidth, in Hz. A filter corner a fraction of
 * a hertz off the request is not a device falling short, and the backends round
 * the value anyway. */
constexpr double kBwEpsHz = 1.0;

/* Classification tolerance for gain, in dB. Device gain grids are 0.2 dB at the
 * finest here (a B200's TX), so a thousandth of a dB is well inside the noise
 * and stops a floating-point residue reading as a clamp. */
constexpr double kGainEpsDb = 1e-3;

/* A range whose bounds are real numbers in the right order and whose ceiling is
 * a positive frequency. All zeros — what an sdrlink server that omits the field
 * leaves behind — is "unknown", not "zero Hz". */
bool frequency_range_usable(const Range& r) {
    return std::isfinite(r.min) && std::isfinite(r.max) && r.min >= 0.0 && r.max > 0.0 &&
           r.max >= r.min;
}

/* Gain ranges get a weaker test than frequency ranges on purpose: gain may be
 * negative (attenuation) on a perfectly ordinary dongle, so only finiteness and
 * ordering can be checked. max > min matches Range::clamp's own convention for
 * "this range says nothing". */
bool gain_range_usable(const Range& r) {
    return std::isfinite(r.min) && std::isfinite(r.max) && r.max > r.min;
}

}  // namespace

/* --- Analog RX bandwidth --------------------------------------------------- */

const char* bandwidth_outcome_to_string(BandwidthOutcome outcome) {
    switch (outcome) {
        case BandwidthOutcome::Matched:      return "follows rate";
        case BandwidthOutcome::Widened:      return "wider than rate";
        case BandwidthOutcome::Narrowed:     return "narrower than rate";
        case BandwidthOutcome::NoRate:       return "no rate";
        case BandwidthOutcome::CapsUnusable: return "caps unusable";
    }
    return "unknown";
}

BandwidthChoice choose_rx_bandwidth(const DeviceCaps& caps, double rate_hz) {
    BandwidthChoice out;

    /* --- 1. Is there a rate to follow at all? -------------------------------
     *
     * ReceiverModel reads its rate back from the radio, so this is whatever the
     * hardware returned — including 0.0 from a device that is not open, and in
     * principle a NaN from a backend that read garbage. Neither is a rate, and
     * deriving a filter width from either would be inventing one. */
    if (!std::isfinite(rate_hz) || rate_hz <= 0.0) {
        out.outcome = BandwidthOutcome::NoRate;
        return out;
    }

    /* --- 2. Can the caps be trusted? ----------------------------------------
     *
     * With no usable range there is no safe value to send, so the analog filter
     * is left exactly as the device set it and the caller is told why. */
    const Range& r = caps.rx_bandwidth;
    if (!frequency_range_usable(r)) {
        out.outcome = BandwidthOutcome::CapsUnusable;
        return out;
    }

    /* --- 3. The width the rate wants ---------------------------------------- */
    const double want = rate_hz * kRxAnalogBandwidthRatio;

    /* --- 4. Clamp to what the device's filter can physically do -------------- */
    const double lo = (r.min > 0.0) ? r.min : 0.0;
    const double hi = r.max;
    const double bw = std::min(std::max(want, lo), hi);

    out.bandwidth_hz = bw;

    /* --- 5. Classify --------------------------------------------------------
     *
     * Against `want`, not against the rate: the caller needs to know whether
     * the analog filter ended up wider than the captured band (folding) or
     * narrower (rolled-off edges), and `want` is where the boundary sits. */
    if (bw > want + kBwEpsHz) {
        out.outcome = BandwidthOutcome::Widened;
    } else if (bw < want - kBwEpsHz) {
        out.outcome = BandwidthOutcome::Narrowed;
    } else {
        out.outcome = BandwidthOutcome::Matched;
    }
    return out;
}

/* --- Gain ------------------------------------------------------------------ */

const char* gain_outcome_to_string(GainOutcome outcome) {
    switch (outcome) {
        case GainOutcome::Applied:     return "applied";
        case GainOutcome::ClampedLow:  return "at device minimum";
        case GainOutcome::ClampedHigh: return "at device maximum";
        case GainOutcome::Unvalidated: return "caps unusable";
        case GainOutcome::Invalid:     return "not a number";
    }
    return "unknown";
}

GainChoice choose_gain(const Range& range, double requested_db) {
    GainChoice out;

    /* --- 1. Is the request a number? ----------------------------------------
     *
     * This is the branch that matters most. A NaN reaching NetworkRadio with
     * the link up is formatted into the control message as the bare token
     * `nan` (network_radio.cpp format_json_number falls through to "%.17g"),
     * which is not JSON and which the far side cannot parse. There is no
     * sensible gain to substitute for a NaN, so nothing is written. */
    if (!std::isfinite(requested_db)) {
        out.outcome = GainOutcome::Invalid;
        return out;
    }

    /* --- 2. Can the range be trusted? ---------------------------------------
     *
     * Unlike the bandwidth above, an unusable range does not stop the write.
     * A radio left at whatever gain it happened to have is deaf or saturated,
     * which is a worse failure than one clamped by the backend a moment later;
     * every backend clamps against its own caps as well. */
    if (!gain_range_usable(range)) {
        out.gain_db = requested_db;
        out.outcome = GainOutcome::Unvalidated;
        return out;
    }

    /* --- 3. Clamp and classify ---------------------------------------------- */
    if (requested_db < range.min - kGainEpsDb) {
        out.gain_db = range.min;
        out.outcome = GainOutcome::ClampedLow;
    } else if (requested_db > range.max + kGainEpsDb) {
        out.gain_db = range.max;
        out.outcome = GainOutcome::ClampedHigh;
    } else {
        /* Inside the range to within the tolerance. Clamp anyway, so a request
         * a hundredth of a dB past the ceiling is still a value the device
         * published rather than one it merely tolerates. */
        out.gain_db = std::min(std::max(requested_db, range.min), range.max);
        out.outcome = GainOutcome::Applied;
    }
    return out;
}

GainChoice choose_rx_gain(const DeviceCaps& caps, double requested_db) {
    return choose_gain(caps.rx_gain, requested_db);
}

GainChoice choose_tx_gain(const DeviceCaps& caps, double requested_db) {
    return choose_gain(caps.tx_gain, requested_db);
}

}  // namespace radio
