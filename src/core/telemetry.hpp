/*
 * mayhem-b200 — anonymous usage ping.
 *
 * Counts distinct installs so the maintainer can see how many people run the
 * software. It is deliberately minimal and honest:
 *
 *   - The ONLY identifier is a random id this install generated once and
 *     stored locally (<data>/telemetry_id). It maps to no person, account,
 *     email, hostname or location. Nothing else about the machine is sent.
 *   - The payload is exactly {id, version, os}. No IP is sent (the server is
 *     told not to log one either).
 *   - It fires at most once per calendar day (a local throttle), on a detached
 *     thread with a short timeout, and can never block or crash startup.
 *   - It is OPT-OUT: --no-telemetry (or the config flag) disables it, and it
 *     is inert unless the maintainer has set kTelemetryEndpoint to their own
 *     deployed Worker URL (see analytics-worker/). A stock build with the
 *     endpoint unset sends nothing.
 *
 * The matching server is analytics-worker/ (a Cloudflare Worker).
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MB200_CORE_TELEMETRY_HPP
#define MB200_CORE_TELEMETRY_HPP

#include <string>

namespace core::telemetry {

/* The maintainer's deployed usage-counter Worker, e.g.
 * "https://mayhem-b200-usage.example.workers.dev/ping". EMPTY by default: a
 * build that has not set this sends nothing, whatever the flags say. */
extern const char* kTelemetryEndpoint;

/* Fire the once-a-day anonymous ping, unless disabled. Returns immediately;
 * the network call runs detached and fail-silent. `enabled` is the opt-out
 * flag (false => do nothing). Safe to call once at startup. */
void ping_on_startup(const std::string& version, bool enabled);

}  // namespace core::telemetry

#endif  // MB200_CORE_TELEMETRY_HPP
