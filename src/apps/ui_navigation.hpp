/*
 * mayhem-b200 — view stack and system chrome.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original design)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_NAVIGATION_H__
#define __MB200_UI_NAVIGATION_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {

/* A stack of full-screen views.
 *
 * push() and pop() are deferred to service(), which the main loop calls between
 * frames. A view's own button handler is usually what asks to pop it, and
 * destroying the object whose method is currently executing is exactly the bug
 * that deferral avoids. */
class NavigationView : public View {
   public:
    explicit NavigationView(Rect parent_rect);

    void push(std::unique_ptr<View> view);

    template <typename T, typename... Args>
    void push_new(Args&&... args) {
        push(std::make_unique<T>(std::forward<Args>(args)...));
    }

    void pop();
    void pop_to_root();

    /* Applies any pending push/pop. Returns true if the stack changed. */
    bool service();

    View* top() const;
    size_t depth() const { return stack_.size(); }
    bool is_root() const { return stack_.size() <= 1; }

    /* The view `i` levels below the top (0 == top()), or nullptr past the
     * bottom of the stack. Read-only stack access exists for the web portal's
     * panel providers: they are handed nav->top(), but an app's data usually
     * lives on the view it pushed FIRST (e.g. ADS-B's aircraft tracker lives
     * on AdsbRxView, which is still on the stack underneath the per-aircraft
     * details page). Without this they would have to go blank whenever the
     * local operator drills into a sub-view. Ownership stays with the stack;
     * the returned pointer is only valid until the next service(). */
    View* at_depth(size_t i) const;

    std::string current_title() const;

    /* Fired after the stack changes, so the status bar can refresh. */
    std::function<void()> on_view_changed{};

    void paint(Painter& painter) override;

   private:
    enum class PendingKind : uint8_t { Push, Pop, PopToRoot };

    struct Pending {
        PendingKind kind;
        std::unique_ptr<View> view;
    };

    void activate_top();

    std::vector<std::unique_ptr<View>> stack_{};
    std::deque<Pending> pending_{};
};

/* The 16-pixel bar across the top: title on the left, radio state on the right,
 * and a back arrow when the stack is deeper than the root. */
class SystemStatusView : public View {
   public:
    static constexpr Dim status_height = 16;

    explicit SystemStatusView(Rect parent_rect);

    void set_title(std::string title);
    void set_back_enabled(bool enabled);

    /* Right-hand indicator, e.g. "B200" or "no dev". */
    void set_device_text(std::string text);
    void set_device_ok(bool ok);

    /* Small activity flags shown between the title and the device text. */
    void set_receiving(bool rx);
    void set_transmitting(bool tx);

    std::function<void()> on_back{};

    void paint(Painter& painter) override;
    bool on_touch(const TouchEvent event) override;

   private:
    std::string title_{};
    std::string device_text_{};
    bool back_enabled_{false};
    bool device_ok_{false};
    bool receiving_{false};
    bool transmitting_{false};
};

}  // namespace ui

#endif /*__MB200_UI_NAVIGATION_H__*/
