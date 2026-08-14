/*
 * mayhem-b200 — entry point.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "apps/about_app.hpp"
#include "core/telemetry.hpp"
#include "apps/app_context.hpp"
#include "apps/event_dispatch.hpp"
#include "apps/main_menu.hpp"
#include "apps/ui_navigation.hpp"
#include "audio/audio_in.hpp"
#include "audio/audio_out.hpp"
#include "core/file_path.hpp"
#include "radio/network_radio.hpp"
#include "radio/receiver_model.hpp"
#include "radio/transmitter_model.hpp"
#include "radio/usrp_radio.hpp"
#include "remote/app_bridge.hpp"
#include "remote/remote_server.hpp"
#include "ui/display.hpp"
#include "ui/input.hpp"
#include "ui/theme.hpp"
#include "ui/ui_painter.hpp"
#include "ui/ui_widget.hpp"
#include "ui/window.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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
    std::string driver{"uhd"};
    std::string device_args{};
    int scale{2};
    bool list_devices{false};
    bool help{false};

    /* --portal[=port]: off by default. See remote/remote_server.hpp for what
     * it serves and the "no authentication, LAN trust boundary" note. */
    bool portal_enabled{false};
    uint16_t portal_port{8090};

    /* Anonymous once-a-day usage ping; on by default, --no-telemetry disables
     * it. Inert anyway unless the build set core::telemetry::kTelemetryEndpoint.
     * See src/core/telemetry.hpp. */
    bool telemetry_enabled{true};
};

Options parse_args(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            o.help = true;
        } else if (arg == "--list") {
            o.list_devices = true;
        } else if (arg.rfind("--driver=", 0) == 0) {
            o.driver = arg.substr(9);
        } else if (arg.rfind("--args=", 0) == 0) {
            o.device_args = arg.substr(7);
        } else if (arg.rfind("--scale=", 0) == 0) {
            o.scale = std::atoi(arg.c_str() + 8);
        } else if (arg == "--portal") {
            o.portal_enabled = true;
        } else if (arg.rfind("--portal=", 0) == 0) {
            o.portal_enabled = true;
            o.portal_port = static_cast<uint16_t>(std::atoi(arg.c_str() + 9));
        } else if (arg == "--no-telemetry") {
            o.telemetry_enabled = false;
        }
    }
    return o;
}

