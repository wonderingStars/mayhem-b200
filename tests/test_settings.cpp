/*
 * mayhem-b200 — persistent settings tests.
 *
 * The interesting cases are the ones that bite in practice: a stored zero must
 * not read back as "absent", a junk line must not take the rest of the file
 * with it, and an app must come back up on the frequency and gain it was last
 * used with. Expected values come from the format defined in settings.hpp and
 * from the firmware's application/app_settings.cpp behaviour it mirrors.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"


#include "settings.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>

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

/* Deletes its file on the way in and on the way out, so a crashed run does not
 * poison the next one. */
struct ScopedFile {
    std::string path;

    explicit ScopedFile(const std::string& p)
        : path{p} { remove(); }
    ~ScopedFile() { remove(); }

    ScopedFile(const ScopedFile&) = delete;
    ScopedFile& operator=(const ScopedFile&) = delete;

    void remove() const {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    bool exists() const {
        std::error_code ec;
        return std::filesystem::exists(path, ec);
    }
};

bool contains(const std::string& haystack, const std::string& needle) {
    return haystack.find(needle) != std::string::npos;
}

}  // namespace

/* --- Parsing ---------------------------------------------------------- */

TEST(settings_parses_sections_and_keys) {
    Settings s{temp_path("mb200_unused.ini")};
    s.from_string(
        "orphan=1\r\n"
        "[audio]\r\n"
        "volume=42\r\n"
        "device=Speakers (Realtek)\r\n"
        "[radio]\r\n"
        "frequency=433920000\r\n");

    /* A key before the first header lands in the unnamed section. */
    CHECK_EQ(s.get_int("", "orphan", -1), static_cast<int64_t>(1));

    CHECK_EQ(s.get_int("audio", "volume", -1), static_cast<int64_t>(42));
    CHECK_STR_EQ(s.get_string("audio", "device", "?"), "Speakers (Realtek)");
    CHECK_EQ(s.get_uint("radio", "frequency", 0), static_cast<uint64_t>(433920000));

    const auto names = s.sections();
    CHECK_EQ(names.size(), static_cast<size_t>(3));
    CHECK_STR_EQ(names[0], "");
    CHECK_STR_EQ(names[1], "audio");
    CHECK_STR_EQ(names[2], "radio");

    const auto audio_keys = s.keys("audio");
    CHECK_EQ(audio_keys.size(), static_cast<size_t>(2));
    CHECK_STR_EQ(audio_keys[0], "volume");
}

TEST(settings_tolerates_malformed_lines) {
    Settings s{temp_path("mb200_unused.ini")};
    s.from_string(
        "[audio]\n"
        "volume=42\n"
        "this line has no equals sign\n"
        "   \n"
        "; a comment = with an equals\n"
        "# another comment\n"
        "=value with no key\n"
        "[unclosed section\n"
        "mode=stereo\n");

    /* Both good keys survived, and the unclosed header did not switch us into
     * some phantom section. */
    CHECK_EQ(s.get_int("audio", "volume", -1), static_cast<int64_t>(42));
    CHECK_STR_EQ(s.get_string("audio", "mode", "?"), "stereo");

    /* Neither the comment nor the keyless line became a key. */
    CHECK(!s.has("audio", "; a comment "));
    CHECK(!s.has("audio", ""));
    CHECK_EQ(s.keys("audio").size(), static_cast<size_t>(2));
    CHECK_EQ(s.sections().size(), static_cast<size_t>(1));
}

TEST(settings_trims_whitespace_around_keys_and_values) {
    Settings s{temp_path("mb200_unused.ini")};
    s.from_string("  [ radio ]  \n   frequency   =   433920000   \n");

    CHECK(s.has("radio", "frequency"));
    CHECK_EQ(s.get_uint("radio", "frequency", 0), static_cast<uint64_t>(433920000));
}

/* --- Typed access ----------------------------------------------------- */

