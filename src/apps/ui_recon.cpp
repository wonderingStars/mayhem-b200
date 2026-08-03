/*
 * mayhem-b200 — Recon (implementation).
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2018 Furrtek
 * Copyright (C) 2023 gullradriel, Nilorea Studio Inc. (original app design)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_recon.hpp"

#include "app_context.hpp"
#include "app_registry.hpp"
#include "bitmaps.hpp"
#include "settings.hpp"
#include "string_format.hpp"
#include "theme.hpp"
#include "ui_alphanum.hpp"
#include "ui_modal.hpp"

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

constexpr std::string_view kSection = "recon";

/* Channel level floor / ceiling used to drive the level bar (dBFS). */
constexpr float kLevelFloorDb = -100.0f;
constexpr float kLevelCeilDb = -10.0f;

}  // namespace

/* ======================================================================== */
/* ReconSetupView                                                            */
/* ======================================================================== */

ReconSetupView::ReconSetupView(std::string input_file, std::string output_file)
    : input_file_{std::move(input_file)}, output_file_{std::move(output_file)} {
    add_children({&labels_,
                  &button_input_file_,
                  &button_output_file_,
                  &check_autosave_,
                  &check_autostart_,
                  &check_clear_output_,
                  &check_load_freqs_,
                  &check_load_ranges_,
                  &check_load_ham_,
                  &check_load_repeaters_,
                  &check_update_ranges_,
                  &text_note_,
                  &button_save_});

    button_input_file_.set_text(input_file_);
    button_output_file_.set_text(output_file_);

    auto& s = core::settings();
    check_autosave_.set_value(s.get_bool(kSection, "autosave", true));
    check_autostart_.set_value(s.get_bool(kSection, "autostart", true));
    check_clear_output_.set_value(s.get_bool(kSection, "clear_output", false));
    check_load_freqs_.set_value(s.get_bool(kSection, "load_freqs", true));
    check_load_ranges_.set_value(s.get_bool(kSection, "load_ranges", true));
    check_load_ham_.set_value(s.get_bool(kSection, "load_hamradios", true));
    check_load_repeaters_.set_value(s.get_bool(kSection, "load_repeaters", true));
    check_update_ranges_.set_value(s.get_bool(kSection, "update_ranges", true));

    text_note_.set_style(ui::Theme::getInstance()->fg_yellow);

    button_input_file_.on_select = [this](ui::Button&) {
        if (auto* nav = app::globals().nav) {
            ui::text_prompt(*nav, input_file_, 28, ENTER_KEYBOARD_MODE_ALPHA,
                            [this](std::string& buffer) {
                                input_file_ = buffer;
                                button_input_file_.set_text(input_file_);
                            });
        }
    };

    button_output_file_.on_select = [this](ui::Button&) {
        if (auto* nav = app::globals().nav) {
            ui::text_prompt(*nav, output_file_, 28, ENTER_KEYBOARD_MODE_ALPHA,
                            [this](std::string& buffer) {
                                output_file_ = buffer;
                                button_output_file_.set_text(output_file_);
                            });
        }
    };

    button_save_.on_select = [this](ui::Button&) {
        save();
        if (on_changed) on_changed({input_file_, output_file_});
        if (auto* nav = app::globals().nav) nav->pop();
    };
}

void ReconSetupView::focus() {
    button_input_file_.focus();
}

void ReconSetupView::save() {
    auto& s = core::settings();
    s.set_string(kSection, "input_file", input_file_);
    s.set_string(kSection, "output_file", output_file_);
    s.set_bool(kSection, "autosave", check_autosave_.value());
    s.set_bool(kSection, "autostart", check_autostart_.value());
    s.set_bool(kSection, "clear_output", check_clear_output_.value());
    s.set_bool(kSection, "load_freqs", check_load_freqs_.value());
    s.set_bool(kSection, "load_ranges", check_load_ranges_.value());
    s.set_bool(kSection, "load_hamradios", check_load_ham_.value());
    s.set_bool(kSection, "load_repeaters", check_load_repeaters_.value());
    s.set_bool(kSection, "update_ranges", check_update_ranges_.value());
    s.save();
}

