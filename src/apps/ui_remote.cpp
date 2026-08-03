/*
 * mayhem-b200 — Remote: user-defined button panel replaying saved captures.
 *
 * Ported from firmware/application/external/remote/ui_remote.* .
 *
 * Copyright (C) 2023 Kyle Reed (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_remote.hpp"

#include "../core/fs_utils.hpp"
#include "../core/settings.hpp"
#include "../core/string_format.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "bitmaps.hpp"
#include "input.hpp"
#include "theme.hpp"
#include "ui_alphanum.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"
#include "ui_playlist_editor.hpp"  /* app::FileBrowserView */

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace app {

/* --- comma-format helpers -------------------------------------------------- */

std::vector<std::string_view> remote_split(std::string_view str, char c) {
    /* firmware/application/file_reader.cpp split_string, byte-for-byte. */
    std::vector<std::string_view> cols;
    size_t start = 0;

    while (start < str.length()) {
        auto it = str.find(c, start);
        if (it == std::string_view::npos)
            break;
        cols.emplace_back(str.substr(start, it - start));
        start = it + 1;
    }

    if (start <= str.length() && !str.empty())
        cols.emplace_back(str.substr(start, str.length() - start));

    return cols;
}

std::string remote_join(char c, std::initializer_list<std::string_view> parts) {
    /* firmware/common/utility.cpp join, byte-for-byte. */
    std::string result;
    size_t total_size = parts.size();
    for (auto s : parts)
        total_size += s.size();

    result.reserve(total_size);
    bool first = true;
    for (auto s : parts) {
        if (!first)
            result += c;
        else
            first = false;
        result += s;
    }
    return result;
}

uint64_t remote_parse_uint(std::string_view s) {
    /* firmware/common/convert.hpp parse_int semantics: decimal, 0 on garbage,
     * strtoull stops at the first non-digit. */
    if (s.empty())
        return 0;

    std::string z{s};
    return std::strtoull(z.c_str(), nullptr, 10);
}

namespace {

/* checked_assign from firmware/common/convert.hpp: assign only if the value fits
 * the target type, otherwise leave it zero. */
template <typename T>
void assign_ranged(std::string_view s, T& out) {
    const uint64_t v = remote_parse_uint(s);
    out = (v <= static_cast<uint64_t>(std::numeric_limits<T>::max()))
              ? static_cast<T>(v)
              : T{0};
}

}  // namespace

/* --- icon / colour tables -------------------------------------------------- */

size_t remote_icon_count() { return 25; }

const ui::Bitmap* remote_icon_at(size_t index) {
    /* Same 25 index slots as upstream RemoteIcons. Slots whose bitmap the
     * trimmed host set does not carry are nullptr — a plain tile, never a wrong
     * icon — but the index still round-trips through the .REM file. */
    static const ui::Bitmap* const table[25] = {
        nullptr,                               // 0
        nullptr,                               // 1  fox      (absent)
        &ui::bitmap_icon_adsb,                 // 2
        &ui::bitmap_icon_ais,                  // 3
        &ui::bitmap_icon_aprs,                 // 4
        &ui::bitmap_icon_btle,                 // 5
        nullptr,                               // 6  burger   (absent)
        nullptr,                               // 7  camera   (absent)
        nullptr,                               // 8  cwgen    (absent)
        nullptr,                               // 9  dmr      (absent)
        nullptr,                               // 10 file_img (absent)
        nullptr,                               // 11 lge      (absent)
        &ui::bitmap_icon_looking,              // 12
        nullptr,                               // 13 memory   (absent)
        nullptr,                               // 14 morse    (absent)
        nullptr,                               // 15 nrf      (absent)
        &ui::bitmap_icon_notepad,              // 16
        &ui::bitmap_icon_rds,                  // 17
        &ui::bitmap_icon_remote,               // 18
        &ui::bitmap_icon_setup,                // 19
        nullptr,                               // 20 sleep    (absent)
        &ui::bitmap_icon_sonde,                // 21
        nullptr,                               // 22 stealth  (absent)
        nullptr,                               // 23 tetra    (absent)
        &ui::bitmap_icon_peripherals_details,  // 24
    };
    if (index >= 25)
        return nullptr;
    return table[index];
}

size_t remote_color_count() { return 21; }

ui::Color remote_color_at(size_t index) {
    /* Upstream RemoteColors, byte-for-byte. */
    using ui::Color;
    static const Color table[21] = {
        Color::black(),         // 0
        Color::white(),         // 1
        Color::darker_grey(),   // 2
        Color::dark_grey(),     // 3
        Color::grey(),          // 4
        Color::light_grey(),    // 5
        Color::red(),           // 6
        Color::orange(),        // 7
        Color::yellow(),        // 8
        Color::green(),         // 9
        Color::blue(),          // 10
        Color::cyan(),          // 11
        Color::magenta(),       // 12
        Color::dark_red(),      // 13
        Color::dark_orange(),   // 14
        Color::dark_yellow(),   // 15
        Color::dark_green(),    // 16
        Color::dark_blue(),     // 17
        Color::dark_cyan(),     // 18
        Color::dark_magenta(),  // 19
        Color::purple(),        // 20
    };
    if (index >= 21)
        index = 0;
    return table[index];
}

/* --- RemoteEntryModel ------------------------------------------------------ */

std::string RemoteEntryModel::to_string() const {
    return remote_join(',', {path,
                             name,
                             to_string_dec_uint(icon),
                             to_string_dec_uint(bg_color),
                             to_string_dec_uint(fg_color),
                             to_string_dec_uint(center_frequency),
                             to_string_dec_uint(sample_rate)});
}

std::optional<RemoteEntryModel> RemoteEntryModel::parse(std::string_view line) {
    const auto cols = remote_split(line, ',');
    if (cols.size() < 7)
        return std::nullopt;

    RemoteEntryModel entry{};
    entry.path = std::string{cols[0]};
    entry.name = std::string{cols[1]};
    assign_ranged(cols[2], entry.icon);
    assign_ranged(cols[3], entry.bg_color);
    assign_ranged(cols[4], entry.fg_color);
    assign_ranged(cols[5], entry.center_frequency);
    assign_ranged(cols[6], entry.sample_rate);
    return entry;
}

/* --- RemoteModel ----------------------------------------------------------- */

bool RemoteModel::delete_entry(const RemoteEntryModel* entry) {
    /* Expects a pointer into `entries`. */
    auto it = std::find_if(
        entries.begin(), entries.end(),
        [entry](const auto& item) { return entry == &item; });
    if (it == entries.end())
        return false;
    entries.erase(it);
    return true;
}

RemoteEntryModel* RemoteModel::entry_at(size_t index) {
    return index < entries.size() ? &entries[index] : nullptr;
}

const RemoteEntryModel* RemoteModel::entry_at(size_t index) const {
    return index < entries.size() ? &entries[index] : nullptr;
}

int RemoteModel::index_of(const RemoteEntryModel* entry) const {
    for (size_t i = 0; i < entries.size(); ++i)
        if (&entries[i] == entry)
            return static_cast<int>(i);
    return -1;
}

const RemoteEntryModel* RemoteModel::find_by_name(std::string_view entry_name) const {
    for (const auto& e : entries)
        if (e.name == entry_name)
            return &e;
    return nullptr;
}

bool RemoteModel::load(const std::string& path) {
    std::vector<std::string> lines;
    if (!core::read_lines(path, lines))
        return false;

    entries.clear();

    bool first = true;
    for (const auto& line : lines) {
        if (line.empty() || line[0] == '#')
            continue;  // Empty or comment line.

        if (first) {
            name = trim(line);  // First content line is the remote name.
            first = false;
            continue;
        }

        if (auto entry = RemoteEntryModel::parse(line))
            entries.push_back(std::move(*entry));
    }

    return true;
}

bool RemoteModel::save(const std::string& path) const {
    std::string content;
    content += name;
    content += '\n';
    for (const auto& entry : entries) {
        content += entry.to_string();
        content += '\n';
    }
    return static_cast<bool>(core::write_file(path, content));
}

/* --- RemoteButton ---------------------------------------------------------- */

RemoteButton::RemoteButton(ui::Rect parent_rect, RemoteEntryModel* entry)
    : ui::NewButton{parent_rect, {}, nullptr} {
    set_entry(entry);
    /* Route short-press / touch / keyboard through on_select2, so on_key only
     * has to add the long-press case. Mirrors upstream. */
    on_select = [this]() {
        if (on_select2)
            on_select2(*this);
    };
}

void RemoteButton::set_entry(RemoteEntryModel* entry) {
    entry_ = entry;
    set_focusable(entry_ != nullptr);
    hidden(entry_ == nullptr);

    if (entry_) {
        set_text(entry_->name);
        set_bitmap(remote_icon_at(entry_->icon));
    }
    set_dirty();
}

bool RemoteButton::on_key(ui::KeyEvent key) {
    if (key == ui::KeyEvent::Select) {
        if (ui::key_is_long_pressed(key) && on_long_select) {
            on_long_select(*this);
            return true;
        }
        if (on_select2) {
            on_select2(*this);
            return true;
        }
    }
    return false;
}

void RemoteButton::paint(ui::Painter& painter) {
    ui::NewButton::paint(painter);

    // Border on the highlighted / focused button.
    if (has_focus() || highlighted()) {
        const auto r = screen_rect();
        painter.draw_rectangle(r, ui::Theme::getInstance()->bg_darkest->foreground);

        const auto p = r.location() + ui::Point{1, 1};
        const auto s = ui::Size{static_cast<ui::Dim>(r.size().width() - 2),
                                static_cast<ui::Dim>(r.size().height() - 2)};
        painter.draw_rectangle({p, s}, ui::Theme::getInstance()->fg_light->foreground);
    }
}

ui::Style RemoteButton::paint_style() {
    if (!entry_)
        return style();

    ui::MutableStyle s{style()};
    s.foreground = remote_color_at(entry_->fg_color);
    s.background = remote_color_at(entry_->bg_color);

    if (has_focus() || highlighted())
        s.invert();

    // NewButton tints the bitmap with color_; keep it in step with the style.
    color_ = s.foreground;
    return s;
}

/* --- RemoteEntryEditView --------------------------------------------------- */

RemoteEntryEditView::RemoteEntryEditView(RemoteEntryModel& entry)
    : entry_{entry} {
    add_children({&labels_, &field_name_, &field_path_, &field_freq_, &text_rate_,
                  &field_icon_index_, &field_fg_color_index_, &field_bg_color_index_,
                  &button_preview_, &button_delete_, &button_done_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_freq_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                              static_cast<uint64_t>(caps.tx_freq.max));
    }

    field_name_.on_select = [this](ui::TextField&) {
        auto* nav = globals().nav;
        if (!nav) return;
        name_buffer_ = entry_.name;
        ui::text_prompt(*nav, name_buffer_, 30, ENTER_KEYBOARD_MODE_ALPHA,
                        [this](std::string& v) {
                            entry_.name = v;
                            field_name_.set_text(v);
                            button_preview_.set_text(v);
                            button_preview_.set_dirty();
                        });
    };

    field_path_.on_select = [this](ui::TextField&) {
        auto* nav = globals().nav;
        if (!nav) return;
        core::ensure_directory(core::captures_directory());
        auto browser = std::make_unique<FileBrowserView>(
            core::captures_directory(), std::vector<std::string>{".C16", ".C8"});
        browser->on_selected = [this](const std::string& p) {
            load_path(p);
            refresh_ui();
        };
        nav->push(std::move(browser));
    };

    field_freq_.on_change = [this](uint64_t f) { entry_.center_frequency = f; };

    field_icon_index_.on_change = [this](int32_t v) {
        entry_.icon = static_cast<uint8_t>(v);
        button_preview_.set_bitmap(remote_icon_at(static_cast<size_t>(v)));
        button_preview_.set_dirty();
    };
    field_fg_color_index_.on_change = [this](int32_t v) {
        entry_.fg_color = static_cast<uint8_t>(v);
        button_preview_.set_dirty();
    };
    field_bg_color_index_.on_change = [this](int32_t v) {
        entry_.bg_color = static_cast<uint8_t>(v);
        button_preview_.set_dirty();
    };

    button_delete_.on_select = [this](ui::Button&) {
        auto* nav = globals().nav;
        if (!nav) return;
        ui::display_modal(
            *nav, "Delete?", "Delete this button?", ui::YESNO,
            [this, nav](bool choice) {
                if (!choice) return;
                // Detach the preview before the entry is erased.
                button_preview_.set_entry(nullptr);
                if (on_delete) on_delete(entry_);
                nav->pop();
            });
    };

    button_done_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };

    refresh_ui();
}

