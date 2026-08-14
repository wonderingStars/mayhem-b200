/*
 * mayhem-b200 — Scanner (implementation).
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2018 Furrtek
 * Copyright (C) 2023 Mark Thompson (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_scanner.hpp"

#include "app_context.hpp"
#include "app_registry.hpp"
#include "bitmaps.hpp"
#include "settings.hpp"
#include "string_format.hpp"
#include "theme.hpp"
#include "ui_alphanum.hpp"
#include "ui_menu.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

#include "../audio/audio_out.hpp"

#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace app {

using Mode = radio::ReceiverModel::Mode;
using AmConfig = radio::ReceiverModel::AmConfig;
using NfmConfig = radio::ReceiverModel::NfmConfig;
using WfmConfig = radio::ReceiverModel::WfmConfig;

namespace {

constexpr std::string_view kSection = "rx_scanner";

/* Level-bar scaling, in dBFS. */
constexpr float kLevelFloorDb = -100.0f;
constexpr float kLevelCeilDb = -10.0f;

/* Modulation indices, matching radio::ReceiverModel::Mode and freqman's m=. */
constexpr int32_t kModAM = 0;
constexpr int32_t kModNFM = 1;
constexpr int32_t kModWFM = 2;

/* Freqman list picker.
 *
 * Upstream pushes FileLoadView over the FREQMAN directory; this build has no
 * such view, so the picker is a MenuView over core::get_freqman_files(). It is
 * declared here rather than in a shared header because every app that needs one
 * owns its own copy (doc/PORTING.md: do not create shared files). */
class ScannerFilePicker : public ui::View {
   public:
    explicit ScannerFilePicker(std::function<void(std::string)> on_pick)
        : on_pick_{std::move(on_pick)} {
        add_children({&menu_, &text_empty_});

        auto files = core::get_freqman_files();
        if (files.empty()) {
            text_empty_.set("No lists in FREQMAN");
            menu_.hidden(true);
            return;
        }

        text_empty_.hidden(true);
        for (auto& stem : files) {
            std::string name = stem;
            menu_.add_item({name, ui::Color::white(), [this, name]() {
                                if (on_pick_) on_pick_(name);
                                if (auto* nav = app::globals().nav) nav->pop();
                            }});
        }
    }

    std::string title() const override { return "Load list"; }

    void focus() override {
        if (!menu_.hidden())
            menu_.focus();
        else
            text_empty_.focus();
    }

   private:
    std::function<void(std::string)> on_pick_{};
    ui::MenuView menu_{{0, 0, 240, 288}};
    ui::Text text_empty_{{0, 8, 240, 16}, ""};
};

}  // namespace

/* ======================================================================== */
/* ScannerView                                                               */
/* ======================================================================== */

