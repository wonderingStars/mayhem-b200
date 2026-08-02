/*
 * mayhem-b200 — modal message and confirmation dialog.
 *
 * The host equivalent of upstream's NavigationView::display_modal() and
 * ModalMessageView (firmware/application/ui_navigation.*). Same three flavours:
 *
 *   INFO   one OK button, pops the modal.
 *   YESNO  YES and NO, the callback gets the answer, then the modal pops.
 *   ABORT  one OK button, pops the modal *and* the view underneath it — for
 *          when an app has failed and cannot carry on.
 *
 * Differences from upstream: there is no utilities bitmap above the text (the
 * host does not carry the icon set), the message block is centred in the space
 * above the buttons instead of starting at a fixed y, and lines longer than the
 * dialog are word-wrapped rather than running off the right edge.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_MODAL_H__
#define __MB200_UI_MODAL_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ui {

class NavigationView;

enum modal_t {
    INFO = 0,
    YESNO,
    ABORT
};

/* Splits on '\n' and wraps anything longer than max_chars, breaking at spaces
 * where it can and mid-word where it cannot. A max_chars of 0 only splits. */
std::vector<std::string> wrap_message(const std::string& message, size_t max_chars);

class ModalMessageView : public View {
   public:
    ModalMessageView(
        NavigationView& nav,
        std::string title,
        std::string message,
        modal_t type = INFO,
        std::function<void(bool)> on_choice = nullptr,
        bool compact = false);

    void paint(Painter& painter) override;
    void focus() override;
    void set_parent_rect(const Rect new_parent_rect) override;

    std::string title() const override { return title_; }

    modal_t type() const { return type_; }
    const std::string& message() const { return message_; }

    /* Fires the choice callback and unwinds the navigation stack. This is what
     * the buttons call. */
    void choose(bool value);

   private:
    const std::string title_;
    const std::string message_;
    const modal_t type_;
    const std::function<void(bool)> on_choice_;
    const bool compact_;

    NavigationView& nav_;

    Button button_ok{
        {UI_POS_X_CENTER(10), UI_POS_Y_BOTTOM(5), UI_POS_WIDTH(10), UI_POS_HEIGHT(3)},
        "OK"};

    Button button_yes{
        {UI_POS_X_CENTER(8) - UI_POS_WIDTH(6), UI_POS_Y_BOTTOM(5), UI_POS_WIDTH(8), UI_POS_HEIGHT(3)},
        "YES"};

    Button button_no{
        {UI_POS_X_CENTER(8) + UI_POS_WIDTH(6), UI_POS_Y_BOTTOM(5), UI_POS_WIDTH(8), UI_POS_HEIGHT(3)},
        "NO"};
};

/* Pushes a modal on the given stack. */
void display_modal(NavigationView& nav, const std::string& title, const std::string& message);
void display_modal(
    NavigationView& nav,
    const std::string& title,
    const std::string& message,
    modal_t type,
    std::function<void(bool)> on_choice = nullptr,
    bool compact = false);

/* Same, on the application's navigation stack (app::globals().nav). No-ops if
 * there is no stack yet — which only happens before main() has wired it up. */
void display_modal(const std::string& title, const std::string& message);
void display_modal(
    const std::string& title,
    const std::string& message,
    modal_t type,
    std::function<void(bool)> on_choice = nullptr,
    bool compact = false);

}  // namespace ui

#endif /*__MB200_UI_MODAL_H__*/
