/*
 * mayhem-b200 — on-screen keyboard and text entry.
 *
 * A port of firmware/application/ui/ui_textentry.* and ui_alphanum.*, plus the
 * EntryTextEdit widget they sit on (firmware/common/ui_widget.*). What changed for
 * the host:
 *
 *   - EntryTextEdit lives here rather than in ui_widget.hpp; nothing outside text
 *     entry uses it.
 *   - The physical keyboard works. Printable characters arrive as
 *     KeyboardEvents (Widget::on_keyboard) and are inserted at the cursor.
 *     Because the window layer maps VK_BACK to KeyEvent::Back, EntryTextEdit also
 *     treats Back as "delete the character before the cursor" while there is
 *     something to delete, and declines it once the field is empty so the view
 *     can still be dismissed with Esc.
 *   - The keyboard buttons forward KeyboardEvents to the view, so typing keeps
 *     working after the focus has moved off the text field.
 *   - enter_mode is mapped to a key set explicitly. Upstream indexes its
 *     three-entry key_sets array with the enter mode directly, which makes
 *     ENTER_KEYBOARD_MODE_SYMBOLS select the hex keys and
 *     ENTER_KEYBOARD_MODE_HEX fall back to letters.
 *   - The shift key is a text button. Upstream uses a bitmap icon that the host
 *     does not carry.
 *   - EntryTextEdit::on_focus/on_blur do not reconfigure the long-press switch mask;
 *     the host input layer decides long-press for itself.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_ALPHANUM_H__
#define __MB200_UI_ALPHANUM_H__

#include "ui.hpp"
#include "ui_painter.hpp"
#include "ui_widget.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#define ENTER_KEYBOARD_MODE_ALPHA 0
#define ENTER_KEYBOARD_MODE_DIGITS 1
#define ENTER_KEYBOARD_MODE_SYMBOLS 2
#define ENTER_KEYBOARD_MODE_HEX 3

namespace ui {

class NavigationView;

/* A single-line editable string with a cursor.
 *
 * The string is owned by the caller and edited in place — the entry view is
 * pushed on the navigation stack and returns nothing, so the caller's buffer is
 * how the result gets out. Its lifetime must outlast the view. */
class EntryTextEdit : public Widget {
   public:
    EntryTextEdit(std::string& str, Point position, uint32_t length = 30)
        : EntryTextEdit{str, 64, position, length} {}

    /* max_length: how long the string may grow.
     * position:   top-left corner, view-local.
     * length:     characters displayed; the field is 8*length by 16 pixels. */
    EntryTextEdit(std::string& str, size_t max_length, Point position, uint32_t length = 30);

    const std::string& value() const;

    size_t max_length() const { return max_length_; }
    uint32_t cursor() const { return cursor_pos_; }
    bool insert_mode() const { return insert_mode_; }

    void set_cursor(uint32_t pos);
    void set_insert_mode();
    void set_overwrite_mode();

    void char_add(char c);
    void char_delete();

    void paint(Painter& painter) override;

    bool on_key(const KeyEvent key) override;
    bool on_encoder(const EncoderEvent delta) override;
    bool on_touch(const TouchEvent event) override;
    bool on_keyboard(const KeyboardEvent event) override;

   protected:
    std::string& text_;
    size_t max_length_;
    uint32_t char_count_;
    uint32_t cursor_pos_;
    bool insert_mode_;
};

/* Base for anything that edits a string: the field, an OK button, and the
 * callback fired when OK is pressed. */
class TextEntryView : public View {
   public:
    std::function<void(std::string&)> on_changed{};

    void focus() override;
    std::string title() const override { return "Text entry"; }

    void set_cursor(uint32_t pos);
    const std::string& value() const { return text_input.value(); }

    /* Fires on_changed and pops the view. What the OK button does. */
    void confirm();

   protected:
    TextEntryView(NavigationView& nav, std::string& str, size_t max_length);

    void char_add(const char c);
    void char_delete();

    NavigationView& nav_;
    std::string& str_;