ScannerView::ScannerView()
    : receiver_{*app::globals().receiver} {
    add_children({&labels_,
                  &field_gain_,
                  &field_volume_,
                  &field_squelch_,
                  &field_browse_wait_,
                  &field_lock_wait_,
                  &field_mode_,
                  &field_bw_,
                  &level_meter_,
                  &text_level_,
                  &field_current_index_,
                  &text_max_index_,
                  &button_load_,
                  &button_clear_,
                  &text_current_desc_,
                  &big_display_,
                  &field_step_,
                  &button_manual_search_,
                  &button_manual_start_,
                  &button_manual_end_,
                  &button_audio_,
                  &button_pause_,
                  &button_dir_,
                  &button_add_,
                  &button_remove_,
                  &text_status_});

    /* --- persisted settings (upstream's app_settings::SettingsManager) --- */
    {
        auto& s = core::settings();
        engine_.set_browse_wait(static_cast<uint32_t>(s.get_uint(kSection, "browse_wait", 5)));
        engine_.set_lock_wait(static_cast<uint32_t>(s.get_uint(kSection, "lock_wait", 2)));
        engine_.set_squelch(static_cast<int32_t>(s.get_int(kSection, "scanner_squelch", -30)));
        frequency_range_.min = s.get_int(kSection, "range_min", 0);
        frequency_range_.max = s.get_int(kSection, "range_max", 0);
        freqman_file_ = s.get_string(kSection, "file", "SCANNER");
    }

    /* --- mode / bandwidth / step selectors --- */
    field_mode_.set_options({{"AM ", kModAM}, {"NFM", kModNFM}, {"WFM", kModWFM}});
    field_mode_.on_change = [this](size_t, int32_t v) { apply_mode(v); };

    field_bw_.on_change = [this](size_t index, int32_t) {
        switch (receiver_.mode()) {
            case Mode::AMAudio:
                receiver_.set_am_configuration(static_cast<AmConfig>(index));
                break;
            case Mode::NarrowbandFMAudio:
                receiver_.set_nfm_configuration(static_cast<NfmConfig>(index));
                break;
            case Mode::WidebandFMAudio:
                receiver_.set_wfm_configuration(static_cast<WfmConfig>(index));
                break;
            default:
                break;
        }
    };

    {
        ui::OptionsField::options_t steps;
        for (size_t i = 0; i < core::freqman_step_count(); ++i) {
            const auto idx = static_cast<core::freqman_index_t>(i);
            steps.push_back({core::freqman_entry_get_step_string_short(idx),
                             core::freqman_entry_get_step_value(idx)});
        }
        field_step_.set_options(std::move(steps));
        field_step_.set_by_value(25'000, false);
    }
    field_step_.on_change = [this](size_t, int32_t) {
        if (manual_search_) restart_scan();
    };

    /* Gain range from the device, not from the published figures. */
    if (auto* r = app::globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
    }

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t v) { receiver_.set_gain(v); };

    field_volume_.set_value(receiver_.volume(), false);
    field_volume_.on_change = [this](int32_t v) {
        receiver_.set_volume(static_cast<uint8_t>(v));
    };

    field_squelch_.set_value(engine_.squelch(), false);
    field_squelch_.on_change = [this](int32_t v) {
        engine_.set_squelch(v);
        core::settings().set_int(kSection, "scanner_squelch", v);
    };

    field_browse_wait_.set_value(static_cast<int32_t>(engine_.browse_wait()), false);
    field_browse_wait_.on_change = [this](int32_t v) {
        engine_.set_browse_wait(static_cast<uint32_t>(v));
        core::settings().set_uint(kSection, "browse_wait", static_cast<uint64_t>(v));
    };

    field_lock_wait_.set_value(static_cast<int32_t>(engine_.lock_wait()), false);
    field_lock_wait_.on_change = [this](int32_t v) {
        engine_.set_lock_wait(static_cast<uint32_t>(v));
        core::settings().set_uint(kSection, "lock_wait", static_cast<uint64_t>(v));
    };

    button_manual_start_.set_text(to_string_short_freq(
        static_cast<uint64_t>(std::max<int64_t>(frequency_range_.min, 0))));
    button_manual_end_.set_text(to_string_short_freq(
        static_cast<uint64_t>(std::max<int64_t>(frequency_range_.max, 0))));

    /* --- buttons --- */
    button_load_.on_select = [this](ui::Button&) {
        auto* nav = app::globals().nav;
        if (!nav) return;
        engine_.scan_pause();
        nav->push(std::make_unique<ScannerFilePicker>(
            [this](std::string stem) { frequency_file_load(stem); }));
    };

    button_clear_.on_select = [this](ui::Button&) {
        if (entries_.empty()) return;
        engine_.stepper().stop();
        entries_.clear();
        show_max_index();
        field_current_index_.set_text("");
        text_current_desc_.set(loaded_filename());
        engine_.stepper().set_freq_lock(0);
    };

    button_manual_start_.on_select = [this](ui::Button&) {
        auto* nav = app::globals().nav;
        if (!nav) return;
        static std::string buf;
        buf = to_string_dec_uint(static_cast<uint64_t>(std::max<int64_t>(frequency_range_.min, 0)));
        ui::text_prompt(*nav, buf, 12, ENTER_KEYBOARD_MODE_DIGITS,
                        [this](std::string& b) {
                            frequency_range_.min = std::strtoll(b.c_str(), nullptr, 10);
                            core::settings().set_int(kSection, "range_min", frequency_range_.min);
                            button_manual_start_.set_text(to_string_short_freq(
                                static_cast<uint64_t>(std::max<int64_t>(frequency_range_.min, 0))));
                        });
    };

    button_manual_end_.on_select = [this](ui::Button&) {
        auto* nav = app::globals().nav;
        if (!nav) return;
        static std::string buf;
        buf = to_string_dec_uint(static_cast<uint64_t>(std::max<int64_t>(frequency_range_.max, 0)));
        ui::text_prompt(*nav, buf, 12, ENTER_KEYBOARD_MODE_DIGITS,
                        [this](std::string& b) {
                            frequency_range_.max = std::strtoll(b.c_str(), nullptr, 10);
                            core::settings().set_int(kSection, "range_max", frequency_range_.max);
                            button_manual_end_.set_text(to_string_short_freq(
                                static_cast<uint64_t>(std::max<int64_t>(frequency_range_.max, 0))));
                        });
    };

    /* Toggle between Manual Search and Freq List Scan, upstream's checks. */
    button_manual_search_.on_select = [this](ui::Button&) {
        if (!manual_search_) {
            if (!frequency_range_.min || !frequency_range_.max) {
                ui::display_modal("Error", "Both START and END freqs\nneed a value");
                return;
            }
            if (frequency_range_.min > frequency_range_.max) {
                ui::display_modal("Error", "END freq\nis lower than START");
                return;
            }
            manual_search_ = true;
        } else {
            manual_search_ = false;
        }
        restart_scan();
    };

    button_pause_.on_select = [this](ui::ButtonWithEncoder&) {
        if (engine_.userpause()) {
            engine_.user_resume();
            button_pause_.set_text("<PAUSE>");
        } else {
            engine_.user_pause();
            button_pause_.set_text("<RESUME>");
        }
    };
    button_pause_.on_change = [this]() {
        handle_encoder(button_pause_.get_encoder_delta());
        button_pause_.set_encoder_delta(0);
    };
    field_current_index_.on_encoder_change = [this](ui::TextField&, ui::EncoderEvent delta) {
        handle_encoder(delta);
    };

    button_dir_.on_select = [this](ui::Button&) {
        fwd_ = !fwd_;
        engine_.stepper().set_scanning_direction(fwd_);
        if (engine_.userpause()) {
            engine_.user_resume();
            button_pause_.set_text("<PAUSE>");
        }
        button_dir_.set_text(fwd_ ? "REVERSE" : "FORWARD");
        engine_.set_color(scanner::BigDisplayColor::Grey);
    };

    button_add_.on_select = [this](ui::Button&) { add_current_frequency(); };
    button_remove_.on_select = [this](ui::Button&) { remove_current_frequency(); };

    button_audio_.on_select = [](ui::Button&) {
        auto* nav = app::globals().nav;
        if (!nav) return;
        const auto* entry = app::AppRegistry::instance().by_id("audio");
        if (entry && entry->factory) nav->push(entry->factory());
    };

    /* Upstream disables the model squelch: the statistics handler is where the
     * squelching happens for this app. */
    receiver_.set_squelch_level(0);

    field_mode_.set_by_value(kModNFM, false);
    apply_mode(kModNFM);

    frequency_file_load(freqman_file_);
}

