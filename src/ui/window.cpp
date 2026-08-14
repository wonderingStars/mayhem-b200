/*
 * mayhem-b200 — Win32 window hosting the 240x320 framebuffer.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version. See LICENSE.GPL-2.0-or-later.
 */

/* Windows implementation of host::Window. window_linux.cpp is the X11 one;
 * CMake compiles only the file matching the target platform, and this guard
 * keeps that true even if the source globs ever pick up both. */
#if defined(_WIN32)

#include "window.hpp"

#include "display.hpp"
#include "input.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <vector>

namespace host {

namespace {

constexpr wchar_t kClassName[] = L"MayhemB200Window";

/* Notification-area (tray) plumbing. The tray icon is what makes a hidden
 * window survivable: it is the "this is running" affordance and, more
 * importantly, the graceful Quit that would otherwise only exist on the
 * window's title bar. Quit pushes the very same Event::Type::Quit that
 * WM_CLOSE does, so it lands in main()'s normal teardown — it is not a kill. */
constexpr UINT kTrayCallbackMsg = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT kMenuShow = 0xE001;
constexpr UINT kMenuHide = 0xE002;
constexpr UINT kMenuQuit = 0xE003;

/* Resource id of the app icon compiled in from mayhem-b200.rc (installer
 * artwork). Falls back to the stock application icon if the resource is
 * missing, so a build without the .rc still gets a usable tray entry. */
constexpr WORD kAppIconResourceId = 101;

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

}  // namespace

struct Window::Impl {
    HWND hwnd{nullptr};
    HDC mem_dc{nullptr};
    HBITMAP dib{nullptr};
    uint32_t* pixels{nullptr};  /* 0x00RRGGBB, width*height */
    BITMAPINFO bmi{};

    std::mutex queue_lock;
    std::deque<Event> queue;

    bool touch_down{false};
    Window* owner{nullptr};
    bool tray_added{false};

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

/* Maps a virtual key to a PortaPack switch, or returns false if unmapped. */
bool map_vk(WPARAM vk, ui::KeyEvent& out) {
    switch (vk) {
        case VK_UP:
            out = ui::KeyEvent::Up;
            return true;
        case VK_DOWN:
            out = ui::KeyEvent::Down;
            return true;
        case VK_LEFT:
            out = ui::KeyEvent::Left;
            return true;
        case VK_RIGHT:
            out = ui::KeyEvent::Right;
            return true;
        case VK_RETURN:
        case VK_SPACE:
            out = ui::KeyEvent::Select;
            return true;
        case VK_ESCAPE:
        case VK_BACK:
            out = ui::KeyEvent::Back;
            return true;
        default:
            return false;
    }
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* impl = reinterpret_cast<Window::Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (impl == nullptr) return DefWindowProcW(hwnd, msg, wparam, lparam);

    const int scale = impl->owner ? std::max(1, impl->owner->scale()) : 1;

    switch (msg) {
        case WM_CLOSE: {
            Event e;
            e.type = Event::Type::Quit;
            impl->push(e);
            return 0;
        }

        /* Tray icon: left double-click restores the window, right-click (or a
         * single left click) opens the menu. */
        case kTrayCallbackMsg: {
            const UINT ev = LOWORD(lparam);
            if (ev == WM_LBUTTONDBLCLK) {
                if (impl->owner) impl->owner->set_visible(true);
                return 0;
            }
            if (ev == WM_RBUTTONUP || ev == WM_LBUTTONUP) {
                const bool shown = impl->owner ? impl->owner->visible() : true;
                HMENU menu = CreatePopupMenu();
                if (menu == nullptr) return 0;
                AppendMenuW(menu, MF_STRING, shown ? kMenuHide : kMenuShow,
                            shown ? L"Hide window" : L"Show window");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, kMenuQuit, L"Quit");

                POINT pt;
                GetCursorPos(&pt);
                /* Required so the menu closes when the user clicks elsewhere —
                 * a documented Win32 quirk for tray menus. */
                SetForegroundWindow(hwnd);
                TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
                PostMessageW(hwnd, WM_NULL, 0, 0);
                DestroyMenu(menu);
                return 0;
            }
            return 0;
        }

        case WM_COMMAND: {
            switch (LOWORD(wparam)) {
                case kMenuShow:
                    if (impl->owner) impl->owner->set_visible(true);
                    return 0;
                case kMenuHide:
                    if (impl->owner) impl->owner->set_visible(false);
                    return 0;
                case kMenuQuit: {
                    /* Same event WM_CLOSE produces: the graceful teardown. */
                    Event e;
                    e.type = Event::Type::Quit;
                    impl->push(e);
                    return 0;
                }
                default:
                    break;
            }
            break;
        }

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            ui::KeyEvent k;
            if (map_vk(wparam, k)) {
                input::note_key_down(k, input::now_ms());
                impl->push_key(k);
                return 0;
            }
            if (wparam == VK_PRIOR) {  /* PageUp */
                impl->push_encoder(+1);
                return 0;
            }
            if (wparam == VK_NEXT) {   /* PageDown */
                impl->push_encoder(-1);
                return 0;
            }
            if (wparam == VK_F11) {
                if (impl->owner) impl->owner->set_scale(impl->owner->scale() % 4 + 1);
                return 0;
            }
            return 0;
        }