void RemoteEntryEditView::focus() {
    field_name_.focus();
}

void RemoteEntryEditView::refresh_ui() {
    field_name_.set_text(entry_.name);
    field_path_.set_text(entry_.path.empty() ? "(none)"
                                             : core::filename(entry_.path));
    field_freq_.set_value(entry_.center_frequency, false);
    field_icon_index_.set_value(entry_.icon, false);
    field_fg_color_index_.set_value(entry_.fg_color, false);
    field_bg_color_index_.set_value(entry_.bg_color, false);

    const uint32_t rate = entry_.sample_rate;
    if (rate >= 1'000'000)
        text_rate_.set(to_string_decimal(static_cast<float>(rate) / 1e6f, 2) + "M");
    else if (rate >= 1'000)
        text_rate_.set(to_string_decimal(static_cast<float>(rate) / 1e3f, 0) + "k");
    else
        text_rate_.set(to_string_dec_uint(rate));

    button_preview_.set_entry(&entry_);
}

void RemoteEntryEditView::load_path(std::string path) {
    // Prefer the .TXT sidecar; fall back to the TX frequency and 500 kHz.
    core::CaptureMetadata md{};
    if (core::read_metadata_file(core::metadata_path_for(path), md)) {
        entry_.center_frequency = md.center_frequency;
        entry_.sample_rate = md.sample_rate;
    } else {
        if (auto* tx = globals().transmitter)
            entry_.center_frequency = tx->target_frequency();
        entry_.sample_rate = 500'000;
    }
    entry_.path = std::move(path);
}

/* --- RemoteAppView --------------------------------------------------------- */

RemoteAppView::RemoteAppView() {
    add_children({&field_title_, &field_filename_, &check_loop_, &console_,
                  &button_add_, &button_new_, &button_open_});

    create_buttons();

    check_loop_.set_value(false);

    field_title_.on_select = [this](ui::TextField&) {
        auto* nav = globals().nav;
        if (!nav) return;
        name_buffer_ = model_.name;
        ui::text_prompt(*nav, name_buffer_, 30, ENTER_KEYBOARD_MODE_ALPHA,
                        [this](std::string& v) {
                            model_.name = v;
                            needs_save_ = true;
                            refresh_ui();
                        });
    };

    field_filename_.on_select = [this](ui::TextField&) {
        auto* nav = globals().nav;
        if (!nav) return;
        name_buffer_ = core::stem(remote_path_);
        ui::text_prompt(*nav, name_buffer_, 30, ENTER_KEYBOARD_MODE_ALPHA,
                        [this](std::string& v) {
                            rename_remote(v);
                            refresh_ui();
                        });
    };

    button_add_.on_select = [this](ui::Button&) { add_button(); };
    button_new_.on_select = [this](ui::Button&) { new_remote(); };
    button_open_.on_select = [this](ui::Button&) { open_remote(); };

    console_.enable_scrolling(true);
    log(STR_COLOR_LIGHT_GREY "Remote: replay captures via TX.");
    log(STR_COLOR_LIGHT_GREY "Long-press a button to edit.");
    log(STR_COLOR_LIGHT_GREY "RF output needs a USRP B200.");

    core::ensure_directory(remotes_directory());

    // Restore the previously used remote, else start a fresh one.
    const std::string last = core::settings().get_string("remote", "path", "");
    if (last.empty() || !load_remote(last))
        init_remote();

    refresh_ui();
}

RemoteAppView::~RemoteAppView() {
    stop();
    save_remote(/*show_errors*/ false);
    core::settings().set_string("remote", "path", remote_path_);
    core::settings().save();
}

void RemoteAppView::on_show() {
    View::on_show();
    refresh_ui();  // A returning edit view may have changed the model.
    if (model_.entries.empty())
        button_add_.focus();
    else if (!buttons_.empty())
        buttons_[0]->focus();
}

void RemoteAppView::on_hide() {
    stop();
    View::on_hide();
}

void RemoteAppView::on_frame_sync() {
    View::on_frame_sync();
    if (!sending_) return;

    // A read failure mid-send stops the DSP thread with finished()==false and an
    // error string set.
    if (replay_.state() == radio::ReplayModel::State::Stopped &&
        !replay_.finished() && !replay_.error().empty()) {
        log(STR_COLOR_RED "Send error:");
        log(truncate(replay_.error(), 28));
        stop();
        return;
    }

    // When not looping, a finished file ends the send. (Looping is handled
    // inside ReplayModel, which never reports finished while looping.)
    if (replay_.finished())
        stop();
}

/* --- grid ------------------------------------------------------------------ */

void RemoteAppView::create_buttons() {
    auto handle_send = [this](RemoteButton& btn) {
        if (!btn.entry())
            return;
        if (btn.entry()->path.empty())
            edit_button(btn);  // No capture bound yet? Edit instead.
        else if (is_sending() && &btn == current_btn_)
            stop();  // Same button again? Stop.
        else
            send_button(btn);
    };
    auto handle_edit = [this](RemoteButton& btn) { edit_button(btn); };

    for (size_t i = 0; i < max_buttons; ++i) {
        const ui::Coord x =
            static_cast<ui::Coord>((i % button_cols) * button_width);
        const ui::Coord y = static_cast<ui::Coord>(
            buttons_top + (i / button_cols) * button_height);

        auto btn = std::make_unique<RemoteButton>(
            ui::Rect{x, y, button_width, button_height}, nullptr);
        btn->on_select2 = handle_send;
        btn->on_long_select = handle_edit;

        add_child(btn.get());
        buttons_.push_back(std::move(btn));
    }
}

void RemoteAppView::reset_buttons() {
    // The model's entries vector can reallocate; null the pointers first so a
    // stale one is never dereferenced before refresh_ui() re-points them.
    for (auto& btn : buttons_)
        btn->set_entry(nullptr);
}

void RemoteAppView::refresh_ui() {
    field_title_.set_text(model_.name);
    field_filename_.set_text(core::stem(remote_path_));

    for (size_t i = 0; i < buttons_.size(); ++i) {
        if (i < model_.entries.size())
            buttons_[i]->set_entry(&model_.entries[i]);
        else
            buttons_[i]->set_entry(nullptr);
    }
}

/* --- actions --------------------------------------------------------------- */

void RemoteAppView::add_button() {
    if (model_.entries.size() >= max_buttons)
        return;

    stop();  // Don't mutate the model while the send thread reads it.
    model_.entries.push_back({{}, "<EMPTY>", 0, 3, 1, 0, 0});
    reset_buttons();
    refresh_ui();
    needs_save_ = true;
}

void RemoteAppView::edit_button(RemoteButton& btn) {
    if (!btn.entry())
        return;

    stop();
    auto* nav = globals().nav;
    if (!nav)
        return;

    auto edit = std::make_unique<RemoteEntryEditView>(*btn.entry());
    edit->on_delete = [this](RemoteEntryModel& to_delete) {
        model_.delete_entry(&to_delete);
        reset_buttons();
    };
    needs_save_ = true;  // The edit view writes straight into the entry.
    nav->push(std::move(edit));
}

void RemoteAppView::send_button(RemoteButton& btn) {
    stop();

    auto* e = btn.entry();
    if (!e)
        return;
    current_btn_ = &btn;

    auto* tx = globals().transmitter;
    if (!tx) {
        show_error("No transmitter wired.\nNeeds a USRP B200.");
        return;
    }

    // If the capture has no sidecar, play it at the rate stored in the .REM.
    replay_.set_default_sample_rate(e->sample_rate > 0 ? static_cast<double>(e->sample_rate)
                                                       : 500'000.0);
    if (!replay_.open(e->path)) {
        show_error("Can't open file:\n" + core::filename(e->path));
        return;
    }

    const double rate = replay_.file_sample_rate();
    replay_.set_output_sample_rate(0.0);  // transmit at the file's native rate
    replay_.set_loop(check_loop_.value());
    replay_.set_ring_sink(&ring_);

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(rate);
    const uint64_t freq = e->center_frequency != 0
                              ? e->center_frequency
                              : (replay_.has_metadata() ? replay_.center_frequency()
                                                        : tx->target_frequency());
    tx->set_target_frequency(freq);
    tx->set_iq_source([this](std::complex<float>* out, size_t n) {
        return ring_.read(out, n);
    });

    if (!tx->start()) {
        show_error("TX start failed.\nNeeds a USRP B200.");
        stop();
        return;
    }

    replay_.play();
    sending_ = true;
    log("Sending: " + e->name);
}

void RemoteAppView::stop() {
    replay_.stop();
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    replay_.clear_sink();
    ring_.clear();
    sending_ = false;
    current_btn_ = nullptr;
}

/* --- remote file lifecycle ------------------------------------------------- */

void RemoteAppView::new_remote() {
    save_remote();
    init_remote();
    refresh_ui();
    set_dirty();  // Redraw to hide the old buttons.
}

void RemoteAppView::open_remote() {
    auto* nav = globals().nav;
    if (!nav)
        return;

    stop();
    core::ensure_directory(remotes_directory());
    auto browser = std::make_unique<FileBrowserView>(
        remotes_directory(), std::vector<std::string>{".REM"});
    browser->on_selected = [this](const std::string& p) {
        save_remote();
        load_remote(p);
        refresh_ui();
    };
    nav->push(std::move(browser));
}

void RemoteAppView::init_remote() {
    model_ = RemoteModel{"<Unnamed Remote>", {}};
    reset_buttons();
    core::ensure_directory(remotes_directory());
    const std::string name =
        core::unique_filename(remotes_directory(), "REMOTE_0001.REM");
    set_remote_path(core::path_join(remotes_directory(),
                                    name.empty() ? "REMOTE.REM" : name));
    needs_save_ = false;
}

bool RemoteAppView::load_remote(std::string path) {
    set_remote_path(std::move(path));
    needs_save_ = false;
    reset_buttons();
    return model_.load(remote_path_);
}

void RemoteAppView::save_remote(bool show_errors) {
    if (!needs_save_ || remote_path_.empty())
        return;

    core::ensure_directory(core::parent_path(remote_path_));
    if (!model_.save(remote_path_)) {
        if (show_errors)
            show_error("Save failed for:\n" + core::stem(remote_path_));
        return;
    }
    needs_save_ = false;
}

void RemoteAppView::rename_remote(const std::string& new_name) {
    if (new_name.empty())
        return;

    const std::string folder = core::parent_path(remote_path_);
    const std::string ext = core::extension(remote_path_);
    const std::string new_path = core::path_join(folder, new_name + ext);

    if (core::exists(new_path)) {
        show_error("Remote " + new_name + " already exists");
        return;
    }

    if (core::exists(remote_path_))
        core::rename_path(remote_path_, new_path);

    set_remote_path(new_path);
}

void RemoteAppView::set_remote_path(std::string path) {
    remote_path_ = std::move(path);
}

void RemoteAppView::show_error(const std::string& msg) const {
    ui::display_modal("Error", msg);
}

void RemoteAppView::log(std::string_view line) {
    console_.writeln(line);
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_remote{{
    "remote", "Remote", app::Category::Home,
    ui::Color::green(), &ui::bitmap_icon_remote,
    [] { return std::make_unique<app::RemoteAppView>(); }}};
}  // namespace