ScannerView::~ScannerView() {
    core::settings().set_string(kSection, "file", freqman_file_);
    core::settings().save();
    /* No receiver stop here on purpose: NavigationView::service() stops the
     * radio centrally when navigation lands at the menu, and stopping here
     * too would bounce the stream on every app-to-app switch. */
}

std::string ScannerView::loaded_filename() const {
    auto filename = freqman_file_;
    if (filename.length() > 23) {
        filename.resize(22);
        filename = filename + "+";
    }
    return filename;
}

void ScannerView::apply_mode(int32_t modulation) {
    switch (modulation) {
        case kModAM:
            receiver_.set_mode(Mode::AMAudio);
            break;
        case kModNFM:
            receiver_.set_mode(Mode::NarrowbandFMAudio);
            break;
        case kModWFM:
            receiver_.set_mode(Mode::WidebandFMAudio);
            break;
        default:
            return;
    }
    update_bandwidth_options(modulation);
}

void ScannerView::update_bandwidth_options(int32_t modulation) {
    ui::OptionsField::options_t options;
    size_t selected = 0;
    switch (modulation) {
        case kModAM:
            options = {{"DSB9k", 0}, {"DSB6k", 1}, {"USB", 2}, {"LSB", 3}, {"CW", 4}};
            selected = static_cast<size_t>(receiver_.am_configuration());
            break;
        case kModNFM:
            options = {{"8k5", 0}, {"11k", 1}, {"16k", 2}};
            selected = static_cast<size_t>(receiver_.nfm_configuration());
            break;
        case kModWFM:
            options = {{"200k", 0}, {"180k", 1}};
            selected = static_cast<size_t>(receiver_.wfm_configuration());
            break;
        default:
            options = {{"-", 0}};
            break;
    }
    field_bw_.set_options(std::move(options));
    field_bw_.set_selected_index(selected, false);
}

