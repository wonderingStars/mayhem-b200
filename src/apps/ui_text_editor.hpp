/*
 * mayhem-b200 — Notepad / text editor (Category::Utilities).
 *
 * A host port of firmware/application/apps/ui_text_editor.* (Kyle Reed,
 * Mark Thompson). The firmware reads a text file through FileWrapper, which keeps
 * a rolling cache of newline byte-offsets so a large file can be scrolled without
 * loading its text into RAM, and does in-place edits by byte-shifting a temp copy.
 *
 * What changed for the host, and why:
 *
 *  - The line-index model is reimplemented here as TextFileModel. Like upstream it
 *    scans the file once for newline offsets and reads line text from disk on
 *    demand, so viewing a large file never holds the whole text. Unlike upstream
 *    it does not byte-shift a temp file on edit: the first edit materialises the
 *    lines into memory (upstream re-reads the whole file on every edit anyway) and
 *    subsequent edits and Save work from that vector via core::write_file. Line
 *    boundaries follow core::read_lines exactly (split on '\n', a trailing '\r'
 *    stripped, no trailing empty line for a file that ends in '\n', no lines for
 *    an empty file) plus a separately-detected final-newline flag so a round trip
 *    preserves the presence or absence of the terminating newline.
 *
 *  - The cursor is drawn by re-painting the character under it with an inverted
 *    style. The firmware XOR's screen pixels via portapack::display.read_pixels /
 *    draw_pixels, which the host Painter does not expose.
 *
 *  - The pop-up NewButton grid and its bitmaps (arrow_left, icon_rename, ...) are
 *    not in the host icon set, so the actions live on a plain Button strip along
 *    the bottom: Open, Edit, +Line, -Line, Save, Exit.
 *
 *  - There is no file picker in the host yet (upstream pushes FileLoadView). Open
 *    prompts for a path with the on-screen / physical keyboard (ui_alphanum). If
 *    the path does not exist it is created empty, so Notepad can start a new file
 *    without a separate file manager — a deliberate host addition, noted on screen.
 *
 *  - Font-zoom (the firmware toggles a 5x8 / 8x16 font) is omitted; the 8x16 font
 *    is used throughout for legibility on a desktop window.
 *
 * Copyright (C) 2023 Kyle Reed
 * Copyright (C) 2023 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_TEXT_EDITOR_H__
#define __MB200_UI_TEXT_EDITOR_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include "fs_utils.hpp"

#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace app {

/* ------------------------------------------------------------------------- *
 * TextFileModel — the line-buffer / model.
 *
 * Header-only and free of any UI dependency so it can be unit tested directly.
 * Two internal representations share one query interface:
 *
 *   Index  : after open(). Holds only the byte offset of each line start plus the
 *            file size and a final-newline flag; line text is read from disk on
 *            demand. This is the "don't load more than the view needs" path.
 *   Buffer : after the first edit (or an explicit materialize()/serialize()/save).
 *            Holds the lines in a std::vector<std::string>; all edits and saves
 *            operate here.
 * ------------------------------------------------------------------------- */
class TextFileModel {
   public:
    TextFileModel() = default;
    ~TextFileModel() { close(); }

    TextFileModel(const TextFileModel&) = delete;
    TextFileModel& operator=(const TextFileModel&) = delete;

    /* Opens the file and builds the newline index. Returns false if the file
     * cannot be read. An empty file is a valid open with zero lines. */
    bool open(const std::string& path) {
        close();

        std::ifstream f(path, std::ios::binary);
        if (!f) return false;

        f.seekg(0, std::ios::end);
        const std::streamoff sz = f.tellg();
        if (sz < 0) return false;

        size_ = static_cast<uint64_t>(sz);
        path_ = path;
        mode_ = Mode::Index;

        if (size_ == 0) {
            count_ = 0;  // matches core::read_lines: empty file -> no lines
            stream_.open(path, std::ios::binary);
            return true;
        }

        f.seekg(0, std::ios::beg);
        starts_.push_back(0);

        constexpr std::streamsize chunk = 65536;
        std::vector<char> buf(static_cast<size_t>(chunk));
        uint64_t abs = 0;
        char last = 0;

        while (true) {
            f.read(buf.data(), chunk);
            const std::streamsize got = f.gcount();
            if (got <= 0) break;

            for (std::streamsize i = 0; i < got; ++i) {
                if (buf[static_cast<size_t>(i)] == '\n') {
                    const uint64_t next_start = abs + static_cast<uint64_t>(i) + 1;
                    // A '\n' that is the final byte does not begin a new line
                    // (no trailing empty line), matching core::read_lines.
                    if (next_start < size_) starts_.push_back(next_start);
                }
            }

            abs += static_cast<uint64_t>(got);
            last = buf[static_cast<size_t>(got - 1)];
            if (got < chunk) break;
        }

        final_nl_ = (last == '\n');
        count_ = starts_.size();

        stream_.open(path, std::ios::binary);
        return stream_.is_open();
    }

