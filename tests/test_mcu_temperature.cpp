/*
 * mayhem-b200 — MCU Temperature N/A-screen tests.
 *
 * MCU Temperature is a hardware-limited app: a B200 host has no LPC43xx die
 * temperature sensor, so the view deliberately produces no reading. The only
 * pure logic to check is that the on-screen report is honest — it states the
 * capability is unavailable and never carries a temperature value.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_mcu_temperature.hpp"

#include <string>
#include <vector>

using namespace app;
using namespace mb200test;

namespace {

bool any_line_contains(const std::vector<std::string>& lines, const std::string& needle) {
    for (const auto& l : lines)
        if (l.find(needle) != std::string::npos) return true;
    return false;
}

/* A digit followed by a degree marker would be a fake reading; assert none
 * exists. The report may contain plain numbers ("43xx", "once a second"), so we
 * specifically look for a number adjacent to C/°/"degrees". */
bool has_fake_temperature(const std::vector<std::string>& lines) {
    for (const auto& l : lines) {
        for (size_t i = 0; i + 1 < l.size(); ++i) {
            const bool digit = (l[i] >= '0' && l[i] <= '9');
            if (!digit) continue;
            const char n = l[i + 1];
            if (n == 'C' || n == 'F' || n == 0x1F /* STR_DEGREES marker */)
                return true;
        }
        if (l.find("degrees") != std::string::npos) return true;
    }
    return false;
}

}  // namespace

TEST(mcutemp_never_available) {
    CHECK(!McuTemperatureView::temperature_available());
}

TEST(mcutemp_report_states_unavailable) {
    const auto lines = McuTemperatureView::status_report();
    CHECK(!lines.empty());
    /* Explains what it did on a PortaPack ... */
    CHECK(any_line_contains(lines, "LPC43xx"));
    /* ... and that a B200 host cannot provide it. */
    CHECK(any_line_contains(lines, "USB SDR"));
    CHECK(any_line_contains(lines, "No MCU temperature to report."));
}

TEST(mcutemp_report_carries_no_reading) {
    const auto lines = McuTemperatureView::status_report();
    CHECK(!has_fake_temperature(lines));
}