/* Upstream ScannerView::frequency_file_load(). */
void ScannerView::frequency_file_load(const std::string& stem) {
    core::freqman_index_t def_mod_index{core::freqman_invalid_index};
    core::freqman_index_t def_bw_index{core::freqman_invalid_index};
    core::freqman_index_t def_step_index{core::freqman_invalid_index};

    core::FreqmanDB db;
    if (!db.open(core::get_freqman_path(stem))) {
        text_current_desc_.set("NO " + stem + core::freqman_extension);
        entries_.clear();
        manual_search_ = true;
        restart_scan();
        return;
    }

    entries_.clear();
    freqman_file_ = stem;

    bool have_range = false;
    scanner::ScannerRange range{0, 0};

    for (auto entry : db) {
        if (core::is_invalid(def_mod_index)) def_mod_index = entry.modulation;
        if (core::is_invalid(def_bw_index)) def_bw_index = entry.bandwidth;
        if (core::is_invalid(def_step_index)) def_step_index = entry.step;

        switch (entry.type) {
            case core::freqman_type::Repeater:
            case core::freqman_type::Single:
                entries_.push_back({entry.frequency_a, entry.description});
                break;
            case core::freqman_type::HamRadio:
                entries_.push_back({entry.frequency_a, "R: " + entry.description});
                entries_.push_back({entry.frequency_b, "T: " + entry.description});
                break;
            case core::freqman_type::Range:
                /* Only the first range in the file is used, as upstream. */
                if (!have_range) {
                    range = {entry.frequency_a, entry.frequency_b};
                    have_range = true;
                }
                break;
            default:
                break;
        }

        if (entries_.size() >= core::freqman_default_max_entries) break;
    }

    if (core::is_valid(def_mod_index) && def_mod_index <= kModWFM) {
        field_mode_.set_by_value(static_cast<int32_t>(def_mod_index), false);
        apply_mode(static_cast<int32_t>(def_mod_index));
    }
    if (core::is_valid(def_bw_index) && def_bw_index < field_bw_.options().size())
        field_bw_.set_selected_index(def_bw_index);
    if (core::is_valid(def_step_index) && def_step_index < core::freqman_step_count())
        field_step_.set_selected_index(def_step_index, false);

    if (have_range) {
        frequency_range_ = range;
        button_manual_start_.set_text(
            to_string_short_freq(static_cast<uint64_t>(std::max<int64_t>(range.min, 0))));
        button_manual_end_.set_text(
            to_string_short_freq(static_cast<uint64_t>(std::max<int64_t>(range.max, 0))));
    }

    /* Scan entries if any, otherwise fall back to a manual range search. */
    manual_search_ = entries_.empty();
    restart_scan();
}

