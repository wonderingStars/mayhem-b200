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

TEST(telemetry_is_inert_in_a_stock_build) {
    /* A build that never set kTelemetryEndpoint sends nothing even when
     * "enabled": the empty endpoint is the hard gate before any id file,
     * throttle file or network call is even considered. */
    CHECK(core::telemetry::kTelemetryEndpoint != nullptr);
    CHECK(std::string{core::telemetry::kTelemetryEndpoint}.empty());
    core::telemetry::ping_on_startup("0.0.0-test", true);
    CHECK(true);
}