    void close() {
        if (stream_.is_open()) stream_.close();
        starts_.clear();
        lines_.clear();
        size_ = 0;
        count_ = 0;
        final_nl_ = false;
        dirty_ = false;
        path_.clear();
        mode_ = Mode::Empty;
    }

    /* True once a file has been opened (even an empty one). */
    bool is_open() const { return mode_ != Mode::Empty; }
    const std::string& path() const { return path_; }

    size_t line_count() const {
        switch (mode_) {
            case Mode::Buffer:
                return lines_.size();
            case Mode::Index:
                return count_;
            default:
                return 0;
        }
    }

    bool has_final_newline() const { return final_nl_; }
    bool dirty() const { return dirty_; }
    void clear_dirty() { dirty_ = false; }

    /* Total bytes the current content would occupy on disk (LF form). */
    uint64_t total_size() const {
        if (mode_ == Mode::Index) return size_;
        if (mode_ == Mode::Buffer) {
            const size_t n = lines_.size();
            if (n == 0) return 0;
            uint64_t s = 0;
            for (const auto& l : lines_) s += l.size();
            s += (n - 1);                 // '\n' separators
            if (final_nl_) s += 1;        // terminating newline
            return s;
        }
        return 0;
    }

    /* Line content with any terminator removed. Empty for an invalid index. */
    std::string get_line(size_t index) {
        if (mode_ == Mode::Buffer)
            return index < lines_.size() ? lines_[index] : std::string{};
        if (mode_ == Mode::Index && index < count_)
            return read_line_from_disk(index);
        return {};
    }

    size_t line_length(size_t index) { return get_line(index).size(); }

    /* Substring of a line for the viewport: from `col`, at most `max_chars`. */
    std::string get_text(size_t index, size_t col, size_t max_chars) {
        std::string line = get_line(index);
        if (col >= line.size()) return {};
        return line.substr(col, max_chars);
    }

    /* --- Edits. Each materialises the buffer first and marks the model dirty. */

    void set_line(size_t index, const std::string& text) {
        materialize();
        if (index < lines_.size()) {
            lines_[index] = text;
            dirty_ = true;
        }
    }

    /* Inserts an empty line so that it becomes line `before`. `before` past the
     * end appends at the end. */
    void insert_line(size_t before) {
        materialize();
        if (before > lines_.size()) before = lines_.size();
        lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(before), std::string{});
        dirty_ = true;
    }

    void delete_line(size_t index) {
        materialize();
        if (index < lines_.size()) {
            lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(index));
            dirty_ = true;
        }
    }

    void append_line(const std::string& text) {
        materialize();
        lines_.push_back(text);
        dirty_ = true;
    }

    /* Forces the in-memory buffer representation. Public for tests; edits call it
     * for you. */
    void materialize() {
        if (mode_ == Mode::Buffer) return;

        std::vector<std::string> tmp;
        if (mode_ == Mode::Index) {
            tmp.reserve(count_);
            for (size_t i = 0; i < count_; ++i)
                tmp.push_back(read_line_from_disk(i));
        }

        if (stream_.is_open()) stream_.close();
        lines_ = std::move(tmp);
        mode_ = Mode::Buffer;
    }

    bool in_memory() const { return mode_ == Mode::Buffer; }

    /* The full byte content as it would be written to disk. */
    std::string serialize() {
        materialize();
        std::string out;
        for (size_t i = 0; i < lines_.size(); ++i) {
            out += lines_[i];
            if (i + 1 < lines_.size()) out += '\n';
        }
        if (final_nl_ && !lines_.empty()) out += '\n';
        return out;
    }

    /* Writes the current content through core::write_file and clears dirty. */
    bool save(const std::string& path) {
        const std::string content = serialize();
        const auto r = core::write_file(path, content);
        if (r) {
            dirty_ = false;
            return true;
        }
        return false;
    }

   private:
    enum class Mode : uint8_t { Empty, Index, Buffer };

    std::string read_line_from_disk(size_t index) {
        if (!stream_.is_open() || index >= count_) return {};

        const uint64_t start = starts_[index];
        uint64_t end;
        if (index + 1 < count_)
            end = starts_[index + 1] - 1;              // exclude the '\n'
        else
            end = final_nl_ ? (size_ - 1) : size_;     // last line

        if (end < start) end = start;
        const uint64_t len = end - start;

        std::string buf(static_cast<size_t>(len), '\0');
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(start));
        stream_.read(&buf[0], static_cast<std::streamsize>(len));
        buf.resize(static_cast<size_t>(stream_.gcount()));

        if (!buf.empty() && buf.back() == '\r') buf.pop_back();  // CRLF
        return buf;
    }

    Mode mode_{Mode::Empty};
    std::string path_{};

    /* Index representation. */
    std::ifstream stream_{};
    std::vector<uint64_t> starts_{};
    uint64_t size_{0};
    size_t count_{0};

    /* Buffer representation. */
    std::vector<std::string> lines_{};

    bool final_nl_{false};
    bool dirty_{false};
};