        case WM_KEYUP:
        case WM_SYSKEYUP: {
            ui::KeyEvent k;
            if (map_vk(wparam, k)) input::note_key_up(k);
            return 0;
        }

        case WM_CHAR: {
            /* Printable Latin-1 only; the firmware's fonts cover 0x20..0xFF. */
            const auto c = static_cast<unsigned>(wparam);
            if (c >= 0x20 && c <= 0xFF) impl->push_char(static_cast<uint8_t>(c));
            return 0;
        }

        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
            if (delta != 0) impl->push_encoder(delta);
            return 0;
        }

        case WM_LBUTTONDOWN: {
            SetCapture(hwnd);
            impl->touch_down = true;
            impl->push_touch({GET_X_LPARAM(lparam) / scale, GET_Y_LPARAM(lparam) / scale},
                             ui::TouchEvent::Type::Start);
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (impl->touch_down) {
                impl->push_touch({GET_X_LPARAM(lparam) / scale, GET_Y_LPARAM(lparam) / scale},
                                 ui::TouchEvent::Type::Move);
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (impl->touch_down) {
                impl->touch_down = false;
                ReleaseCapture();
                impl->push_touch({GET_X_LPARAM(lparam) / scale, GET_Y_LPARAM(lparam) / scale},
                                 ui::TouchEvent::Type::End);
            }
            return 0;
        }

        case WM_KILLFOCUS: {
            /* Do not leave keys latched down when the window loses focus. */
            input::reset();
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;  /* we paint every pixel ourselves */

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            if (impl->mem_dc) {
                SetStretchBltMode(dc, COLORONCOLOR);
                StretchBlt(dc, 0, 0, ui::screen_width * scale, ui::screen_height * scale,
                           impl->mem_dc, 0, 0, ui::screen_width, ui::screen_height, SRCCOPY);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        default:
            break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

Window::Window() = default;

Window::~Window() {
    destroy();
}

bool Window::create(const std::string& title, int scale, bool hidden) {
    scale_ = std::clamp(scale, 1, 6);
    impl_ = new Impl();
    impl_->owner = this;

    HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kClassName;
    /* Re-registering the same class fails harmlessly if we are recreated. */
    RegisterClassExW(&wc);

    RECT r{0, 0, ui::screen_width * scale_, ui::screen_height * scale_};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&r, style, FALSE);

    impl_->hwnd = CreateWindowExW(0, kClassName, widen(title).c_str(), style,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  r.right - r.left, r.bottom - r.top,
                                  nullptr, nullptr, inst, nullptr);
    if (impl_->hwnd == nullptr) {
        delete impl_;
        impl_ = nullptr;
        return false;
    }

    SetWindowLongPtrW(impl_->hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(impl_));

    /* A top-down 32bpp DIB section: composite_bgra() writes straight into it. */
    HDC dc = GetDC(impl_->hwnd);
    impl_->mem_dc = CreateCompatibleDC(dc);

    impl_->bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    impl_->bmi.bmiHeader.biWidth = ui::screen_width;
    impl_->bmi.bmiHeader.biHeight = -static_cast<LONG>(ui::screen_height);  /* top-down */
    impl_->bmi.bmiHeader.biPlanes = 1;
    impl_->bmi.bmiHeader.biBitCount = 32;
    impl_->bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    impl_->dib = CreateDIBSection(dc, &impl_->bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(impl_->hwnd, dc);

    if (impl_->dib == nullptr || bits == nullptr) {
        destroy();
        return false;
    }
    impl_->pixels = static_cast<uint32_t*>(bits);
    SelectObject(impl_->mem_dc, impl_->dib);

    /* The tray icon is registered whether or not the window is showing: it is
     * the graceful-quit affordance, and with --hidden (and the launcher hiding
     * the console too) it is the ONLY one the user has. Failure is not fatal —
     * a missing tray icon must not stop the radio from running. */
    {
        HICON icon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(kAppIconResourceId),
                                                   IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                   GetSystemMetrics(SM_CYSMICON), 0));
        /* IDI_APPLICATION spelled out: the project does not define UNICODE, so
         * the bare macro would expand to the ANSI form and not match LoadIconW. */
        if (icon == nullptr) icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));

        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = impl_->hwnd;
        nid.uID = kTrayIconId;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = kTrayCallbackMsg;
        nid.hIcon = icon;
        const std::wstring tip = widen(title);
        wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
        impl_->tray_added = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    }

