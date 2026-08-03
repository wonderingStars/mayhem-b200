/*
 * mayhem-b200 — Replay: transmit a captured IQ file or a .PPL playlist.
 *
 * Ported from firmware/application/apps/ui_playlist.* .
 *
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2023 Kyle Reed, zxkmm
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_playlist.hpp"

#include "../core/file_path.hpp"
#include "../core/fs_utils.hpp"
#include "../core/string_format.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_navigation.hpp"

#include <memory>
#include <utility>

namespace app {

/* --- construction ---------------------------------------------------------- */

PlaylistView::PlaylistView() {
    add_children({&text_filename_, &labels_, &field_freq_, &text_rate_,
                  &text_duration_, &check_loop_, &text_track_,
                  &bar_track_, &bar_file_,
                  &button_play_, &button_prev_, &button_next_,
                  &button_add_, &button_delete_, &button_open_, &button_save_,
                  &console_});

    check_loop_.set_value(true);

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_freq_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                              static_cast<uint64_t>(caps.tx_freq.max));
    }
    if (auto* tx = globals().transmitter)
        field_freq_.set_value(tx->target_frequency(), false);

    field_freq_.on_change = [this](uint64_t f) {
        if (auto* e = current()) e->info.metadata.center_frequency = f;
    };

    button_play_.on_select = [this](ui::Button&) { toggle(); };

    button_prev_.on_select = [this](ui::Button&) {
        if (playing_ || entries_.empty()) return;
        index_ = (index_ == 0) ? entries_.size() - 1 : index_ - 1;
        update_ui();
    };
    button_next_.on_select = [this](ui::Button&) {
        if (playing_ || entries_.empty()) return;
        index_ = (index_ + 1 >= entries_.size()) ? 0 : index_ + 1;
        update_ui();
    };

    button_add_.on_select = [this](ui::Button&) {
        if (playing_) return;
        auto browser = std::make_unique<FileBrowserView>(
            core::captures_directory(), std::vector<std::string>{".C16", ".C8"});
        browser->on_selected = [this](const std::string& p) { add_capture(p); };
        if (auto* nav = globals().nav) nav->push(std::move(browser));
    };
    button_delete_.on_select = [this](ui::Button&) {
        if (playing_) return;
        delete_entry();
    };
    button_open_.on_select = [this](ui::Button&) {
        if (playing_) stop();
        core::ensure_directory(playlists_directory());
        auto browser = std::make_unique<FileBrowserView>(
            playlists_directory(), std::vector<std::string>{".PPL"});
        browser->on_selected = [this](const std::string& p) { load_playlist(p); };
        if (auto* nav = globals().nav) nav->push(std::move(browser));
    };
    button_save_.on_select = [this](ui::Button&) {
        if (playing_) stop();
        save_playlist();
    };

    console_.enable_scrolling(true);
    log(STR_COLOR_LIGHT_GREY "Replay: IQ file -> B200 TX.");
    log(STR_COLOR_LIGHT_GREY "RF output needs a USRP B200;");
    log(STR_COLOR_LIGHT_GREY "unverified without hardware.");

    update_ui();
}

PlaylistView::~PlaylistView() {
    replay_.stop();
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    replay_.clear_sink();
}

/* --- view hooks ------------------------------------------------------------ */

void PlaylistView::on_show() {
    View::on_show();
    if (entries_.empty())
        button_add_.focus();
    else
        button_play_.focus();
}

void PlaylistView::on_hide() {
    stop();
    View::on_hide();
}

void PlaylistView::on_frame_sync() {
    View::on_frame_sync();
    if (!playing_) return;

    /* Honour the inter-track delay without freezing the UI. */
    if (waiting_delay_) {
        if (std::chrono::steady_clock::now() >= delay_deadline_) {
            waiting_delay_ = false;
            replay_.play();
        }
        return;
    }

    bar_file_.set_value(static_cast<uint32_t>(replay_.progress() * 1000.0));

    /* A read failure mid-playback stops the DSP thread with finished()==false
     * and an error string set. */
    if (replay_.state() == radio::ReplayModel::State::Stopped &&
        !replay_.finished() && !replay_.error().empty()) {
        log(STR_COLOR_RED "Replay error:");
        log(truncate(replay_.error(), 28));
        stop();
        return;
    }

    if (replay_.finished()) {
        if (next_track())
            send_current_track();
        else
            stop();
    }
}

/* --- playlist management --------------------------------------------------- */

PlaylistView::Entry* PlaylistView::current() {
    if (entries_.empty() || index_ >= entries_.size()) return nullptr;
    return &entries_[index_];
}

bool PlaylistView::at_end() const {
    return index_ >= entries_.size();
}

void PlaylistView::load_playlist(const std::string& path) {
    stop();
    entries_.clear();
    index_ = 0;
    playlist_path_ = path;
    dirty_ = false;

    std::vector<std::string> lines;
    if (!core::read_lines(path, lines)) {
        log(STR_COLOR_RED "Cannot read playlist.");
        update_ui();
        return;
    }

    for (const auto& ppl : parse_ppl(lines)) {
        Entry e;
        e.path = ppl.path;
        e.delay_ms = ppl.delay_ms;
        e.info = core::inspect_iq_file(ppl.path);
        entries_.push_back(std::move(e));
    }

    log(to_string_dec_uint(entries_.size()) +
        (entries_.size() == 1 ? " entry loaded." : " entries loaded."));
    update_ui();
}

void PlaylistView::add_capture(const std::string& path) {
    if (playlist_path_.empty()) {
        core::ensure_directory(playlists_directory());
        const std::string name =
            core::unique_filename(playlists_directory(), "PLAY_0001.PPL");
        playlist_path_ = core::path_join(playlists_directory(),
                                         name.empty() ? "PLAY.PPL" : name);
    }

    Entry e;
    e.path = path;
    e.delay_ms = 0;
    e.info = core::inspect_iq_file(path);
    entries_.push_back(std::move(e));
    index_ = entries_.size() - 1;
    dirty_ = true;
    update_ui();
}