/* ======================================================================== */
/* ReconView                                                                 */
/* ======================================================================== */

ReconView::ReconView()
    : receiver_{*app::globals().receiver} {
    add_children({&big_freq_,
                  &freq_stats_,
                  &text_timer_,
                  &file_name_,
                  &desc_cycle_,
                  &level_meter_,
                  &text_max_,
                  &text_nb_locks_,
                  &param_labels_,
                  &field_mode_,
                  &field_bw_,
                  &step_mode_,
                  &field_match_mode_,
                  &field_squelch_,
                  &field_nblocks_,
                  &field_wait_,
                  &field_lock_wait_,
                  &field_gain_,
                  &field_volume_,
                  &button_pause_,
                  &button_scanner_mode_,
                  &button_dir_,
                  &button_restart_,
                  &button_config_,
                  &button_loop_,
                  &button_add_,
                  &button_manual_start_,
                  &button_manual_end_,
                  &button_manual_recon_,
                  &button_remove_});

    load_persisted_settings();

    /* Build the step selector from the freqman step table, value = Hz. */
    {
        ui::OptionsField::options_t steps;
        for (size_t i = 0; i < core::freqman_step_count(); ++i) {
            int32_t hz = core::freqman_entry_get_step_value(static_cast<core::freqman_index_t>(i));
            steps.push_back({core::freqman_entry_get_step_string_short(
                                 static_cast<core::freqman_index_t>(i)),
                             hz});
        }
        step_mode_.set_options(std::move(steps));
        step_mode_.set_by_value(25'000, false);  /* 25 kHz default */
    }
    engine_.set_default_step(step_mode_.selected_index_value());
    step_mode_.on_change = [this](size_t, int32_t hz) {
        engine_.set_default_step(hz);
    };

    /* Gain range and default frequency from the device caps, not constants. */
    if (auto* r = app::globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
    }
    range_min_ = static_cast<int64_t>(receiver_.target_frequency());
    range_max_ = range_min_;
    if (update_ranges_) {
        int64_t f = static_cast<int64_t>(receiver_.target_frequency());
        range_min_ = std::max<int64_t>(0, f - 1'000'000);
        range_max_ = f + 1'000'000;
    }
    button_manual_start_.set_text(to_string_short_freq(static_cast<uint64_t>(range_min_)));
    button_manual_end_.set_text(to_string_short_freq(static_cast<uint64_t>(range_max_)));

    /* engine config from the fields */
    engine_.set_squelch(kReconDefSquelch);
    engine_.set_wait(kReconDefWaitDuration);
    engine_.set_lock_nb_match(kReconDefNbMatch);
    engine_.set_lock_duration(kReconMinLockDuration);
    engine_.set_match_mode(kReconMatchContinuous);
    engine_.set_continuous(core::settings().get_bool(kSection, "continuous", true));

    field_squelch_.set_value(kReconDefSquelch, false);
    field_squelch_.on_change = [this](int32_t v) { engine_.set_squelch(v); };

    field_wait_.set_value(kReconDefWaitDuration, false);
    field_wait_.on_change = [this](int32_t v) { engine_.set_wait(v); };

    field_nblocks_.set_value(static_cast<int32_t>(kReconDefNbMatch), false);
    field_nblocks_.on_change = [this](int32_t v) {
        engine_.set_lock_nb_match(static_cast<uint32_t>(v));
    };

    field_lock_wait_.set_value(static_cast<int32_t>(kReconMinLockDuration), false);
    field_lock_wait_.on_change = [this](int32_t v) {
        engine_.set_lock_duration(static_cast<uint32_t>(v));
    };

    field_match_mode_.set_by_value(kReconMatchContinuous, false);
    field_match_mode_.on_change = [this](size_t, int32_t v) { engine_.set_match_mode(v); };

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

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t v) { receiver_.set_gain(v); };

    field_volume_.set_value(receiver_.volume(), false);
    field_volume_.on_change = [this](int32_t v) {
        receiver_.set_volume(static_cast<uint8_t>(v));
    };

    /* --- buttons --- */
    button_pause_.on_select = [this](ui::Button&) {
        if (engine_.freq_lock() > 0) {
            /* Locked: skip to the next channel. */
            engine_.request_step(engine_.forward() ? 1 : -1);
        } else if (engine_.recon()) {
            engine_.pause();
        } else {
            engine_.resume();
        }
    };

    button_dir_.on_select = [this](ui::Button&) {
        bool fwd = !engine_.forward();
        engine_.set_forward(fwd);
        button_dir_.set_text(fwd ? "FW>" : "<RW");
        if (!engine_.recon()) engine_.resume();
    };

    button_restart_.on_select = [this](ui::Button&) { reload_restart(); };

    button_scanner_mode_.on_select = [this](ui::Button&) {
        manual_mode_ = false;
        set_scanner_mode(!scanner_mode_);
        frequency_file_load();
        if (autostart_)
            engine_.resume();
        else
            engine_.pause();
    };

    button_loop_.on_select = [this](ui::Button&) {
        set_loop_config(!engine_.continuous());
    };
    set_loop_config(engine_.continuous());

    button_config_.on_select = [this](ui::Button&) { open_config(); };

    button_add_.on_select = [this](ui::Button&) {
        if (!scanner_mode_) recon_save_freq();
    };

    button_remove_.on_select = [this](ui::Button&) { handle_remove(); };

    button_manual_recon_.on_select = [this](ui::Button&) { start_manual_search(); };

    button_manual_start_.on_select = [this](ui::ButtonWithEncoder&) {
        if (auto* nav = app::globals().nav) {
            static std::string buf;
            buf = to_string_dec_uint(static_cast<uint64_t>(range_min_));
            ui::text_prompt(*nav, buf, 12, ENTER_KEYBOARD_MODE_DIGITS,
                            [this](std::string& b) {
                                range_min_ = std::strtoll(b.c_str(), nullptr, 10);
                                button_manual_start_.set_text(
                                    to_string_short_freq(static_cast<uint64_t>(range_min_)));
                            });
        }
    };
    button_manual_start_.on_change = [this]() {
        range_min_ += static_cast<int64_t>(button_manual_start_.get_encoder_delta()) *
                      engine_.default_step();
        if (range_min_ < 0) range_min_ = 0;
        button_manual_start_.set_text(to_string_short_freq(static_cast<uint64_t>(range_min_)));
        button_manual_start_.set_encoder_delta(0);
    };

    button_manual_end_.on_select = [this](ui::ButtonWithEncoder&) {
        if (auto* nav = app::globals().nav) {
            static std::string buf;
            buf = to_string_dec_uint(static_cast<uint64_t>(range_max_));
            ui::text_prompt(*nav, buf, 12, ENTER_KEYBOARD_MODE_DIGITS,
                            [this](std::string& b) {
                                range_max_ = std::strtoll(b.c_str(), nullptr, 10);
                                button_manual_end_.set_text(
                                    to_string_short_freq(static_cast<uint64_t>(range_max_)));
                            });
        }
    };
    button_manual_end_.on_change = [this]() {
        range_max_ += static_cast<int64_t>(button_manual_end_.get_encoder_delta()) *
                      engine_.default_step();
        if (range_max_ < 1) range_max_ = 1;
        button_manual_end_.set_text(to_string_short_freq(static_cast<uint64_t>(range_max_)));
        button_manual_end_.set_encoder_delta(0);
    };

    /* Start on AM, then load the list. */
    field_mode_.set_by_value(kModAM, false);
    apply_mode(kModAM);

    if (clear_output_) core::delete_freqman_file(output_file_);

    frequency_file_load();
    if (autostart_ && engine_.has_entries())
        engine_.resume();
    else
        engine_.pause();

    update_readouts();
}

