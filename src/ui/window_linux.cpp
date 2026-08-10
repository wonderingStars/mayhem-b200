/*
 * mayhem-b200 — X11 window hosting the 240x320 framebuffer.
 *
 * The Linux counterpart of window.cpp. Same host::Window contract, same
 * keyboard/mouse mapping (see window.hpp); only the OS calls differ. CMake
 * compiles exactly one of the two per platform.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version. See LICENSE.GPL-2.0-or-later.
 */

#if !defined(_WIN32)

#include "window.hpp"

#include "display.hpp"
#include "input.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace host {

struct Window::Impl {
    ::Display* dpy{nullptr};
    int screen{0};
    ::Window win{0};
    GC gc{nullptr};
    Visual* visual{nullptr};
    int depth{0};
    Atom wm_delete{0};

    /* The scaled image handed to XPutImage. Reallocated whenever the scale
     * changes. XCreateImage takes ownership of `scaled`, so XDestroyImage
     * frees both. */
    XImage* image{nullptr};
    uint32_t* scaled{nullptr};
    int image_scale{0};

    /* display.composite_bgra() always writes one screen's worth of
     * 0x00RRGGBB; at scale 1 it can write straight into the XImage, otherwise
     * it lands here first and gets expanded. */
    std::vector<uint32_t> composed;

    /* True when the server's TrueColor masks are the usual
     * 0xFF0000/0x00FF00/0x0000FF, in which case composite_bgra()'s output is
     * already in the server's pixel format and needs no repacking. */
    bool direct_pixels{true};
    int red_shift{16};
    int green_shift{8};
    int blue_shift{0};

    /* Kept for parity with the Win32 implementation, whose window procedure
     * can run re-entrantly. Here every push happens on the UI thread inside
     * pump(), but poll_event()'s contract is the same either way. */
    std::mutex queue_lock;
    std::deque<Event> queue;

    bool touch_down{false};
    bool needs_repaint{true};
    Window* owner{nullptr};

    void push(const Event& e) {
        std::lock_guard<std::mutex> g{queue_lock};
        /* Bound the queue so a stalled UI thread cannot grow it without limit.
         * Encoder detents are the only high-rate source; dropping the oldest
         * keeps the most recent user intent. */
        if (queue.size() > 256) queue.pop_front();
        queue.push_back(e);
    }

    void push_key(ui::KeyEvent k) {
        Event e;
        e.type = Event::Type::Key;
        e.key = k;
        push(e);
    }

    void push_encoder(int delta) {
        Event e;
        e.type = Event::Type::Encoder;
        e.encoder = delta;
        push(e);
    }

    void push_touch(ui::Point p, ui::TouchEvent::Type t) {
        Event e;
        e.type = Event::Type::Touch;
        e.touch = {p, t};
        push(e);
    }

    void push_char(uint8_t c) {
        Event e;
        e.type = Event::Type::Character;
        e.character = c;
        push(e);
    }
};

namespace {

/* Position of the lowest set bit of a colour mask — how far to shift an
 * 8-bit channel to land in the server's pixel layout. */
int mask_shift(unsigned long mask) {
    if (mask == 0) return 0;
    int shift = 0;
    while ((mask & 1u) == 0) {
        mask >>= 1;
        shift++;
    }
    return shift;
}

/* Maps an X keysym to a PortaPack switch, or returns false if unmapped.
 * Deliberately the same set window.cpp's map_vk() covers. */
bool map_keysym(KeySym ks, ui::KeyEvent& out) {
    switch (ks) {
        case XK_Up:
        case XK_KP_Up:
            out = ui::KeyEvent::Up;
            return true;
        case XK_Down:
        case XK_KP_Down:
            out = ui::KeyEvent::Down;
            return true;
        case XK_Left:
        case XK_KP_Left:
            out = ui::KeyEvent::Left;
            return true;
        case XK_Right:
        case XK_KP_Right:
            out = ui::KeyEvent::Right;
            return true;
        case XK_Return:
        case XK_KP_Enter:
        case XK_space:
            out = ui::KeyEvent::Select;
            return true;
        case XK_Escape:
        case XK_BackSpace:
            out = ui::KeyEvent::Back;
            return true;
        default:
            return false;
    }
}

}  // namespace

