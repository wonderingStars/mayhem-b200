/*
 * mayhem-b200 — entry point.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "apps/about_app.hpp"
#include "apps/app_context.hpp"
#include "apps/event_dispatch.hpp"
#include "apps/main_menu.hpp"
#include "apps/ui_navigation.hpp"
#include "audio/audio_in.hpp"
#include "audio/audio_out.hpp"
#include "core/file_path.hpp"
#include "radio/receiver_model.hpp"
#include "radio/transmitter_model.hpp"
#include "radio/usrp_radio.hpp"
#include "ui/display.hpp"
#include "ui/theme.hpp"
#include "ui/ui_painter.hpp"
#include "ui/ui_widget.hpp"
#include "ui/window.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#if defined(_WIN32)
#include <windows.h>  /* GetModuleFileNameW for the bundled-images lookup */
#endif

namespace {

/* Root of the widget tree: the status bar on top, the view stack below. */
class SystemView : public ui::View {
   public:
    SystemView()
        : ui::View{{0, 0, ui::screen_width, ui::screen_height}},
          status_{{0, 0, ui::screen_width, ui::SystemStatusView::status_height}},
          navigation_{{0, ui::SystemStatusView::status_height, ui::screen_width,
                       ui::screen_height - ui::SystemStatusView::status_height}} {
        set_style(ui::Theme::getInstance()->bg_darkest);
        add_children({&status_, &navigation_});

        status_.on_back = [this] { navigation_.pop(); };
        navigation_.on_view_changed = [this] { refresh_status(); };
    }

    ui::NavigationView& navigation() { return navigation_; }
    ui::SystemStatusView& status() { return status_; }

    void refresh_status() {
        status_.set_title(navigation_.current_title());
        status_.set_back_enabled(!navigation_.is_root());
    }

   private:
    ui::SystemStatusView status_;
    ui::NavigationView navigation_;
};

struct Options {
    std::string device_args{};
    int scale{2};
    bool list_devices{false};
    bool help{false};
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            o.help = true;
        } else if (arg == "--list") {
            o.list_devices = true;
        } else if (arg.rfind("--args=", 0) == 0) {
            o.device_args = arg.substr(7);
        } else if (arg.rfind("--scale=", 0) == 0) {
            o.scale = std::atoi(arg.c_str() + 8);
        }
    }
    return o;
}

void print_help() {
    std::printf(
        "mayhem-b200 %s - PortaPack Mayhem for the Ettus USRP B200\n"
        "\n"
        "Usage: mayhem-b200 [options]\n"
        "  --args=<uhd args>  Device address, e.g. --args=type=b200,serial=31C9297\n"
        "  --scale=<1..6>     Window magnification (default 2)\n"
        "  --list             List attached USRP devices and exit\n"
        "  --help             This text\n"
        "\n"
        "Controls: arrows navigate and tune, Enter selects, Esc goes back,\n"
        "mouse wheel is the encoder, the mouse is the touch screen, F11 rescales.\n",
        app::kVersion);
}

#if defined(_WIN32)
/* In a downloaded release the UHD firmware/FPGA images ship in an `uhd-images`
 * folder next to the exe. UHD looks them up via UHD_IMAGES_DIR, so if the user
 * has not set it and that bundled folder exists, point UHD at it. This is what
 * lets the packaged build initialise a B200 without a separate UHD install. */
void use_bundled_uhd_images() {
    if (const char* existing = std::getenv("UHD_IMAGES_DIR");
        existing != nullptr && existing[0] != '\0')
        return;  /* respect an explicit user setting */

    wchar_t exe[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    std::wstring path{exe, n};
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    path.resize(slash);
    path += L"\\uhd-images";

    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    _wputenv_s(L"UHD_IMAGES_DIR", path.c_str());
}
#else
void use_bundled_uhd_images() {}
#endif

}  // namespace

int main(int argc, char** argv) {
    use_bundled_uhd_images();

    const Options options = parse_args(argc, argv);

    if (options.help) {
        print_help();
        return 0;
    }

    if (options.list_devices) {
        const auto devices = radio::UsrpRadio::find("");
        if (devices.empty()) {
            std::printf("No USRP devices found.\n");
            return 1;
        }
        for (const auto& d : devices)
            std::printf("%s\n  args: %s\n", d.label().c_str(), d.args.c_str());
        return 0;
    }

    /* Make sure the data folders exist before any app tries to write there. */
    core::ensure_directory(core::captures_directory());
    core::ensure_directory(core::freqman_directory());

    host::display.init();

    host::Window window;
    if (!window.create(std::string{"Mayhem B200 "} + app::kVersion, options.scale)) {
        std::fprintf(stderr, "failed to create window\n");
        return 1;
    }

    radio::UsrpRadio radio;
    audio::AudioOut audio_out;

    const bool radio_ok = radio.open(options.device_args);
    if (!radio_ok) {
        /* Not fatal. The UI is useful without hardware, and Radio setup has a
         * Reconnect button for when the device shows up. */
        std::fprintf(stderr, "no USRP opened: %s\n", radio.last_error().c_str());
    }

    if (!audio_out.start()) {
        std::fprintf(stderr, "audio output unavailable: %s\n", audio_out.last_error().c_str());
    }

    radio::ReceiverModel receiver{radio, audio_out};
    receiver.set_target_frequency(100'000'000);
    receiver.set_volume(40);
    if (radio_ok) receiver.set_gain(40);

    /* The transmit chain and mic capture the TX / Mic apps need. Constructed but
     * idle — nothing transmits or captures until an app explicitly starts it. */
    radio::TransmitterModel transmitter{radio};
    audio::AudioIn audio_in;

    SystemView system_view;

    auto& ctx = app::globals();
    ctx.radio = &radio;
    ctx.receiver = &receiver;
    ctx.transmitter = &transmitter;
    ctx.audio_out = &audio_out;
    ctx.audio_in = &audio_in;
    ctx.nav = &system_view.navigation();

    system_view.navigation().push_new<app::MainMenuView>();
    system_view.navigation().service();
    system_view.refresh_status();

    ui::Painter painter;
    app::EventDispatcher dispatcher{system_view, system_view.context()};
    dispatcher.on_back = [&system_view] { system_view.navigation().pop(); };

    bool running = true;
    auto next_frame = std::chrono::steady_clock::now();

    while (running && window.pump()) {
        host::Event event;
        while (window.poll_event(event)) {
            if (!dispatcher.dispatch(event)) {
                running = false;
                break;
            }
        }
        if (!running) break;

        if (system_view.navigation().service()) system_view.refresh_status();

        /* Status bar reflects the live radio state. */
        auto& status = system_view.status();
        if (radio.is_open()) {
            status.set_device_text(radio.caps().mboard);
            status.set_device_ok(true);
        } else {
            status.set_device_text("no dev");
            status.set_device_ok(false);
        }
        status.set_receiving(receiver.running());
        status.set_transmitting(radio.tx_running());

        system_view.on_frame_sync();
        painter.paint_widget_tree(&system_view);
        window.present();

        /* ~60 Hz, sleeping against an absolute deadline so a slow frame does
         * not make every later frame late. */
        next_frame += std::chrono::milliseconds(16);
        const auto now = std::chrono::steady_clock::now();
        if (next_frame > now)
            std::this_thread::sleep_for(next_frame - now);
        else
            next_frame = now;
    }

    receiver.stop();
    transmitter.stop();
    audio_in.stop();
    radio.close();
    audio_out.stop();
    window.destroy();

    return 0;
}