ReconView::~ReconView() {
    /* Leave the radio streaming for the menu; navigation mutes audio. */
}

core::freqman_load_options ReconView::make_load_options() const {
    core::freqman_load_options o;
    o.load_freqs = load_freqs_;
    o.load_ranges = load_ranges_;
    o.load_hamradios = load_ham_;
    o.load_repeaters = load_repeaters_;
    return o;
}

void ReconView::load_persisted_settings() {
    auto& s = core::settings();
    input_file_ = s.get_string(kSection, "input_file", "RECON");
    output_file_ = s.get_string(kSection, "output_file", "RECON_RESULTS");
    autosave_ = s.get_bool(kSection, "autosave", true);
    autostart_ = s.get_bool(kSection, "autostart", true);
    clear_output_ = s.get_bool(kSection, "clear_output", false);
    load_freqs_ = s.get_bool(kSection, "load_freqs", true);
    load_ranges_ = s.get_bool(kSection, "load_ranges", true);
    load_ham_ = s.get_bool(kSection, "load_hamradios", true);
    load_repeaters_ = s.get_bool(kSection, "load_repeaters", true);
    update_ranges_ = s.get_bool(kSection, "update_ranges", true);
}

void ReconView::set_scanner_mode(bool scanner) {
    scanner_mode_ = scanner;
    if (scanner_mode_) {
        button_scanner_mode_.set_text("SCAN");
        button_scanner_mode_.set_fg_color(ui::Color::red());
        button_remove_.set_text("<DELETE>");
        button_add_.hidden(true);
    } else {
        button_scanner_mode_.set_text(manual_mode_ ? "MANUAL" : "RECON");
        button_scanner_mode_.set_fg_color(ui::Color::blue());
        button_remove_.set_text(manual_mode_ ? "<DELETE>" : "<REMOVE>");
        button_add_.hidden(false);
    }
}