    visible_ = !hidden;
    ShowWindow(impl_->hwnd, hidden ? SW_HIDE : SW_SHOW);
    if (!hidden) UpdateWindow(impl_->hwnd);
    return true;
}

void Window::set_visible(bool visible) {
    if (impl_ == nullptr || impl_->hwnd == nullptr) return;
    visible_ = visible;
    ShowWindow(impl_->hwnd, visible ? SW_SHOW : SW_HIDE);
    if (visible) {
        SetForegroundWindow(impl_->hwnd);
        /* The framebuffer has not changed, so present() would skip the blit;
         * force it or the restored window comes back blank. */
        present(true);
    }
}

void Window::destroy() {
    if (impl_ == nullptr) return;
    if (impl_->tray_added && impl_->hwnd) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = impl_->hwnd;
        nid.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &nid);
        impl_->tray_added = false;
    }
    if (impl_->mem_dc) DeleteDC(impl_->mem_dc);
    if (impl_->dib) DeleteObject(impl_->dib);
    if (impl_->hwnd) DestroyWindow(impl_->hwnd);
    delete impl_;
    impl_ = nullptr;
    closed_ = true;
}

void Window::set_scale(int scale) {
    if (impl_ == nullptr || impl_->hwnd == nullptr) return;
    scale_ = std::clamp(scale, 1, 6);

    RECT r{0, 0, ui::screen_width * scale_, ui::screen_height * scale_};
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(impl_->hwnd, GWL_STYLE));
    AdjustWindowRect(&r, style, FALSE);
    SetWindowPos(impl_->hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                 SWP_NOMOVE | SWP_NOZORDER);
    present(true);
}

bool Window::pump() {
    if (impl_ == nullptr) return false;

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            closed_ = true;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !closed_;
}

void Window::present(bool force) {
    if (impl_ == nullptr || impl_->pixels == nullptr) return;

    /* Nothing to blit to while hidden. The damage flag is deliberately left
     * unconsumed so the first present() after set_visible(true) — which passes
     * force anyway — repaints a complete frame. The portal is unaffected: it
     * reads the display's separate non-destructive damage counter. */
    if (!visible_ && !force) return;

    const bool damaged = display.take_damage();
    if (!damaged && !force) return;

    display.composite_bgra(impl_->pixels);

    HDC dc = GetDC(impl_->hwnd);
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchBlt(dc, 0, 0, ui::screen_width * scale_, ui::screen_height * scale_,
               impl_->mem_dc, 0, 0, ui::screen_width, ui::screen_height, SRCCOPY);
    ReleaseDC(impl_->hwnd, dc);
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
    if (impl_ == nullptr || impl_->hwnd == nullptr) return;
    SetWindowTextW(impl_->hwnd, widen(title).c_str());
}

}  // namespace host

#endif /* _WIN32 */