    EntryTextEdit text_input;
    Button button_ok{
        {22 * 8, 32 * 8 - 3, 8 * 8, 3 * 16 + 3},
        "OK"};
};

/* A keyboard key. Exists so the 29 keys can live in a std::array (Button has no
 * default constructor and is neither copyable nor movable) and so that typing
 * on the physical keyboard still reaches the text field when a key button holds
 * the focus. */
class AlphanumKey : public Button {
   public:
    AlphanumKey()
        : Button{{0, 0, 0, 0}, ""} {}

    std::function<bool(KeyboardEvent)> on_char{};

    bool on_keyboard(const KeyboardEvent event) override {
        return on_char ? on_char(event) : false;
    }
};

/* The on-screen keyboard: 29 character keys in five columns, a shift key, a
 * mode key cycling abc / 123 / hex, a delete key and OK. The raw field enters a
 * character by its decimal code, for the bytes with no key. */
class AlphanumView : public TextEntryView {
   public:
    AlphanumView(NavigationView& nav, std::string& str, size_t max_length, uint8_t enter_mode);

    bool on_encoder(const EncoderEvent delta) override;
    bool on_keyboard(const KeyboardEvent event) override;

    /* Which key set is showing: 0 letters, 1 digits/symbols, 2 hex. */
    uint32_t mode() const { return mode_; }

   private:
    enum class ShiftMode : uint8_t {
        None = 0,
        Shift = 1,
        ShiftLock = 2,
    };

    static constexpr const char* keys_lower = "abcdefghijklmnopqrstuvwxyz, .";
    static constexpr const char* keys_upper = "ABCDEFGHIJKLMNOPQRSTUVWXYZ, .";
    static constexpr const char* keys_digit = "1234567890()'`\"+-*/=<>_\\!?, .";
    static constexpr const char* keys_symbl = "!@#$%^&*()[]'`\"{}|:;<>-_~?, .";
    static constexpr const char* keys_hex = "1234567890ABCDEF ";

    struct key_set_t {
        const char* name;
        const char* normal;
        const char* shifted;
    };

    static const key_set_t key_sets[3];

    int16_t focused_button_ = 0;
    uint32_t mode_ = 0;
    ShiftMode shift_mode_ = ShiftMode::None;

    void set_enter_mode(uint8_t enter_mode);
    void set_mode(uint32_t new_mode, ShiftMode new_shift_mode = ShiftMode::None);
    void refresh_keys();
    void on_button(Button& button);

    std::array<AlphanumKey, 29> buttons{};

    Button button_shift{
        {screen_width - screen_width / 5, 214, screen_width / 5, 38},
        "SHIFT"};

    Labels labels{
        {{1 * 8, 33 * 8}, "Raw:", Color::light_grey()},
        {{1 * 8, 35 * 8}, "AKA:", Color::light_grey()}};

    NumberField field_raw{
        {5 * 8, 33 * 8},
        3,
        {1, 255},
        1,
        '0'};

    Text text_raw_to_char{
        {5 * 8, 35 * 8, 4 * 8, 16},
        "0"};

    Button button_delete{
        {9 * 8, 32 * 8 - 3, 7 * 8, 3 * 16 + 3},
        "<DEL"};

    Button button_mode{
        {16 * 8, 32 * 8 - 3, 6 * 8, 3 * 16 + 3},
        ""};
};

/* Pushes an AlphanumView to edit 'str'.
 *
 * Returns immediately. 'str' is taken by reference and the caller must keep it
 * alive until the view is dismissed; on_done is called with it when OK is
 * pressed. */
void text_prompt(
    NavigationView& nav,
    std::string& str,
    size_t max_length,
    uint8_t mode,
    std::function<void(std::string&)> on_done = nullptr);

void text_prompt(
    NavigationView& nav,
    std::string& str,
    uint32_t cursor_pos,
    size_t max_length,
    uint8_t mode,
    std::function<void(std::string&)> on_done = nullptr);

}  // namespace ui

#endif /*__MB200_UI_ALPHANUM_H__*/
