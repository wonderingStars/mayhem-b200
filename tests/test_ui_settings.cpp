/*
 * mayhem-b200 — settings-screen tests.
 *
 * The settings views are thin wrappers over core::pmem, so the logic worth
 * testing without a screen is: (1) the Display page's theme-option -> ThemeId
 * mapping, which must match upstream SetThemeView's `selected < MAX` guard, and
 * (2) that each value a page writes survives a real core::Settings disk
 * save/load round trip — including a stored value that equals the type's zero,
 * which must read back as the stored zero and not silently revert to the
 * firmware default.
 *
 * Expected values come from ui_settings.hpp, theme.hpp (the ThemeId enum), and
 * core/settings.cpp's documented [pmem] defaults — not from whatever the code
 * happens to emit.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include <process.h> /* _getpid: shared-temp names must be unique PER PROCESS */

#include "ui_settings.hpp"

#include "settings.hpp"
#include "theme.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>

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
    return (dir / (std::to_string(_getpid()) + "_" + name)).string();
}

/* Points the process-wide store at a scratch file for the duration of a test
 * and puts the real path back afterwards, so a persistence test can drive the
 * genuine core::pmem code path without clobbering the user's settings.ini. */
struct GlobalSettingsRedirect {
    std::string original;

    explicit GlobalSettingsRedirect(std::string scratch)
        : original{core::settings().path()} {
        std::error_code ec;
        std::filesystem::remove(scratch, ec);
        core::settings().set_path(std::move(scratch));
        core::settings().clear();
    }

    ~GlobalSettingsRedirect() {
        const std::string scratch = core::settings().path();
        core::settings().set_path(original);
        core::settings().load();  // false on first run; an empty store is correct then
        std::error_code ec;
        std::filesystem::remove(scratch, ec);
    }

    GlobalSettingsRedirect(const GlobalSettingsRedirect&) = delete;
    GlobalSettingsRedirect& operator=(const GlobalSettingsRedirect&) = delete;
};

/* A real disk round trip: flush the in-memory store to its file, then reload it
 * from that file into a cleared store. What survives is what persisted. */
void save_and_reload() {
    CHECK(core::pmem::save());
    CHECK(core::settings().load());
}

}  // namespace

/* --- Display: theme option -> ThemeId ---------------------------------- */

TEST(settings_theme_option_maps_to_each_theme_id) {
    using ui::Theme;
    Theme::ThemeId id{};

    struct Case {
        int32_t option;
        Theme::ThemeId expected;
    };
    const Case cases[] = {
        {0, Theme::DefaultGrey},
        {1, Theme::Yellow},
        {2, Theme::Aqua},
        {3, Theme::Green},
        {4, Theme::Red},
        {5, Theme::Dark},
    };

    for (const auto& c : cases) {
        id = Theme::Red;  // seed with something other than the expected answer
        CHECK(app::settings_theme_id_from_option(c.option, id));
        CHECK_EQ(static_cast<int>(id), static_cast<int>(c.expected));
    }

    /* The zero option is a valid, distinct theme (DefaultGrey), not a rejected
     * "unset" — the same property the persistence tests lean on. */
    CHECK(app::settings_theme_id_from_option(0, id));
    CHECK_EQ(static_cast<int>(id), static_cast<int>(Theme::DefaultGrey));
}

TEST(settings_theme_option_rejects_out_of_range) {
    using ui::Theme;
    Theme::ThemeId id = Theme::Aqua;  // must be left untouched on rejection

    /* MAX itself and beyond are invalid, matching `selected < MAX`. */
    CHECK(!app::settings_theme_id_from_option(Theme::MAX, id));
    CHECK(!app::settings_theme_id_from_option(Theme::MAX + 1, id));
    CHECK(!app::settings_theme_id_from_option(6, id));
    CHECK(!app::settings_theme_id_from_option(-1, id));
    CHECK(!app::settings_theme_id_from_option(1000, id));

    /* A rejected option leaves the caller's variable alone. */
    CHECK_EQ(static_cast<int>(id), static_cast<int>(Theme::Aqua));
}

/* --- Persistence through core::Settings save/load ---------------------- */