TEST(settings_defaults_when_absent) {
    Settings s{temp_path("mb200_unused.ini")};

    CHECK(!s.has("nope", "nope"));
    CHECK_EQ(s.get_bool("nope", "nope", true), true);
    CHECK_EQ(s.get_int("nope", "nope", -7), static_cast<int64_t>(-7));
    CHECK_EQ(s.get_uint("nope", "nope", 7), static_cast<uint64_t>(7));
    CHECK_NEAR(s.get_float("nope", "nope", 1.5f), 1.5, 1e-9);
    CHECK_STR_EQ(s.get_string("nope", "nope", "fallback"), "fallback");
}

TEST(settings_bool_spellings) {
    Settings s{temp_path("mb200_unused.ini")};
    s.from_string(
        "[b]\n"
        "one=1\nzero=0\ntrue=true\nfalse=False\nyes=YES\nno=no\non=on\noff=off\n"
        "numeric=2\njunk=maybe\n");

    CHECK_EQ(s.get_bool("b", "one", false), true);
    CHECK_EQ(s.get_bool("b", "zero", true), false);
    CHECK_EQ(s.get_bool("b", "true", false), true);
    CHECK_EQ(s.get_bool("b", "false", true), false);
    CHECK_EQ(s.get_bool("b", "yes", false), true);
    CHECK_EQ(s.get_bool("b", "no", true), false);
    CHECK_EQ(s.get_bool("b", "on", false), true);
    CHECK_EQ(s.get_bool("b", "off", true), false);

    /* The firmware's rule: any non-zero number is true. */
    CHECK_EQ(s.get_bool("b", "numeric", false), true);

    /* Unparseable text falls back to the caller's default rather than
     * guessing. */
    CHECK_EQ(s.get_bool("b", "junk", true), true);
    CHECK_EQ(s.get_bool("b", "junk", false), false);
}

TEST(settings_rejects_unparseable_numbers) {
    Settings s{temp_path("mb200_unused.ini")};
    s.from_string(
        "[n]\n"
        "trailing=123abc\n"
        "negative_uint=-1\n"
        "empty=\n"
        "huge=99999999999999999999999999\n");

    CHECK_EQ(s.get_int("n", "trailing", -1), static_cast<int64_t>(-1));
    /* strtoull would turn "-1" into 18446744073709551615; that is never what a
     * settings file meant. */
    CHECK_EQ(s.get_uint("n", "negative_uint", 5), static_cast<uint64_t>(5));
    CHECK_EQ(s.get_int("n", "empty", 9), static_cast<int64_t>(9));
    CHECK_EQ(s.get_uint("n", "huge", 3), static_cast<uint64_t>(3));

    /* Present but unparseable is still present. */
    CHECK(s.has("n", "trailing"));
    CHECK_STR_EQ(s.get_raw("n", "trailing"), "123abc");
}

TEST(settings_zero_is_not_absent) {
    ScopedFile file{temp_path("mb200_test_zero.ini")};

    {
        Settings s{file.path};
        s.set_bool("z", "flag", false);
        s.set_int("z", "offset", 0);
        s.set_uint("z", "frequency", 0);
        s.set_float("z", "gain", 0.0f);
        s.set_string("z", "label", "");
        CHECK(s.save());
    }

    CHECK(file.exists());

    Settings t{file.path};
    CHECK(t.load());

    /* Every one of these is present, so the caller's default must lose. */
    CHECK(t.has("z", "flag"));
    CHECK(t.has("z", "offset"));
    CHECK(t.has("z", "frequency"));
    CHECK(t.has("z", "gain"));
    CHECK(t.has("z", "label"));

    CHECK_EQ(t.get_bool("z", "flag", true), false);
    CHECK_EQ(t.get_int("z", "offset", 42), static_cast<int64_t>(0));
    CHECK_EQ(t.get_uint("z", "frequency", 42), static_cast<uint64_t>(0));
    CHECK_NEAR(t.get_float("z", "gain", 42.0f), 0.0, 1e-9);
    CHECK_STR_EQ(t.get_string("z", "label", "fallback"), "");

    /* ...and an absent key in the same section still gets the default. */
    CHECK(!t.has("z", "missing"));
    CHECK_EQ(t.get_int("z", "missing", 42), static_cast<int64_t>(42));
}

