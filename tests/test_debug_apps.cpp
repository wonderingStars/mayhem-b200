/*
 * mayhem-b200 — Debug-category app tests (Ext Sensors, Debug PMem).
 *
 * Two things worth checking without a screen:
 *
 *   (1) Debug PMem's settings-dump formatting: a known core::Settings content
 *       must produce an exact, deterministic dump — section headers, "key: value"
 *       lines, and the empty-store fallback. Expected text comes from the
 *       contract in ui_debug_pmem.hpp (a transform of core::Settings::to_string
 *       output), not from whatever the code happens to emit.
 *
 *   (2) Ext Sensors is a PortaPack-only app with no B200 equivalent: it must
 *       report that sensor data is unavailable and must NOT print a fabricated
 *       reading (no temperature/pressure/lux value).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"


#include "ui_debug_pmem.hpp"
#include "ui_extsensors.hpp"

#include "settings.hpp"

#include <filesystem>
#include <string>

using app::DebugPMemView;
using app::ExtSensorsView;
using core::Settings;

namespace {

std::string temp_path(const char* name) {
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) dir = std::filesystem::current_path();
    /* PID in the name: this path lives in the SHARED system temp dir, and a
     * fixed name is the same file in every concurrently-running test process
     * (one per git worktree is normal here). Concurrent suites then delete or
     * overwrite each other's fixtures mid-test -- observed 2026-08-13 as
     * intermittent read-back failures with the binary md5-pinned. */
    return (dir / (std::to_string(mb200test::test_pid()) + "_" + name)).string();
}

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

/* --- Debug PMem: settings-dump formatting ----------------------------- */

TEST(debug_pmem_dump_formats_known_settings_exactly) {
    Settings s{temp_path("mb200_unused.ini")};
    /* An orphan key (unnamed section), then two named sections. Insertion order
     * is preserved, which is what makes the expected text deterministic. */
    s.from_string(
        "orphan=1\r\n"
        "[audio]\r\n"
        "volume=42\r\n"
        "device=Speakers (Realtek)\r\n"
        "[pmem]\r\n"
        "target_frequency=433920000\r\n");

    const std::string expected =
        "  orphan: 1\n"
        "[audio]\n"
        "  volume: 42\n"
        "  device: Speakers (Realtek)\n"
        "[pmem]\n"
        "  target_frequency: 433920000\n";

    CHECK_STR_EQ(DebugPMemView::format_settings_dump(s), expected);
}

TEST(debug_pmem_dump_reports_empty_store) {
    Settings s{temp_path("mb200_unused.ini")};
    s.from_string("");  /* deterministically empty, whatever the ctor did */

    CHECK_STR_EQ(DebugPMemView::format_settings_dump(s), "(no settings stored)\n");
}

TEST(debug_pmem_dump_named_section_only) {
    Settings s{temp_path("mb200_unused.ini")};
    s.from_string(
        "[pmem]\r\n"
        "correction_ppb=0\r\n"
        "config_splash=1\r\n");

    /* No unnamed keys, so the dump opens with the header. A stored zero is shown
     * as "0", not dropped — the dump reflects presence, like the store does. */
    const std::string expected =
        "[pmem]\n"
        "  correction_ppb: 0\n"
        "  config_splash: 1\n";

    CHECK_STR_EQ(DebugPMemView::format_settings_dump(s), expected);
}

TEST(debug_pmem_dump_round_trips_live_style_pmem) {
    /* Build a store the way core::pmem does (values in [pmem]) and confirm the
     * dump renders each key it wrote. */
    Settings s{temp_path("mb200_unused.ini")};
    s.set_int("pmem", "correction_ppb", -1500);
    s.set_uint("pmem", "target_frequency", 100'000'000);
    s.set_bool("pmem", "config_splash", false);

    const std::string dump = DebugPMemView::format_settings_dump(s);
    CHECK(contains(dump, "[pmem]\n"));
    CHECK(contains(dump, "  correction_ppb: -1500\n"));
    CHECK(contains(dump, "  target_frequency: 100000000\n"));
    CHECK(contains(dump, "  config_splash: 0\n"));
}

/* --- Ext Sensors: honest N/A screen ----------------------------------- */

TEST(extsensors_reports_unavailable_not_a_value) {
    /* A B200 host can never have sensor data. */
    CHECK_EQ(ExtSensorsView::sensors_available(), false);

    /* Join the on-screen lines and check the result is an unavailability
     * message, not a reading. */
    std::string joined;
    for (const auto& line : ExtSensorsView::status_report())
        joined += line + "\n";

    CHECK(contains(joined, "unavailable"));

    /* No fabricated sensor readings: none of the value markers the real
     * PortaPack app printed (temperature, humidity, pressure, lux) may appear
     * as an actual measurement. The app describes what those WERE, so allow the
     * bare category labels but forbid the value syntax that a reading uses. */
    CHECK(!contains(joined, " hPa"));       /* pressure reading */
    CHECK(!contains(joined, " LUX ("));     /* upstream lux line: "L: N LUX" */
    CHECK(!contains(joined, "T: "));        /* upstream temp line: "T: NN.NN" */
    CHECK(!contains(joined, "H: "));        /* upstream humidity: "H: NN.N%" */
    CHECK(!contains(joined, "P: "));        /* upstream pressure: "P: NN.N hPa" */

    /* The report is non-empty and speaks to sensors. */
    CHECK(!joined.empty());
    CHECK(contains(joined, "sensor"));
}

TEST(extsensors_report_is_stable) {
    /* No hardware, no clock: the report is fixed, so two reads match. This is
     * what lets the previous test's negative checks be meaningful. */
    const auto a = ExtSensorsView::status_report();
    const auto b = ExtSensorsView::status_report();
    CHECK_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        CHECK_STR_EQ(a[i], b[i]);
}