TEST(settings_radio_audio_display_persist_through_disk) {
    GlobalSettingsRedirect redirect{temp_path("mb200_test_screens.ini")};
    core::pmem::defaults();

    /* Radio */
    core::pmem::set_correction_ppb(-1500);
    core::pmem::set_config_converter(true);
    core::pmem::set_config_updown_converter(true);
    core::pmem::set_config_converter_freq(125'000'000);
    /* Audio */
    core::pmem::set_headphone_volume(72);
    core::pmem::set_beep_on_packets(true);
    /* Display */
    core::pmem::set_ui_theme_id(ui::Theme::Green);

    save_and_reload();

    CHECK_EQ(core::pmem::correction_ppb(), -1500);
    CHECK_EQ(core::pmem::config_converter(), true);
    CHECK_EQ(core::pmem::config_updown_converter(), true);
    CHECK_EQ(core::pmem::config_converter_freq(), static_cast<int64_t>(125'000'000));
    CHECK_EQ(static_cast<int>(core::pmem::headphone_volume()), 72);
    CHECK_EQ(core::pmem::beep_on_packets(), true);
    CHECK_EQ(static_cast<int>(core::pmem::ui_theme_id()),
             static_cast<int>(ui::Theme::Green));
}

TEST(settings_zero_values_persist_and_are_not_the_default) {
    GlobalSettingsRedirect redirect{temp_path("mb200_test_screens_zero.ini")};
    core::pmem::defaults();

    /* Volume's firmware default is 40, so a stored 0 that reads back as 0 proves
     * the zero was persisted rather than falling through to the default. */
    core::pmem::set_headphone_volume(0);
    save_and_reload();
    CHECK_EQ(static_cast<int>(core::pmem::headphone_volume()), 0);

    /* correction_ppb, converter freq and theme all default to 0, so proving the
     * stored zero is distinct from the default needs the non-zero -> 0
     * transition to survive a reload. */
    core::pmem::set_correction_ppb(4200);
    core::pmem::set_config_converter_freq(9'000'000);
    core::pmem::set_ui_theme_id(ui::Theme::Dark);
    save_and_reload();
    CHECK_EQ(core::pmem::correction_ppb(), 4200);
    CHECK_EQ(core::pmem::config_converter_freq(), static_cast<int64_t>(9'000'000));
    CHECK_EQ(static_cast<int>(core::pmem::ui_theme_id()),
             static_cast<int>(ui::Theme::Dark));

    core::pmem::set_correction_ppb(0);
    core::pmem::set_config_converter_freq(0);
    core::pmem::set_ui_theme_id(0);
    core::pmem::set_config_converter(false);
    save_and_reload();

    CHECK_EQ(core::pmem::correction_ppb(), 0);
    CHECK_EQ(core::pmem::config_converter_freq(), static_cast<int64_t>(0));
    CHECK_EQ(static_cast<int>(core::pmem::ui_theme_id()), 0);
    CHECK_EQ(core::pmem::config_converter(), false);

    /* The zero is genuinely on disk: the key is present in the [pmem] section,
     * so a reader distinguishes "stored 0" from "absent, use default". */
    CHECK(core::settings().has("pmem", "correction_ppb"));
    CHECK(core::settings().has("pmem", "ui_theme_id"));
    CHECK(core::settings().has("pmem", "config_converter_freq"));
}

TEST(settings_reset_restores_firmware_defaults) {
    GlobalSettingsRedirect redirect{temp_path("mb200_test_screens_reset.ini")};

    /* Move everything off its default... */
    core::pmem::set_correction_ppb(9999);
    core::pmem::set_config_converter(true);
    core::pmem::set_config_converter_freq(50'000'000);
    core::pmem::set_headphone_volume(11);
    core::pmem::set_beep_on_packets(true);
    core::pmem::set_ui_theme_id(ui::Theme::Red);
    CHECK(core::pmem::save());

    /* ...then do what the "Reset Settings" tile does. */
    core::pmem::defaults();

    /* Firmware defaults, from core/settings.cpp. */
    CHECK_EQ(core::pmem::correction_ppb(), core::pmem::ppb_reset_value);
    CHECK_EQ(core::pmem::config_converter(), false);
    CHECK_EQ(core::pmem::config_converter_freq(), static_cast<int64_t>(0));
    CHECK_EQ(static_cast<int>(core::pmem::headphone_volume()), 40);
    CHECK_EQ(core::pmem::beep_on_packets(), false);
    CHECK_EQ(static_cast<int>(core::pmem::ui_theme_id()),
             static_cast<int>(ui::Theme::DefaultGrey));

    /* And the reset is durable across a reload. */
    CHECK(core::pmem::save());
    CHECK(core::settings().load());
    CHECK_EQ(static_cast<int>(core::pmem::headphone_volume()), 40);
    CHECK_EQ(core::pmem::correction_ppb(), core::pmem::ppb_reset_value);
}