TEST(settings_round_trips_every_type) {
    ScopedFile file{temp_path("mb200_test_types.ini")};

    {
        Settings s{file.path};
        s.set_bool("app", "enabled", true);
        s.set_int("app", "trim_ppb", -1234567);
        s.set_uint("app", "frequency", 6'000'000'000ull);  /* > 32 bits */
        s.set_float("app", "gain_db", 37.25f);
        s.set_float("app", "odd", 3.14159265f);
        s.set_string("app", "name", "Analog Audio");
        s.set_string("other", "name", "second section");
        CHECK(s.dirty());
        CHECK(s.save());
        CHECK(!s.dirty());
    }

    Settings t{file.path};
    CHECK(t.load());
    CHECK(!t.dirty());

    CHECK_EQ(t.get_bool("app", "enabled", false), true);
    CHECK_EQ(t.get_int("app", "trim_ppb", 0), static_cast<int64_t>(-1234567));
    CHECK_EQ(t.get_uint("app", "frequency", 0), static_cast<uint64_t>(6'000'000'000ull));
    /* %.9g recovers a float bit-for-bit, so exact equality is the right test. */
    CHECK(t.get_float("app", "gain_db", 0.0f) == 37.25f);
    CHECK(t.get_float("app", "odd", 0.0f) == 3.14159265f);
    CHECK_STR_EQ(t.get_string("app", "name", ""), "Analog Audio");
    CHECK_STR_EQ(t.get_string("other", "name", ""), "second section");

    /* Sections are separate namespaces. */
    CHECK(!t.has("other", "enabled"));
}

TEST(settings_writes_readable_ini) {
    Settings s{temp_path("mb200_unused.ini")};
    s.set_int("audio", "volume", 42);
    s.set_bool("audio", "muted", false);

    const auto text = s.to_string();
    CHECK(contains(text, "[audio]"));
    CHECK(contains(text, "volume=42"));
    CHECK(contains(text, "muted=0"));
    CHECK(contains(text, "\r\n"));

    /* And it reads back into an equivalent store. */
    Settings t{temp_path("mb200_unused.ini")};
    t.from_string(text);
    CHECK_EQ(t.get_int("audio", "volume", 0), static_cast<int64_t>(42));
    CHECK_EQ(t.get_bool("audio", "muted", true), false);
}

TEST(settings_strips_newlines_from_strings) {
    Settings s{temp_path("mb200_unused.ini")};
    s.set_string("app", "note", "line one\nline two");

    /* A newline cannot be represented, so it becomes a space rather than
     * silently corrupting the following key. */
    CHECK_STR_EQ(s.get_string("app", "note", ""), "line one line two");

    Settings t{temp_path("mb200_unused.ini")};
    t.from_string(s.to_string());
    CHECK_STR_EQ(t.get_string("app", "note", ""), "line one line two");
    CHECK_EQ(t.keys("app").size(), static_cast<size_t>(1));
}

TEST(settings_erase_and_clear) {
    Settings s{temp_path("mb200_unused.ini")};
    s.set_int("a", "x", 1);
    s.set_int("a", "y", 2);
    s.set_int("b", "x", 3);

    CHECK(s.erase("a", "x"));
    CHECK(!s.erase("a", "x"));
    CHECK(!s.has("a", "x"));
    CHECK(s.has("a", "y"));
    CHECK(s.has("b", "x"));

    CHECK(s.erase_section("b"));
    CHECK(!s.has("b", "x"));
    CHECK_EQ(s.sections().size(), static_cast<size_t>(1));

    s.clear();
    CHECK_EQ(s.sections().size(), static_cast<size_t>(0));
}

TEST(settings_load_of_missing_file_is_a_clean_empty_store) {
    ScopedFile file{temp_path("mb200_test_missing.ini")};

    Settings s{file.path};
    s.set_int("a", "x", 1);

    CHECK(!s.load());  /* nothing there yet — a first run */
    CHECK(!s.has("a", "x"));
    CHECK_EQ(s.sections().size(), static_cast<size_t>(0));
}

/* --- pmem ------------------------------------------------------------- */

TEST(pmem_defaults_match_the_firmware) {
    core::pmem::defaults();

    CHECK_EQ(core::pmem::target_frequency(),
             static_cast<uint64_t>(core::pmem::target_frequency_reset_value));
    CHECK_EQ(core::pmem::correction_ppb(), core::pmem::ppb_reset_value);
    CHECK_EQ(core::pmem::config_splash(), true);
    CHECK_EQ(core::pmem::load_app_settings(), true);
    CHECK_EQ(core::pmem::hide_clock(), false);
}

TEST(pmem_stores_and_reads_back) {
    core::pmem::defaults();

    core::pmem::set_target_frequency(433'920'000);
    core::pmem::set_correction_ppb(-1500);
    core::pmem::set_config_splash(false);
    core::pmem::set_config_converter_freq(-125'000'000);
    core::pmem::set_headphone_volume(255);  /* clipped to the 0..99 range */

    CHECK_EQ(core::pmem::target_frequency(), static_cast<uint64_t>(433'920'000));
    CHECK_EQ(core::pmem::correction_ppb(), -1500);
    CHECK_EQ(core::pmem::config_splash(), false);
    CHECK_EQ(core::pmem::config_converter_freq(), static_cast<int64_t>(-125'000'000));
    CHECK_EQ(static_cast<int>(core::pmem::headphone_volume()), 99);

    core::pmem::defaults();
    CHECK_EQ(core::pmem::config_splash(), true);
}

/* --- BoundSetting ----------------------------------------------------- */

TEST(bound_setting_parses_and_formats_each_type) {
    bool b = false;
    uint8_t u8 = 0;
    int32_t i32 = 0;
    uint32_t u32 = 0;
    int64_t i64 = 0;
    uint64_t u64 = 0;
    float f = 0.0f;
    std::string str;

    core::SettingBindings bindings;
    bindings.emplace_back("b", &b);
    bindings.emplace_back("u8", &u8);
    bindings.emplace_back("i32", &i32);
    bindings.emplace_back("u32", &u32);
    bindings.emplace_back("i64", &i64);
    bindings.emplace_back("u64", &u64);
    bindings.emplace_back("f", &f);
    bindings.emplace_back("str", &str);

    bindings[0].parse("1");
    bindings[1].parse("200");
    bindings[2].parse("-2000000000");
    bindings[3].parse("4000000000");
    bindings[4].parse("-9000000000000");
    bindings[5].parse("18000000000000000000");
    bindings[6].parse("37.25");
    bindings[7].parse("  padded  ");

    CHECK_EQ(b, true);
    CHECK_EQ(static_cast<int>(u8), 200);
    CHECK_EQ(i32, -2000000000);
    CHECK_EQ(u32, 4000000000u);
    CHECK_EQ(i64, static_cast<int64_t>(-9000000000000LL));
    CHECK_EQ(u64, static_cast<uint64_t>(18000000000000000000ull));
    CHECK(f == 37.25f);
    CHECK_STR_EQ(str, "padded");

    CHECK_STR_EQ(bindings[0].to_string(), "1");
    CHECK_STR_EQ(bindings[1].to_string(), "200");
    CHECK_STR_EQ(bindings[2].to_string(), "-2000000000");
    CHECK_STR_EQ(bindings[3].to_string(), "4000000000");
    CHECK_STR_EQ(bindings[4].to_string(), "-9000000000000");
    CHECK_STR_EQ(bindings[5].to_string(), "18000000000000000000");
    CHECK_STR_EQ(bindings[6].to_string(), "37.25");
    CHECK_STR_EQ(bindings[7].to_string(), "padded");
}

TEST(bound_setting_keeps_the_default_on_a_bad_value) {
    uint8_t u8 = 7;
    int32_t i32 = -3;
    float f = 1.5f;

    core::SettingBindings bindings;
    bindings.emplace_back("u8", &u8);
    bindings.emplace_back("i32", &i32);
    bindings.emplace_back("f", &f);

    bindings[0].parse("300");     /* out of range for uint8_t */
    bindings[1].parse("rubbish");
    bindings[2].parse("");

    CHECK_EQ(static_cast<int>(u8), 7);
    CHECK_EQ(i32, -3);
    CHECK(f == 1.5f);
}

/* --- Per-app stores --------------------------------------------------- */

TEST(app_store_round_trips_and_survives_junk) {
    const std::string store = "__mb200_test_store";
    ScopedFile file{(std::filesystem::path{core::settings_directory()} /
                     (store + ".ini"))
                        .string()};

    uint64_t frequency = 433'920'000;
    float gain = 37.5f;
    std::string label = "test";

    {
        core::SettingBindings bindings;
        bindings.emplace_back("frequency", &frequency);
        bindings.emplace_back("gain", &gain);
        bindings.emplace_back("label", &label);
        CHECK(core::save_settings(store, bindings));
    }

    CHECK(file.exists());

    /* Corrupt the file the way a hand edit would: one junk line in the middle,
     * one unknown key. The known keys either side must still load. */
    {
        std::string text;
        {
            std::ifstream in{file.path, std::ios::binary};
            text.assign(std::istreambuf_iterator<char>{in},
                        std::istreambuf_iterator<char>{});
        }
        std::ofstream out{file.path, std::ios::binary | std::ios::trunc};
        out << "# a comment\r\n"
            << "not a setting at all\r\n"
            << "unknown_key=12345\r\n"
            << text;
    }

    uint64_t loaded_frequency = 0;
    float loaded_gain = 0.0f;
    std::string loaded_label;

    core::SettingBindings bindings;
    bindings.emplace_back("frequency", &loaded_frequency);
    bindings.emplace_back("gain", &loaded_gain);
    bindings.emplace_back("label", &loaded_label);

    CHECK(core::load_settings(store, bindings));
    CHECK_EQ(loaded_frequency, static_cast<uint64_t>(433'920'000));
    CHECK(loaded_gain == 37.5f);
    CHECK_STR_EQ(loaded_label, "test");
}

TEST(app_store_load_of_missing_file_reports_false) {
    uint64_t frequency = 12345;

    core::SettingBindings bindings;
    bindings.emplace_back("frequency", &frequency);

    CHECK(!core::load_settings("__mb200_no_such_store", bindings));
    /* The bound variable keeps its value. */
    CHECK_EQ(frequency, static_cast<uint64_t>(12345));
}

TEST(app_settings_manager_restores_radio_state_next_launch) {
    using core::app_settings::AppSettings;
    using core::app_settings::Mode;
    using core::app_settings::SettingsManager;

    core::pmem::set_load_app_settings(true);
    core::pmem::set_save_app_settings(true);

    const std::string app = "__mb200_test_app";
    ScopedFile file{
        (std::filesystem::path{core::settings_directory()} / (app + ".ini")).string()};

    /* Stand-ins for the receiver model. */
    uint64_t radio_frequency = 88'500'000;
    float radio_gain = 33.0f;
    uint64_t radio_bandwidth = 1'750'000;
    uint8_t radio_squelch = 0;  /* the zero case, again */

    auto capture = [&](AppSettings& s) {
        s.rx_frequency = radio_frequency;
        s.rx_gain = radio_gain;
        s.baseband_bandwidth = radio_bandwidth;
        s.squelch = radio_squelch;
    };
    auto apply = [&](AppSettings& s) {
        radio_frequency = s.rx_frequency;
        radio_gain = s.rx_gain;
        radio_bandwidth = s.baseband_bandwidth;
        radio_squelch = s.squelch;
    };

    /* First launch: no file, so nothing is loaded and the destructor writes
     * whatever the radio was left at. */
    {
        SettingsManager manager{app, Mode::Rx, capture, apply};
        CHECK(!manager.loaded());
        CHECK_EQ(manager.raw().rx_frequency, static_cast<uint64_t>(88'500'000));
    }
    CHECK(file.exists());

    /* Second launch: the radio starts somewhere else entirely and must be put
     * back where it was. */
    radio_frequency = 1;
    radio_gain = 1.0f;
    radio_bandwidth = 1;
    radio_squelch = 77;

    {
        SettingsManager manager{app, Mode::Rx, capture, apply};
        CHECK(manager.loaded());
        CHECK_EQ(manager.raw().rx_frequency, static_cast<uint64_t>(88'500'000));
    }

    CHECK_EQ(radio_frequency, static_cast<uint64_t>(88'500'000));
    CHECK(radio_gain == 33.0f);
    CHECK_EQ(radio_bandwidth, static_cast<uint64_t>(1'750'000));
    /* A squelch of 0 was saved and must come back as 0, not as the 77 the
     * radio happened to be sitting at. */
    CHECK_EQ(static_cast<int>(radio_squelch), 0);
}

TEST(app_settings_manager_honours_the_load_app_settings_flag) {
    using core::app_settings::AppSettings;
    using core::app_settings::Mode;
    using core::app_settings::SettingsManager;

    core::pmem::set_load_app_settings(true);
    core::pmem::set_save_app_settings(true);

    const std::string app = "__mb200_test_flag";
    ScopedFile file{
        (std::filesystem::path{core::settings_directory()} / (app + ".ini")).string()};

    uint64_t radio_frequency = 144'800'000;
    std::string ui_choice = "waterfall";

    auto capture = [&](AppSettings& s) { s.rx_frequency = radio_frequency; };
    auto apply = [&](AppSettings& s) { radio_frequency = s.rx_frequency; };

    {
        core::SettingBindings extra;
        extra.emplace_back("ui_choice", &ui_choice);
        SettingsManager manager{app, Mode::Rx, capture, apply, std::move(extra)};
        CHECK(!manager.loaded());
    }
    CHECK(file.exists());

    /* Turn the radio half off. The app's own settings still load; the radio
     * state does not. */
    core::pmem::set_load_app_settings(false);

    radio_frequency = 7;
    ui_choice.clear();
    {
        core::SettingBindings extra;
        extra.emplace_back("ui_choice", &ui_choice);
        SettingsManager manager{app, Mode::Rx, capture, apply, std::move(extra)};
        CHECK(manager.loaded());
    }

    CHECK_STR_EQ(ui_choice, "waterfall");
    CHECK_EQ(radio_frequency, static_cast<uint64_t>(7));

    core::pmem::set_load_app_settings(true);
}

TEST(app_settings_bindings_follow_the_mode) {
    using core::app_settings::AppSettings;
    using core::app_settings::Mode;

    AppSettings rx;
    rx.mode = Mode::Rx;
    core::SettingBindings rx_bindings;
    core::app_settings::bind_common(rx, rx_bindings);

    AppSettings tx;
    tx.mode = Mode::Tx;
    core::SettingBindings tx_bindings;
    core::app_settings::bind_common(tx, tx_bindings);

    auto has_name = [](const core::SettingBindings& b, const char* name) {
        for (const auto& setting : b)
            if (setting.name() == name) return true;
        return false;
    };

    CHECK(has_name(rx_bindings, "rx_frequency"));
    CHECK(has_name(rx_bindings, "squelch"));
    CHECK(!has_name(rx_bindings, "tx_frequency"));

    CHECK(has_name(tx_bindings, "tx_frequency"));
    CHECK(has_name(tx_bindings, "tx_gain"));
    CHECK(!has_name(tx_bindings, "rx_frequency"));

    /* Common settings are present whichever direction the app runs. */
    for (const char* name : {"baseband_bandwidth", "sampling_rate", "step", "volume"}) {
        CHECK(has_name(rx_bindings, name));
        CHECK(has_name(tx_bindings, name));
    }
}