void ScannerView::restart_scan() {
    if (engine_.userpause()) {
        engine_.user_resume();
        button_pause_.set_text("<PAUSE>");
    }
    engine_.reset_timers();
    start_scan();
}

void ScannerView::start_scan() {
    show_max_index();

    if (manual_search_) {
        button_manual_search_.set_text("SCAN");
        text_current_desc_.set("SEARCHING...");
        engine_.stepper().start_range(frequency_range_,
                                      field_step_.selected_index_value(), fwd_);
        if (!engine_.stepper().running())
            text_current_desc_.set("BAD RANGE / STEP");
    } else {
        button_manual_search_.set_text("SRCH");
        text_current_desc_.set(loaded_filename());

        std::vector<int64_t> freqs;
        freqs.reserve(entries_.size());
        for (const auto& e : entries_) freqs.push_back(e.freq);
        engine_.stepper().start_list(std::move(freqs), fwd_);
    }
}

void ScannerView::show_max_index() {
    field_current_index_.set_text("<->");
    if (entries_.size() >= core::freqman_default_max_entries) {
        text_max_index_.set_style(ui::Theme::getInstance()->fg_red);
        text_max_index_.set("/ " + to_string_dec_uint(core::freqman_default_max_entries) +
                            " (MAX)");
    } else {
        text_max_index_.set_style(ui::Theme::getInstance()->fg_medium);
        text_max_index_.set("/ " + to_string_dec_uint(entries_.size()));
    }
}

void ScannerView::handle_encoder(int32_t delta) {
    if (delta == 0) return;
    engine_.stepper().set_index_stepper(delta > 0 ? 1 : -1);
    /* Restart the browse timer when the frequency changes (upstream). */
    engine_.restart_browse_timer();
}

/* Upstream ScannerView::handle_retune(). */
void ScannerView::handle_retune(int64_t freq, uint32_t index) {
    current_index_ = index;
    current_frequency_ = freq;

    receiver_.set_target_frequency(static_cast<uint64_t>(std::max<int64_t>(freq, 0)));

    const auto color = engine_.retune_color();
    if (color != scanner::BigDisplayColor::Keep) {
        engine_.set_color(color);
        apply_color();
    }

    if (!manual_search_) {
        if (!entries_.empty())
            field_current_index_.set_text(to_string_dec_uint(index + 1, 3));

        if (index < entries_.size() && entries_[index].description.size() > 1)
            text_current_desc_.set(entries_[index].description);
        else
            text_current_desc_.set(loaded_filename());
    }
}

void ScannerView::apply_color() {
    const auto c = engine_.color();
    if (c == shown_color_) return;
    shown_color_ = c;
    switch (c) {
        case scanner::BigDisplayColor::Grey:
            big_display_.set_style(ui::Theme::getInstance()->fg_medium);
            break;
        case scanner::BigDisplayColor::Yellow:
            big_display_.set_style(ui::Theme::getInstance()->fg_yellow);
            break;
        case scanner::BigDisplayColor::Green:
            big_display_.set_style(ui::Theme::getInstance()->fg_green);
            break;
        case scanner::BigDisplayColor::Red:
            big_display_.set_style(ui::Theme::getInstance()->fg_red);
            break;
        case scanner::BigDisplayColor::Keep:
            break;
    }
}