void print_help() {
    std::printf(
        "mayhem-b200 %s - PortaPack Mayhem for the Ettus USRP B200\n"
        "\n"
        "Usage: mayhem-b200 [options]\n"
        "  --driver=<uhd|sdrlink>  Radio backend (default uhd)\n"
        "  --args=<backend args>   uhd: device address, e.g. type=b200,serial=31C9297\n"
        "                          sdrlink: host[:port] of an sdrlink server, e.g.\n"
        "                          --driver=sdrlink --args=127.0.0.1:5960\n"
        "  --scale=<1..6>          Window magnification (default 2)\n"
        "  --list                  List attached USRP devices and exit (uhd only)\n"
        "  --portal[=port]         Serve the JSON web portal API (default port 8090).\n"
        "  --no-telemetry          Disable the anonymous usage ping (see docs).\n"
        "                          LAN-reachable and unauthenticated. Off unless given.\n"
        "                          It streams the screen and accepts input, so anyone\n"
        "                          who can reach the port can operate this radio,\n"
        "                          transmit apps included -- including a web page you\n"
        "                          merely visit, which can post to 127.0.0.1.\n"
        "  --help                  This text\n"
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

    /* --driver selects the backend behind the RadioDevice interface; nothing
     * past this point needs to know which one it is talking to. */
    std::unique_ptr<radio::RadioDevice> radio_backend;
    if (options.driver == "sdrlink") {
        radio_backend = std::make_unique<radio::NetworkRadio>();
    } else if (options.driver == "uhd") {
        radio_backend = std::make_unique<radio::UsrpRadio>();
    } else {
        std::fprintf(stderr, "unknown --driver=%s (expected uhd or sdrlink)\n", options.driver.c_str());
        return 1;
    }
    radio::RadioDevice& radio = *radio_backend;

    audio::AudioOut audio_out;

    const bool radio_ok = radio.open(options.device_args);
    if (!radio_ok) {
        /* Not fatal. The UI is useful without hardware, and Radio setup has a
         * Reconnect button for when the device shows up. */
        std::fprintf(stderr, "no %s device opened: %s\n", radio.driver_name(), radio.last_error().c_str());
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

    /* Anonymous usage ping (opt-out): counts distinct installs, nothing more.
     * See src/core/telemetry.hpp. Disclosed here so it is never silent. */
    if (options.telemetry_enabled && core::telemetry::kTelemetryEndpoint[0] != '\0') {
        std::printf("Anonymous usage ping enabled (random install id only; "
                    "disable with --no-telemetry).\n");
    }
    core::telemetry::ping_on_startup(app::kVersion, options.telemetry_enabled);

    /* The portal is pumped from this same thread's main loop below
     * (drain_launch_queue() + refresh(), both UI-thread-only by contract —
     * see remote/app_bridge.hpp's file header) rather than getting its own
     * "UI" thread: there already is exactly one thread that owns
     * app::globals().nav and every app's view state, and every design here
     * is about respecting that, not adding a second one. RemoteServer's own
     * accept loop and per-connection handlers do run on their own threads
     * (necessarily — Winsock's accept()/recv() block), but they only ever
     * reach into app state through AppBridge's queue and cache, never
     * directly. */
    remote::RemoteServer portal_server;
    if (options.portal_enabled) {
        if (portal_server.start(options.portal_port)) {
            std::printf("Web portal: http://127.0.0.1:%u/\n", portal_server.port());
        } else {
            std::fprintf(stderr, "portal failed to start: %s\n", portal_server.last_error().c_str());
        }
    }

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

        /* Remote input joins the local queue's stream right here, so a portal
         * client's key reaches EventDispatcher on the same thread and in the
         * same frame a local one would. The two-step for keys is copied from
         * window.cpp/window_linux.cpp verbatim: the long-press timer is fed
         * by both transitions, but only the press dispatches an Event.
         * dispatch()'s return is ignored on purpose — false means Quit, and
         * the portal must not be able to close the application (the queue
         * cannot carry one; see remote::parse_input_events). */
        if (options.portal_enabled) {
            std::vector<remote::RemoteInput> remote_input;
            if (remote::AppBridge::instance().drain_input_queue(remote_input)) {
                for (const auto& ri : remote_input) {
                    if (ri.action == remote::RemoteInput::Action::KeyUp) {
                        input::note_key_up(ri.event.key);
                        continue;
                    }
                    if (ri.action == remote::RemoteInput::Action::KeyDown)
                        input::note_key_down(ri.event.key, input::now_ms());
                    dispatcher.dispatch(ri.event);
                }
            }
        }

        /* Applies any queued portal launch()/home() request before service()
         * so the same frame's service() call both performs it and reports
         * the change, exactly as a locally-driven push/pop would. */
        if (options.portal_enabled) remote::AppBridge::instance().drain_launch_queue();
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

        /* Recomputes the cached status/current-app/panel JSON the HTTP
         * handlers read. UI-thread-only, once per frame — see
         * AppBridge::refresh()'s own doc for why. */
        if (options.portal_enabled) remote::AppBridge::instance().refresh();

        system_view.on_frame_sync();
        painter.paint_widget_tree(&system_view);
        window.present();

        /* After present(), not before: the framebuffer only holds this frame
         * once paint_widget_tree() has run, and capturing from inside
         * refresh() above would publish the previous one. Reads the display's
         * non-destructive damage counter, so it cannot steal present()'s
         * repaint signal on either platform. */
        if (options.portal_enabled) remote::AppBridge::instance().capture_screen_frame();

        /* ~60 Hz, sleeping against an absolute deadline so a slow frame does
         * not make every later frame late. */
        next_frame += std::chrono::milliseconds(16);
        const auto now = std::chrono::steady_clock::now();
        if (next_frame > now)
            std::this_thread::sleep_for(next_frame - now);
        else
            next_frame = now;
    }

    portal_server.stop();

    receiver.stop();
    transmitter.stop();
    audio_in.stop();
    radio.close();
    audio_out.stop();
    window.destroy();

    return 0;
}
