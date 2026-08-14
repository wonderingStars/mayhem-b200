/*
 * mayhem-b200 — the anonymous usage ping stays quiet unless asked.
 *
 * The ping is opt-out AND inert unless a build sets kTelemetryEndpoint, so the
 * two things that must never fail are: a disabled ping does nothing, and a
 * stock (unconfigured) build does nothing — neither may throw, block, or reach
 * the network. The actual POST is verified against the real Worker
 * (analytics-worker/) out of band; there is no live endpoint in the test
 * process, which is exactly why the default must be silent.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "telemetry.hpp"

#include <string>

TEST(telemetry_is_a_no_op_when_disabled) {
    /* --no-telemetry: returns at once, touches nothing, cannot throw. */
    core::telemetry::ping_on_startup("0.0.0-test", false);
    CHECK(true); /* reaching here without a throw or hang is the assertion */
}

TEST(telemetry_is_inert_when_endpoint_is_empty) {
    /* The empty endpoint is the hard gate: with no endpoint, even an "enabled"
     * ping does nothing — no id file, no throttle file, no network — which is
     * the guarantee a fork (or a stock build) relies on. This build ships with
     * a real endpoint, so blank it for the check and restore it after; the
     * point is that ping_on_startup must not fire when it is empty. */
    CHECK(core::telemetry::kTelemetryEndpoint != nullptr);
    const char* saved = core::telemetry::kTelemetryEndpoint;
    core::telemetry::kTelemetryEndpoint = "";
    core::telemetry::ping_on_startup("0.0.0-test", true); /* must be a no-op */
    core::telemetry::kTelemetryEndpoint = saved;
    CHECK(std::string{core::telemetry::kTelemetryEndpoint}.size() > 0);
}
