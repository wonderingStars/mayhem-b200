/*
 * mayhem-b200 — view stack and system chrome.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_navigation.hpp"

#include "app_context.hpp"
#include "receiver_model.hpp"
#include "transmitter_model.hpp"

#include "theme.hpp"

namespace ui {

/* --- NavigationView -------------------------------------------------------- */

NavigationView::NavigationView(Rect parent_rect)
    : View{parent_rect} {
    set_style(Theme::getInstance()->bg_darkest);
}

void NavigationView::push(std::unique_ptr<View> view) {
    if (!view) return;
    pending_.push_back({PendingKind::Push, std::move(view)});
}

void NavigationView::pop() {
    pending_.push_back({PendingKind::Pop, nullptr});
}

void NavigationView::pop_to_root() {
    pending_.push_back({PendingKind::PopToRoot, nullptr});
}

View* NavigationView::top() const {
    return stack_.empty() ? nullptr : stack_.back().get();
}

View* NavigationView::at_depth(size_t i) const {
    if (i >= stack_.size()) return nullptr;
    return stack_[stack_.size() - 1 - i].get();
}

std::string NavigationView::current_title() const {
    const View* t = top();
    return t ? t->title() : std::string{};
}

bool NavigationView::service() {
    if (pending_.empty()) return false;

    bool changed = false;

    while (!pending_.empty()) {
        Pending op = std::move(pending_.front());
        pending_.pop_front();

        switch (op.kind) {
            case PendingKind::Push: {
                if (auto* current = top()) {
                    current->on_hide();
                    remove_child(current);
                }
                op.view->set_parent_rect({0, 0, size().width(), size().height()});
                stack_.push_back(std::move(op.view));
                changed = true;
                break;
            }

            case PendingKind::Pop: {
                /* Never pop the root: there would be nothing to draw. */
                if (stack_.size() <= 1) break;
                if (auto* current = top()) {
                    current->on_hide();
                    remove_child(current);
                }
                stack_.pop_back();
                changed = true;
                break;
            }

            case PendingKind::PopToRoot: {
                if (stack_.size() <= 1) break;
                if (auto* current = top()) {
                    current->on_hide();
                    remove_child(current);
                }
                while (stack_.size() > 1) stack_.pop_back();
                changed = true;
                break;
            }
        }
    }

    if (changed) {
        activate_top();

        /* Landing at the root — the menu — means no app is running, and no
         * app running must mean an idle radio: the handheld this ports stops
         * its baseband when an app closes, and leaving the B200 streaming
         * into a demodulator nobody is watching burns USB bandwidth, CPU and
         * front-end power for nothing (the old behaviour muted only the
         * SPEAKER, which is how "leave the app" still showed receiving:true
         * — reported from live use 2026-08-14). The check is where the BATCH
         * lands, deliberately: an app-to-app switch queues PopToRoot+Push
         * together and only passes through the root inside this loop, and
         * stopping there would bounce the radio on every switch. Central,
         * here, so ninety-odd apps need no edits and a future one cannot
         * forget. */
        if (is_root()) {
            auto& ctx = app::globals();
            if (ctx.receiver != nullptr && ctx.receiver->running()) ctx.receiver->stop();
            if (ctx.transmitter != nullptr && ctx.transmitter->running()) ctx.transmitter->stop();
        }

        if (on_view_changed) on_view_changed();
    }

    return changed;
}

void NavigationView::activate_top() {
    auto* view = top();
    if (view == nullptr) return;

    view->set_parent_rect({0, 0, size().width(), size().height()});
    add_child(view);
    view->on_show();
    view->set_dirty();
    set_dirty();
}

void NavigationView::paint(Painter& painter) {
    painter.fill_rectangle(screen_rect(), style().background);
}

/* --- SystemStatusView ------------------------------------------------------ */

SystemStatusView::SystemStatusView(Rect parent_rect)
    : View{parent_rect} {
    set_style(Theme::getInstance()->bg_dark);
}

void SystemStatusView::set_title(std::string title) {
    if (title == title_) return;
    title_ = std::move(title);
    set_dirty();
}

void SystemStatusView::set_back_enabled(bool enabled) {
    if (enabled == back_enabled_) return;
    back_enabled_ = enabled;
    set_dirty();
}

void SystemStatusView::set_device_text(std::string text) {
    if (text == device_text_) return;
    device_text_ = std::move(text);
    set_dirty();
}

void SystemStatusView::set_device_ok(bool ok) {
    if (ok == device_ok_) return;
    device_ok_ = ok;
    set_dirty();
}

void SystemStatusView::set_receiving(bool rx) {
    if (rx == receiving_) return;
    receiving_ = rx;
    set_dirty();
}

void SystemStatusView::set_transmitting(bool tx) {
    if (tx == transmitting_) return;
    transmitting_ = tx;
    set_dirty();
}

void SystemStatusView::paint(Painter& painter) {
    const auto r = screen_rect();
    const auto& s = style();
    const auto& font = font::fixed_8x16;

    painter.fill_rectangle(r, s.background);

    int x = r.left() + 2;

    if (back_enabled_) {
        /* A left-pointing chevron drawn from two strokes; cheaper than carrying
         * a bitmap and it scales with the font metrics. */
        const int cy = r.top() + r.height() / 2;
        const Color arrow = Theme::getInstance()->fg_light->foreground;
        painter.draw_line({x + 6, cy - 5}, {x + 1, cy}, arrow);
        painter.draw_line({x + 1, cy}, {x + 6, cy + 5}, arrow);
        painter.draw_hline({x + 1, cy}, 8, arrow);
        x += 12;
    }

    painter.draw_string({x, r.top()}, font, s.foreground, s.background, title_);

    /* Right-aligned device state. */
    const int device_w = static_cast<int>(device_text_.size()) * font.char_width();
    const int device_x = r.right() - device_w - 2;
    const Color device_color = device_ok_
                                   ? Theme::getInstance()->fg_green->foreground
                                   : Theme::getInstance()->fg_red->foreground;
    painter.draw_string({device_x, r.top()}, font, device_color, s.background, device_text_);

    /* RX/TX flags sit just left of the device text. */
    int flag_x = device_x - 20;
    if (transmitting_) {
        painter.draw_string({flag_x, r.top()}, font,
                            Theme::getInstance()->fg_red->foreground, s.background, "T");
        flag_x -= 10;
    }
    if (receiving_) {
        painter.draw_string({flag_x, r.top()}, font,
                            Theme::getInstance()->fg_green->foreground, s.background, "R");
    }
}

bool SystemStatusView::on_touch(const TouchEvent event) {
    if (event.type != TouchEvent::Type::End) return true;
    if (!back_enabled_) return true;

    /* Only the arrow area acts as a button. */
    const auto r = screen_rect();
    if (event.point.x() <= r.left() + 14 && on_back) on_back();
    return true;
}

}  // namespace ui