/* ------------------------------------------------------------------------- *
 * TextViewer — scrolling read view over a TextFileModel.
 * ------------------------------------------------------------------------- */
enum class ScrollDirection : uint8_t { Vertical, Horizontal };

class TextViewer : public ui::Widget {
   public:
    explicit TextViewer(ui::Rect parent_rect);

    std::function<void()> on_select{};
    std::function<void()> on_cursor_moved{};

    void paint(ui::Painter& painter) override;
    bool on_key(const ui::KeyEvent key) override;
    bool on_encoder(const ui::EncoderEvent delta) override;
    void on_focus() override;

    void set_model(TextFileModel* model) { model_ = model; }
    void clear_model() { model_ = nullptr; }
    bool has_model() const { return model_ != nullptr; }

    uint32_t line() const { return cursor_line_; }
    uint32_t col() const { return cursor_col_; }

    void cursor_home();
    void cursor_end();
    void reset_cursor();
    void clamp_cursor();

    void redraw() { set_dirty(); }

   private:
    /* Returns true if the cursor actually moved. */
    bool apply_scrolling_constraints(int16_t delta_line, int16_t delta_col);
    int32_t line_len(int32_t index);

    TextFileModel* model_{nullptr};
    const ui::Style* font_style_{nullptr};
    int16_t char_width_{8};
    int16_t char_height_{16};
    uint32_t max_line_{1};
    uint32_t max_col_{1};

    uint32_t cursor_line_{0};
    uint32_t cursor_col_{0};
    ScrollDirection dir_{ScrollDirection::Vertical};

    uint32_t first_line_{0};
    uint32_t first_col_{0};
};

/* ------------------------------------------------------------------------- *
 * TextEditorView — the app view.
 * ------------------------------------------------------------------------- */
class TextEditorView : public ui::View {
   public:
    TextEditorView();

    std::string title() const override { return "Notepad"; }

    void on_show() override;

   private:
    static constexpr size_t max_edit_length = 1024;
    static constexpr size_t max_path_length = 255;

    void open_file(const std::string& path);
    void show_open_prompt();
    void show_edit_line();
    void refresh_ui();
    void update_position();
    void request_exit();

    TextFileModel model_{};
    std::string path_{};

    std::string open_path_buffer_{};
    std::string pending_open_path_{};
    std::string edit_line_buffer_{};
    size_t edit_line_index_{0};

    TextViewer viewer{{0, 0, 240, 240}};

    ui::Text text_position{{0, 244, 240, 16}, ""};
    ui::Text text_size{{0, 260, 240, 16}, ""};

    ui::Button button_open{{0, 280, 40, 24}, "Open"};
    ui::Button button_edit{{40, 280, 40, 24}, "Edit"};
    ui::Button button_addline{{80, 280, 40, 24}, "+Ln"};
    ui::Button button_delline{{120, 280, 40, 24}, "-Ln"};
    ui::Button button_save{{160, 280, 40, 24}, "Save"};
    ui::Button button_exit{{200, 280, 40, 24}, "Exit"};
};

}  // namespace app

#endif /*__MB200_UI_TEXT_EDITOR_H__*/