void ReconView::set_loop_config(bool continuous) {
    engine_.set_continuous(continuous);
    button_loop_.set_fg_color(continuous ? ui::Color::green() : ui::Color::light_grey());
    core::settings().set_bool(kSection, "continuous", continuous);
}

void ReconView::frequency_file_load() {
    manual_mode_ = false;

    const std::string& file_input = scanner_mode_ ? output_file_ : input_file_;
    file_name_.set(file_input + "=>" + output_file_);

    core::freqman_db db;
    if (!core::load_freqman_file(file_input, db, make_load_options()) || db.empty()) {
        engine_.set_list(core::freqman_db{});
        file_name_.set_style(ui::Theme::getInstance()->fg_red);
        desc_cycle_.set("...empty file...");
        last_index_ = -1;
        return;
    }

    engine_.set_list(std::move(db));
    engine_.set_forward(engine_.forward());
    engine_.prepare_start();
    last_index_ = -1;
    last_applied_mod_ = -1;

    /* Recon opens the receiver squelch fully; the engine gates on channel
     * level, not the DSP squelch. */
    receiver_.set_squelch_level(0);

    handle_retune();
}

void ReconView::reload_restart() {
    frequency_file_load();
    if (engine_.has_entries()) engine_.resume();
    button_dir_.set_text(engine_.forward() ? "FW>" : "<RW");
}

void ReconView::start_manual_search() {
    scanner_mode_ = false;
    manual_mode_ = true;
    engine_.pause();

    if (range_min_ == 0 || range_max_ == 0) {
        ui::display_modal("Error", "Both START and END freqs\nneed a value");
        return;
    }
    if (range_min_ > range_max_) {
        std::swap(range_min_, range_max_);
        button_manual_start_.set_text(to_string_short_freq(static_cast<uint64_t>(range_min_)));
        button_manual_end_.set_text(to_string_short_freq(static_cast<uint64_t>(range_max_)));
    }

    core::freqman_db db;
    auto entry = std::make_unique<core::freqman_entry>();
    entry->type = core::freqman_type::Range;
    entry->frequency_a = range_min_;
    entry->frequency_b = range_max_;
    entry->description = "MANUAL " +
                         to_string_short_freq(static_cast<uint64_t>(range_min_)) + ">" +
                         to_string_short_freq(static_cast<uint64_t>(range_max_));
    entry->modulation = core::freqman_invalid_index;
    entry->bandwidth = core::freqman_invalid_index;
    entry->step = core::freqman_invalid_index;
    db.push_back(std::move(entry));

    engine_.set_list(std::move(db));
    engine_.prepare_start();
    last_index_ = -1;

    set_scanner_mode(false);
    button_scanner_mode_.set_text("MANUAL");
    button_scanner_mode_.set_fg_color(ui::Color::light_grey());
    file_name_.set("MANUAL => " + output_file_);

    receiver_.set_squelch_level(0);
    handle_retune();
    engine_.resume();
}

