/*
 * mayhem-b200 — input routing.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "event_dispatch.hpp"

namespace app {

EventDispatcher::EventDispatcher(ui::Widget& root, ui::Context& context)
    : root_{root},
      context_{context} {
}

bool EventDispatcher::dispatch(const host::Event& event) {
    switch (event.type) {
        case host::Event::Type::Quit:
            return false;

        case host::Event::Type::Key:
            dispatch_key(event.key);
            return true;

        case host::Event::Type::Encoder:
            dispatch_encoder(event.encoder);
            return true;

        case host::Event::Type::Touch:
            dispatch_touch(event.touch);
            return true;

        case host::Event::Type::Character:
            dispatch_character(event.character);
            return true;
    }
    return true;
}

void EventDispatcher::dispatch_key(ui::KeyEvent key) {
    auto* focus = context_.focus_manager().focus_widget();

    /* The focused widget gets first refusal. */
    if (focus != nullptr && focus->on_key(key)) return;

    /* Back is the one key with an app-level meaning. */
    if (key == ui::KeyEvent::Back) {
        if (on_back) on_back();
        return;
    }

    /* Otherwise the direction keys move focus. */
    context_.focus_manager().update(&root_, key);

    /* If focus did not move and the top view wants the key, let it have it —
     * this is how the receiver app tunes with Left/Right. */
    if (context_.focus_manager().focus_widget() == focus && focus != nullptr) {
        for (auto* child : root_.children()) {
            if (!child->hidden() && child->on_key(key)) return;
        }
    }
}

void EventDispatcher::dispatch_encoder(ui::EncoderEvent delta) {
    auto* focus = context_.focus_manager().focus_widget();
    if (focus != nullptr && focus->on_encoder(delta)) return;

    for (auto* child : root_.children()) {
        if (!child->hidden() && child->on_encoder(delta)) return;
    }
}

bool EventDispatcher::touch_widget(ui::Widget* widget, const ui::TouchEvent& event) {
    if (widget == nullptr || widget->hidden()) return false;
    if (!widget->screen_rect().contains(event.point)) return false;

    /* Children are painted after their parent, so they are on top: search them
     * in reverse order and let the topmost one win. */
    const auto& children = widget->children();
    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        if (touch_widget(*it, event)) return true;
    }

    return widget->on_touch(event);
}

void EventDispatcher::dispatch_touch(const ui::TouchEvent& event) {
    if (event.type == ui::TouchEvent::Type::Start) {
        touch_capture_ = nullptr;

        /* Record which widget accepted the press so drag and release follow it. */
        std::function<ui::Widget*(ui::Widget*)> find = [&](ui::Widget* w) -> ui::Widget* {
            if (w == nullptr || w->hidden()) return nullptr;
            if (!w->screen_rect().contains(event.point)) return nullptr;

            const auto& children = w->children();
            for (auto it = children.rbegin(); it != children.rend(); ++it) {
                if (auto* hit = find(*it)) return hit;
            }
            return w->on_touch(event) ? w : nullptr;
        };

        touch_capture_ = find(&root_);
        return;
    }

    if (touch_capture_ != nullptr) {
        touch_capture_->on_touch(event);
        if (event.type == ui::TouchEvent::Type::End) touch_capture_ = nullptr;
        return;
    }

    touch_widget(&root_, event);
}

void EventDispatcher::dispatch_character(ui::KeyboardEvent c) {
    auto* focus = context_.focus_manager().focus_widget();
    if (focus != nullptr && focus->on_keyboard(c)) return;

    for (auto* child : root_.children()) {
        if (!child->hidden() && child->on_keyboard(c)) return;
    }
}

}  // namespace app