void PlaylistView::delete_entry() {
    if (entries_.empty()) return;
    entries_.erase(entries_.begin() + static_cast<ptrdiff_t>(index_));
    if (index_ > 0) --index_;
    dirty_ = true;
    update_ui();
}

void PlaylistView::save_playlist() {
    if (entries_.empty()) {
        log(STR_COLOR_DARK_YELLOW "Nothing to save.");
        return;
    }
    if (playlist_path_.empty()) {
        core::ensure_directory(playlists_directory());
        playlist_path_ = core::path_join(playlists_directory(), "PLAY.PPL");
    }

    std::string content;
    for (const auto& e : entries_)
        content += format_ppl_line({e.path, e.delay_ms}) + "\n";

    core::ensure_directory(core::parent_path(playlist_path_));
    if (!core::write_file(playlist_path_, content)) {
        log(STR_COLOR_RED "Save failed.");
        return;
    }

    dirty_ = false;
    log("Saved " + core::filename(playlist_path_));
}

/* --- transport ------------------------------------------------------------- */

void PlaylistView::toggle() {
    if (playing_)
        stop();
    else
        start();
}

void PlaylistView::start() {
    if (entries_.empty()) {
        log(STR_COLOR_DARK_YELLOW "Nothing to play.");
        return;
    }
    playing_ = true;
    index_ = 0;
    send_current_track();
}

void PlaylistView::stop() {
    replay_.stop();
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    replay_.clear_sink();
    ring_.clear();
    playing_ = false;
    waiting_delay_ = false;
    bar_file_.set_value(0);
    update_ui();
}

bool PlaylistView::next_track() {
    ++index_;
    if (at_end()) {
        index_ = 0;
        return check_loop_.value();  /* keep going only if looping */
    }
    return true;
}

void PlaylistView::send_current_track() {
    replay_.stop();
    auto* tx = globals().transmitter;
    if (tx) tx->stop();
    ring_.clear();
    waiting_delay_ = false;

    Entry* e = current();
    if (!e) return;

    if (!replay_.open(e->path)) {
        log(STR_COLOR_RED "Open failed:");
        log(truncate(replay_.error(), 28));
        stop();
        return;
    }

    const double rate = replay_.file_sample_rate();
    replay_.set_output_sample_rate(0.0);  /* transmit at the file's native rate */
    replay_.set_ring_sink(&ring_);

    if (!tx) {
        log(STR_COLOR_RED "No transmitter wired.");
        stop();
        return;
    }

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(rate);
    const uint64_t freq = e->info.has_metadata ? e->info.metadata.center_frequency
                                               : field_freq_.value();
    tx->set_target_frequency(freq);
    tx->set_iq_source([this](std::complex<float>* out, size_t n) {
        return ring_.read(out, n);
    });

    if (!tx->start()) {
        log(STR_COLOR_RED "TX start failed:");
        if (auto* r = globals().radio)
            log(truncate(r->last_error(), 28));
        log(STR_COLOR_LIGHT_GREY "Needs a USRP B200.");
        stop();
        return;
    }

    if (e->delay_ms > 0) {
        waiting_delay_ = true;
        delay_deadline_ = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(e->delay_ms);
        log("Track " + to_string_dec_uint(index_ + 1) + ": wait " +
            to_string_dec_uint(e->delay_ms) + "ms");
    } else {
        replay_.play();
    }

    update_ui();
}

/* --- ui -------------------------------------------------------------------- */

void PlaylistView::update_ui() {
    if (entries_.empty()) {
        text_filename_.set("-");
        text_rate_.set("-");
        text_duration_.set("-");
        text_track_.set(playlist_path_.empty() ? "Open playlist or add capture."
                                               : core::filename(playlist_path_));
        bar_track_.set_max(0);
        bar_track_.set_value(0);
        bar_file_.set_max(0);
        bar_file_.set_value(0);
    } else {
        if (index_ >= entries_.size()) index_ = 0;
        Entry* e = current();

        text_filename_.set(core::filename(e->path));

        const double sr = e->info.sample_rate;
        if (e->info.has_metadata && sr > 0.0) {
            if (sr >= 1e6)
                text_rate_.set(to_string_decimal(static_cast<float>(sr / 1e6), 2) + "M");
            else
                text_rate_.set(to_string_decimal(static_cast<float>(sr / 1e3), 0) + "k");
            field_freq_.set_value(e->info.metadata.center_frequency, false);
        } else {
            text_rate_.set(STR_COLOR_DARK_YELLOW "no meta");
        }

        if (e->info.duration_ms > 0)
            text_duration_.set(to_string_time_ms(e->info.duration_ms));
        else
            text_duration_.set("-");

        text_track_.set(to_string_dec_uint(index_ + 1) + "/" +
                        to_string_dec_uint(entries_.size()) + " " +
                        core::filename(playlist_path_));

        bar_track_.set_max(entries_.size() > 1
                               ? static_cast<uint32_t>(entries_.size() - 1)
                               : 1);
        bar_track_.set_value(static_cast<uint32_t>(index_));
        bar_file_.set_max(1000);
    }

    button_play_.set_text(playing_ ? "Stop" : "Play");
}

void PlaylistView::log(std::string_view line) {
    console_.writeln(line);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_replay{{
    "replay", "Replay", app::Category::Home,
    ui::Color::green(), &ui::bitmap_icon_replay,
    [] { return std::make_unique<app::PlaylistView>(); }}};
}  // namespace