void ReconView::apply_mode(int32_t modulation) {
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
        case kModSPEC:
            receiver_.set_mode(Mode::SpectrumAnalysis);
            break;
        default:
            return;
    }
    last_applied_mod_ = modulation;
    update_bandwidth_options(modulation);
}

void ReconView::update_bandwidth_options(int32_t modulation) {
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
            selected = 0;
            break;
    }
    field_bw_.set_options(std::move(options));
    field_bw_.set_selected_index(selected, false);
}

void ReconView::handle_retune() {
    receiver_.set_target_frequency(static_cast<uint64_t>(engine_.freq()));
    big_freq_.set(static_cast<uint64_t>(engine_.freq()));

    if (!engine_.has_entries()) return;

    if (engine_.current_index() != last_index_) {
        last_index_ = engine_.current_index();
        if (engine_.current_is_valid()) {
            const auto& e = engine_.current_entry();
            /* Per-entry modulation, if the entry carries one. */
            if (core::is_valid(e.modulation) && e.modulation <= kModSPEC &&
                e.modulation != last_applied_mod_) {
                field_mode_.set_by_value(e.modulation, false);
                apply_mode(e.modulation);
            }
            /* Description with a type tag, like upstream. */
            std::string prefix;
            switch (e.type) {
                case core::freqman_type::Range: prefix = "R: "; break;
                case core::freqman_type::HamRadio: prefix = "H: "; break;
                case core::freqman_type::Repeater: prefix = "L: "; break;
                default: prefix = "S: "; break;
            }
            desc_cycle_.set(e.description.empty() ? "...no description..."
                                                  : prefix + e.description);
        }
    }
}

void ReconView::recon_save_freq() {
    if (!engine_.has_entries() || !engine_.current_is_valid()) return;

    core::FreqmanDB db;
    if (!db.open(core::get_freqman_path(output_file_), /*create*/ true)) return;

    core::freqman_entry entry = engine_.current_entry();  /* copy */
    if (entry.type == core::freqman_type::Range) {
        /* For a range, store the frequency the scan is currently on. */
        entry.frequency_a = engine_.freq();
        entry.frequency_b = 0;
        entry.type = core::freqman_type::Single;
        entry.step = core::freqman_invalid_index;
    }

    if (db.find_entry(entry) == db.end())
        db.append_entry(entry);  /* append_entry autosaves */
}

void ReconView::handle_remove() {
    if (!engine_.has_entries() || !engine_.current_is_valid()) return;

    if (!scanner_mode_ && !manual_mode_) {
        /* Recon mode: lock the current frequency out for this session. */
        engine_.lockout_current();
        engine_.request_step(engine_.forward() ? 1 : -1);
        return;
    }

    /* Scanner / Manual: delete the entry from the output file. */
    core::freqman_entry entry = engine_.current_entry();
    core::FreqmanDB db;
    if (db.open(core::get_freqman_path(output_file_)))
        db.delete_entry(entry);
}

void ReconView::open_config() {
    auto* nav = app::globals().nav;
    if (!nav) return;

    engine_.pause();
    auto view = std::make_unique<ReconSetupView>(input_file_, output_file_);
    view->on_changed = [this](std::vector<std::string> result) {
        if (result.size() >= 2) {
            input_file_ = result[0];
            output_file_ = result[1];
        }
        load_persisted_settings();
        frequency_file_load();
        if (autostart_)
            engine_.resume();
        else
            engine_.pause();
    };
    nav->push(std::move(view));
}