void ScannerView::add_current_frequency() {
    if (current_frequency_ <= 0) return;

    core::FreqmanDB db;
    if (!db.open(core::get_freqman_path(freqman_file_), /*create*/ true)) {
        ui::display_modal("Error", "Cannot open " + freqman_file_ +
                                       core::freqman_extension + "\nfor appending freq.");
        return;
    }

    core::freqman_entry entry{};
    entry.frequency_a = current_frequency_;
    entry.type = core::freqman_type::Single;

    auto it = db.find_entry([&entry](const core::freqman_entry& e) {
        return e.frequency_a == entry.frequency_a;
    });
    if (it != db.end()) {
        ui::display_modal("Error", "Frequency already exists");
        return;
    }

    db.append_entry(entry);
    if (entries_.size() < core::freqman_default_max_entries) {
        entries_.push_back({current_frequency_, ""});
        show_max_index();
    }
}

void ScannerView::remove_current_frequency() {
    if (entries_.size() <= current_index_) return;

    engine_.stepper().set_scanning(false);
    engine_.stepper().set_freq_del(entries_[current_index_].freq);
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(current_index_));

    show_max_index();
    text_current_desc_.set("");
    engine_.stepper().set_freq_lock(0);
}

void ScannerView::on_show() {
    View::on_show();
    button_pause_.focus();
    if (!receiver_.running()) receiver_.start();
    timing_started_ = false;
}

void ScannerView::on_frame_sync() {
    View::on_frame_sync();

    const auto now = std::chrono::steady_clock::now();
    if (!timing_started_) {
        timing_started_ = true;
        last_step_ = now;
        last_stats_ = now;
        return;
    }

    /* --- stepper: one pass every SCANNER_SLEEP_MS --- */
    const auto step_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now - last_step_)
                                  .count();
    if (step_elapsed >= static_cast<long long>(scanner::kScannerSleepMs)) {
        last_step_ = now;
        const auto tick = engine_.stepper().tick();
        if (tick.emitted) handle_retune(tick.freq, tick.index);
    }

    /* --- statistics: STATISTICS_UPDATES_PER_SEC times a second --- */
    const auto stats_interval = 1000 / static_cast<long long>(scanner::kStatisticsUpdatesPerSec);
    const auto stats_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   now - last_stats_)
                                   .count();
    if (stats_elapsed >= stats_interval) {
        last_stats_ = now;

        const float level = receiver_.channel_level_db();
        engine_.on_statistics_update(static_cast<int32_t>(std::lround(level)));
        apply_color();

        /* Audio gate: the engine says whether the scanner wants to hear the
         * channel; the firmware called audio::output::start()/stop(). */
        if (engine_.audio_enabled() != shown_audio_) {
            shown_audio_ = engine_.audio_enabled();
            if (auto* out = app::globals().audio_out) out->set_muted(!shown_audio_);
        }

        float frac = (level - kLevelFloorDb) / (kLevelCeilDb - kLevelFloorDb);
        frac = std::clamp(frac, 0.0f, 1.0f);
        level_meter_.set_value(static_cast<uint8_t>(frac * 255.0f));
        text_level_.set(to_string_dec_int(static_cast<int32_t>(std::lround(level))) + "dB");

        if (shown_frequency_ != current_frequency_) {
            shown_frequency_ = current_frequency_;
            big_display_.set(static_cast<uint64_t>(std::max<int64_t>(current_frequency_, 0)));
        }

        std::string status = engine_.userpause() ? "PAUSED "
                             : engine_.stepper().is_scanning() ? "SCAN "
                                                               : "HOLD ";
        status += to_string_dec_uint(engine_.stepper().is_freq_lock()) + "/" +
                  to_string_dec_uint(scanner::kMaxFreqLock);
        text_status_.set(status);
    }
}

}  // namespace app

/* --- Registration --------------------------------------------------------- */

namespace {
const app::Registrar reg_scanner{{"scanner", "Scanner", app::Category::Receive,
                                  ui::Color::green(), &ui::bitmap_icon_scanner,
                                  [] { return std::make_unique<app::ScannerView>(); }}};
}  // namespace
