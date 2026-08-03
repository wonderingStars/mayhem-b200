/*
 * mayhem-b200 — focus-manager lifetime tests.
 *
 * Regression cover for a use-after-free found by the hardware app sweep: the
 * FocusManager keeps a raw pointer to the focused widget, and when a view is
 * destroyed (leaving one app for another) that pointer dangled. The next
 * set_focus_widget() blurs the previous focus widget — a call into freed memory.
 * The Widget destructor now tells the manager to drop it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_focus.hpp"
#include "ui_widget.hpp"

#include <memory>

namespace {

/* A minimal focusable widget whose on_blur would touch freed memory if the
 * manager were allowed to keep a dangling pointer to it. */
class Focusable : public ui::Widget {
   public:
    Focusable() { set_focusable(true); }
    void paint(ui::Painter&) override {}
    bool blurred = false;
    void on_blur() override { blurred = true; }
};

}  // namespace

TEST(focus_cleared_when_focused_widget_destroyed) {
    auto& fm = Focusable{}.context().focus_manager();

    auto* w = new Focusable();
    fm.set_focus_widget(w);
    CHECK(fm.focus_widget() == w);

    delete w;  /* destructor must drop it from the manager */
    CHECK(fm.focus_widget() == nullptr);
}

TEST(focusing_new_widget_after_old_destroyed_does_not_touch_freed_memory) {
    auto& fm = Focusable{}.context().focus_manager();

    /* Focus a heap widget, then destroy it while it still "has" focus. */
    auto* first = new Focusable();
    fm.set_focus_widget(first);
    delete first;

    /* Before the fix this call blurred `first` (freed) and crashed. It must now
     * simply focus the new widget with no dangling access. */
    Focusable second;
    fm.set_focus_widget(&second);
    CHECK(fm.focus_widget() == &second);

    fm.set_focus_widget(nullptr);
}

TEST(mirror_widget_also_cleared_on_destroy) {
    auto& fm = Focusable{}.context().focus_manager();

    auto* w = new Focusable();
    fm.setMirror(w);
    delete w;

    /* No direct getter for the mirror; the guarantee is that a later focus
     * change that consults the mirror does not fault. */
    Focusable other;
    fm.set_focus_widget(&other);
    CHECK(fm.focus_widget() == &other);
    fm.set_focus_widget(nullptr);
}

TEST(destroying_a_non_focused_widget_leaves_focus_intact) {
    auto& fm = Focusable{}.context().focus_manager();

    Focusable keeper;
    fm.set_focus_widget(&keeper);

    { Focusable transient; }  /* destroyed; was never focused */

    CHECK(fm.focus_widget() == &keeper);
    fm.set_focus_widget(nullptr);
}