void ReconView::on_show() {
    View::on_show();
    button_pause_.focus();
    if (!receiver_.running()) receiver_.start();
    tick_started_ = false;
}

void ReconView::on_hide() {
    View::on_hide();
}

void ReconView::on_frame_sync() {
    View::on_frame_sync();

    const auto now = std::chrono::steady_clock::now();
    if (!tick_started_) {
        tick_started_ = true;
        last_tick_ = now;
        return;
    }

    const auto elapsed_ms = static_cast<int32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_tick_).count());
    if (elapsed_ms < kStatsUpdateIntervalMs) return;
    last_tick_ = now;

    /* Nothing loaded: keep the readouts alive but do not step or retune. */
    if (!engine_.has_entries()) {
        update_readouts();
        return;
    }

    const int32_t db = static_cast<int32_t>(std::lround(receiver_.channel_level_db()));
    const bool retuned = engine_.process(db, elapsed_ms);
    if (retuned) handle_retune();

    update_readouts();
}

void ReconView::update_readouts() {
    /* Level bar. */
    float lvl = receiver_.channel_level_db();
    float frac = (lvl - kLevelFloorDb) / (kLevelCeilDb - kLevelFloorDb);
    frac = std::clamp(frac, 0.0f, 1.0f);
    level_meter_.set_value(static_cast<uint8_t>(frac * 255.0f));

    const int32_t db = static_cast<int32_t>(std::lround(lvl));

    if (last_shown_freq_ != engine_.freq()) {
        last_shown_freq_ = engine_.freq();
        big_freq_.set(static_cast<uint64_t>(engine_.freq()));
    }

    if (last_shown_size_ != engine_.size()) {
        last_shown_size_ = engine_.size();
    }
    text_max_.set("/" + to_string_dec_uint(engine_.size()) + " " +
                  to_string_dec_int(db) + "dB");

    if (last_shown_lock_ != engine_.freq_lock() ||
        last_shown_status_ != engine_.status()) {
        last_shown_lock_ = engine_.freq_lock();
        last_shown_status_ = engine_.status();
        text_nb_locks_.set(to_string_dec_uint(engine_.freq_lock()) + "/" +
                           to_string_dec_uint(engine_.lock_nb_match()));

        if (!engine_.recon()) {
            big_freq_.set_style(ui::Theme::getInstance()->bg_darkest);
            button_pause_.set_text("<RESUME>");
        } else if (engine_.freq_lock() == 0) {
            big_freq_.set_style(ui::Theme::getInstance()->bg_darkest);
            button_pause_.set_text("PAUSE");
        } else if (engine_.freq_lock() >= engine_.lock_nb_match()) {
            big_freq_.set_style(ui::Theme::getInstance()->fg_green);
            button_pause_.set_text("<SKIP>");
        } else {
            big_freq_.set_style(ui::Theme::getInstance()->fg_yellow);
            button_pause_.set_text("<SKIP>");
        }
    }

    if (last_shown_timer_ != engine_.timer()) {
        last_shown_timer_ = engine_.timer();
        text_timer_.set("T:" + to_string_dec_int(engine_.timer()));
    }

    if (engine_.current_is_valid()) {
        int idx = engine_.current_index() + 1;
        freq_stats_.set(to_string_dec_int(idx) + "/" +
                        to_string_dec_uint(engine_.size()) + "  " +
                        (engine_.recon() ? "SCAN" : "HOLD"));
    }
}

}  // namespace app

/* --- Registration --------------------------------------------------------- */

namespace {
const app::Registrar reg_recon{{"recon", "Recon", app::Category::Home,
                                ui::Color::green(), &ui::bitmap_icon_scanner,
                                [] { return std::make_unique<app::ReconView>(); }}};
}  // namespace