Window::Window() = default;

Window::~Window() {
    destroy();
}

/* Allocates (or reallocates) the XImage for the current scale. */
static bool alloc_image(Window::Impl* impl, int scale) {
    if (impl->image != nullptr) {
        XDestroyImage(impl->image);  /* frees impl->scaled too */
        impl->image = nullptr;
        impl->scaled = nullptr;
    }

    const int w = ui::screen_width * scale;
    const int h = ui::screen_height * scale;
    const size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);

    impl->scaled = static_cast<uint32_t*>(std::malloc(count * sizeof(uint32_t)));
    if (impl->scaled == nullptr) return false;
    std::memset(impl->scaled, 0, count * sizeof(uint32_t));

    impl->image = XCreateImage(impl->dpy, impl->visual, static_cast<unsigned>(impl->depth),
                               ZPixmap, 0, reinterpret_cast<char*>(impl->scaled),
                               static_cast<unsigned>(w), static_cast<unsigned>(h), 32, 0);
    if (impl->image == nullptr) {
        std::free(impl->scaled);
        impl->scaled = nullptr;
        return false;
    }

    impl->image_scale = scale;
    return true;
}

bool Window::create(const std::string& title, int scale) {
    scale_ = std::clamp(scale, 1, 6);
    impl_ = new Impl();
    impl_->owner = this;

    impl_->dpy = XOpenDisplay(nullptr);
    if (impl_->dpy == nullptr) {
        /* No DISPLAY, or nothing listening on it. main() reports this as
         * "failed to create window" and exits, same as a Win32 failure. */
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    impl_->screen = DefaultScreen(impl_->dpy);
    impl_->visual = DefaultVisual(impl_->dpy, impl_->screen);
    impl_->depth = DefaultDepth(impl_->dpy, impl_->screen);

    /* composite_bgra() produces 0x00RRGGBB. On the near-universal 24/32-bit
     * TrueColor visual that is already the server's word layout, so the blit
     * is a straight copy; anything else gets repacked per pixel using the
     * visual's own masks rather than being refused. */
    impl_->direct_pixels = (impl_->visual->red_mask == 0x00FF0000ul &&
                            impl_->visual->green_mask == 0x0000FF00ul &&
                            impl_->visual->blue_mask == 0x000000FFul);
    impl_->red_shift = mask_shift(impl_->visual->red_mask);
    impl_->green_shift = mask_shift(impl_->visual->green_mask);
    impl_->blue_shift = mask_shift(impl_->visual->blue_mask);

    const int w = ui::screen_width * scale_;
    const int h = ui::screen_height * scale_;

    impl_->win = XCreateSimpleWindow(impl_->dpy, RootWindow(impl_->dpy, impl_->screen),
                                     0, 0, static_cast<unsigned>(w), static_cast<unsigned>(h),
                                     0, BlackPixel(impl_->dpy, impl_->screen),
                                     BlackPixel(impl_->dpy, impl_->screen));
    if (impl_->win == 0) {
        destroy();
        return false;
    }

    XSelectInput(impl_->dpy, impl_->win,
                 ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                     ButtonReleaseMask | PointerMotionMask | StructureNotifyMask | FocusChangeMask);

    /* The Win32 window has no WS_THICKFRAME/WS_MAXIMIZEBOX: the framebuffer is
     * a fixed 240x320 and only F11's integer scale changes its size. Size hints
     * ask the window manager for the same. */
    XSizeHints hints{};
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = w;
    hints.min_height = hints.max_height = h;
    XSetWMNormalHints(impl_->dpy, impl_->win, &hints);

    /* Without this the window manager closes the connection on the title-bar
     * X instead of telling us, which shows up as an XIO fatal error. */
    impl_->wm_delete = XInternAtom(impl_->dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(impl_->dpy, impl_->win, &impl_->wm_delete, 1);

    XStoreName(impl_->dpy, impl_->win, title.c_str());

    impl_->gc = XCreateGC(impl_->dpy, impl_->win, 0, nullptr);
    if (impl_->gc == nullptr || !alloc_image(impl_, scale_)) {
        destroy();
        return false;
    }

    impl_->composed.assign(static_cast<size_t>(ui::screen_width) * ui::screen_height, 0);

    XMapWindow(impl_->dpy, impl_->win);
    XFlush(impl_->dpy);
    return true;
}

void Window::destroy() {
    if (impl_ == nullptr) return;
    if (impl_->dpy != nullptr) {
        if (impl_->image != nullptr) XDestroyImage(impl_->image);  /* frees impl_->scaled */
        if (impl_->gc != nullptr) XFreeGC(impl_->dpy, impl_->gc);
        if (impl_->win != 0) XDestroyWindow(impl_->dpy, impl_->win);
        XCloseDisplay(impl_->dpy);
    }
    delete impl_;
    impl_ = nullptr;
    closed_ = true;
}

void Window::set_scale(int scale) {
    if (impl_ == nullptr || impl_->win == 0) return;
    scale_ = std::clamp(scale, 1, 6);

    const int w = ui::screen_width * scale_;
    const int h = ui::screen_height * scale_;

    /* The min == max size hints have to be relaxed to the new size before the
     * resize, or a strict window manager refuses it. */
    XSizeHints hints{};
    hints.flags = PMinSize | PMaxSize;
    hints.min_width = hints.max_width = w;
    hints.min_height = hints.max_height = h;
    XSetWMNormalHints(impl_->dpy, impl_->win, &hints);
    XResizeWindow(impl_->dpy, impl_->win, static_cast<unsigned>(w), static_cast<unsigned>(h));

    if (!alloc_image(impl_, scale_)) return;
    present(true);
}

bool Window::pump() {
    if (impl_ == nullptr) return false;

    while (XPending(impl_->dpy) > 0) {
        XEvent ev;
        XNextEvent(impl_->dpy, &ev);

        const int scale = std::max(1, scale_);

        switch (ev.type) {
            case ClientMessage: {
                if (static_cast<Atom>(ev.xclient.data.l[0]) == impl_->wm_delete) {
                    Event e;
                    e.type = Event::Type::Quit;
                    impl_->push(e);
                }
                break;
            }

            case Expose:
                impl_->needs_repaint = true;
                break;

            case KeyPress: {
                char text[8] = {0};
                KeySym ks = 0;
                const int n = XLookupString(&ev.xkey, text, sizeof(text) - 1, &ks, nullptr);

                ui::KeyEvent k;
                if (map_keysym(ks, k)) {
                    input::note_key_down(k, input::now_ms());
                    impl_->push_key(k);
                    break;
                }
                if (ks == XK_Prior) {  /* PageUp */
                    impl_->push_encoder(+1);
                    break;
                }
                if (ks == XK_Next) {   /* PageDown */
                    impl_->push_encoder(-1);
                    break;
                }
                if (ks == XK_F11) {
                    set_scale(scale_ % 4 + 1);
                    break;
                }
                /* Printable Latin-1 only; the firmware's fonts cover 0x20..0xFF.
                 * Mapped keys returned above, so — as on Win32, where a handled
                 * WM_KEYDOWN never becomes a WM_CHAR — Space and Backspace do
                 * not also arrive as characters. */
                if (n > 0) {
                    const auto c = static_cast<unsigned char>(text[0]);
                    if (c >= 0x20) impl_->push_char(c);
                }
                break;
            }

            case KeyRelease: {
                KeySym ks = XLookupKeysym(&ev.xkey, 0);
                ui::KeyEvent k;
                if (map_keysym(ks, k)) input::note_key_up(k);
                break;
            }

            case ButtonPress: {
                /* Buttons 4 and 5 are the wheel; X delivers them as clicks. */
                if (ev.xbutton.button == Button4) {
                    impl_->push_encoder(+1);
                } else if (ev.xbutton.button == Button5) {
                    impl_->push_encoder(-1);
                } else if (ev.xbutton.button == Button1) {
                    impl_->touch_down = true;
                    impl_->push_touch({ev.xbutton.x / scale, ev.xbutton.y / scale},
                                      ui::TouchEvent::Type::Start);
                }
                break;
            }

            case MotionNotify: {
                if (impl_->touch_down) {
                    impl_->push_touch({ev.xmotion.x / scale, ev.xmotion.y / scale},
                                      ui::TouchEvent::Type::Move);
                }
                break;
            }

            case ButtonRelease: {
                if (ev.xbutton.button == Button1 && impl_->touch_down) {
                    impl_->touch_down = false;
                    impl_->push_touch({ev.xbutton.x / scale, ev.xbutton.y / scale},
                                      ui::TouchEvent::Type::End);
                }
                break;
            }

            case FocusOut:
                /* Do not leave keys latched down when the window loses focus. */
                input::reset();
                break;

            default:
                break;
        }
    }

    return !closed_;
}

void Window::present(bool force) {
    if (impl_ == nullptr || impl_->image == nullptr) return;

    const bool damaged = display.take_damage();
    if (!damaged && !force && !impl_->needs_repaint) return;

    const int scale = std::max(1, scale_);
    const int w = ui::screen_width * scale;
    const int h = ui::screen_height * scale;

    if (scale == 1 && impl_->direct_pixels) {
        display.composite_bgra(impl_->scaled);
    } else {
        display.composite_bgra(impl_->composed.data());

        const uint32_t* src = impl_->composed.data();
        for (int y = 0; y < ui::screen_height; y++) {
            uint32_t* first = impl_->scaled + static_cast<size_t>(y) * scale * w;
            for (int x = 0; x < ui::screen_width; x++) {
                const uint32_t rgb = src[static_cast<size_t>(y) * ui::screen_width + x];
                const uint32_t pixel =
                    impl_->direct_pixels
                        ? rgb
                        : ((((rgb >> 16) & 0xFFu) << impl_->red_shift) |
                           (((rgb >> 8) & 0xFFu) << impl_->green_shift) |
                           ((rgb & 0xFFu) << impl_->blue_shift));
                for (int k = 0; k < scale; k++) first[x * scale + k] = pixel;
            }
            /* Rows repeat verbatim, so copy the first one rather than
             * re-expanding it scale-1 more times. */
            for (int r = 1; r < scale; r++)
                std::memcpy(first + static_cast<size_t>(r) * w, first,
                            static_cast<size_t>(w) * sizeof(uint32_t));
        }
    }

    XPutImage(impl_->dpy, impl_->win, impl_->gc, impl_->image, 0, 0, 0, 0,
              static_cast<unsigned>(w), static_cast<unsigned>(h));
    XFlush(impl_->dpy);
    impl_->needs_repaint = false;
}

bool Window::poll_event(Event& out) {
    if (impl_ == nullptr) return false;

    std::lock_guard<std::mutex> g{impl_->queue_lock};
    if (impl_->queue.empty()) return false;
    out = impl_->queue.front();
    impl_->queue.pop_front();
    if (out.type == Event::Type::Quit) closed_ = true;
    return true;
}

void Window::set_title(const std::string& title) {
    if (impl_ == nullptr || impl_->win == 0) return;
    XStoreName(impl_->dpy, impl_->win, title.c_str());
    XFlush(impl_->dpy);
}

}  // namespace host

#endif /* !_WIN32 */
