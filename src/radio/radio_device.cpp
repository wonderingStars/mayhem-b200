/*
 * mayhem-b200 — shared radio types.
 *
 * Definitions for the small value types every backend shares. Kept separate
 * from usrp_radio.cpp so a backend that is not UHD (the network client, for
 * instance) does not have to drag the UHD implementation in to link.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "radio_device.hpp"

#include <algorithm>

namespace radio {

double Range::clamp(double v) const {
    /* An unpopulated range must not clamp everything to zero — that would
     * silently pin the tuner to DC when a capability read fails. */
    if (max <= min) return v;
    return std::min(std::max(v, min), max);
}

std::string DeviceInfo::label() const {
    std::string s = product.empty() ? type : product;
    if (s.empty()) s = "SDR";
    if (!serial.empty()) s += " " + serial;
    return s;
}

void StreamStats::reset() {
    rx_samples.store(0);
    tx_samples.store(0);
    overflows.store(0);
    underflows.store(0);
    rx_dropped.store(0);
    timeouts.store(0);
    errors.store(0);
}

}  // namespace radio
