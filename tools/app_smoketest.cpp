/*
 * mayhem-b200 — radio app smoke test.
 *
 * Walks the app registry and exercises every radio app (Receive / Transmit /
 * Transceiver, plus the radio Home apps) against whatever device is attached:
 *
 *   - Receive / Transceiver / Home apps get the full runtime lifecycle:
 *     construct -> on_show (which starts the receiver) -> ~40 frames of
 *     on_frame_sync + paint against live samples -> on_hide. This proves the
 *     app's per-frame pipeline runs on real hardware without crashing.
 *
 *   - Transmit apps are constructed and painted only. on_show is NOT called and
 *     nothing is ever keyed, so this NEVER radiates. Confirms the view builds
 *     and renders; the encoders themselves are covered by the unit tests.
 *
 * Every app runs inside try/catch so one failure does not abort the sweep. Games
 * and pure utilities are skipped (this is the radio sweep).
 *
 * Build:  cmake --build build --target mb200_appsmoke
 * Run:    build\mb200_appsmoke.exe
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "apps/app_context.hpp"
#include "apps/app_registry.hpp"
#include "audio/audio_in.hpp"
#include "audio/audio_out.hpp"
#include "radio/receiver_model.hpp"
#include "radio/transmitter_model.hpp"
#include "radio/usrp_radio.hpp"
#include "ui/display.hpp"
#include "ui/ui_painter.hpp"
#include "ui/ui_widget.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using app::Category;

namespace {

bool is_radio(Category c) {
    return c == Category::Receive || c == Category::Transmit ||
           c == Category::Transceiver || c == Category::Home;
}

const char* cat(Category c) { return app::category_name(c); }

}  // namespace

int main(int argc, char** argv) {
    /* Modes:
     *   (no args)        sweep every radio app in one process (aborts on a crash)
     *   --ids            print each radio app id, one per line (for a driver loop)
     *   --app <id>       run just that one app, then exit — so a driver can run
     *                    each in its own process and a segfault only loses one
     *   [uhd args]       anything else is passed to radio.open()
     */
    std::string args;
    std::string only_id;
    bool list_ids = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--ids") {
            list_ids = true;
        } else if (a == "--app" && i + 1 < argc) {
            only_id = argv[++i];
        } else {
            args = a;
        }
    }

    if (list_ids) {
        for (const auto& e : app::AppRegistry::instance().all())
            if (is_radio(e.category)) std::printf("%s\n", e.id.c_str());
        return 0;
    }

    std::printf("=== mayhem-b200 radio app smoke test ===\n\n");

    host::display.init();

    radio::UsrpRadio radio;
    const bool radio_ok = radio.open(args);
    std::printf("radio: %s\n", radio_ok ? radio.caps().mboard.c_str() : "NOT OPEN (running dry)");

    audio::AudioOut audio_out;
    audio_out.start();
    audio::AudioIn audio_in;
    radio::ReceiverModel receiver{radio, audio_out};
    receiver.set_target_frequency(100'000'000);
    if (radio_ok) receiver.set_gain(40);
    radio::TransmitterModel transmitter{radio};

    auto& ctx = app::globals();
    ctx.radio = &radio;
    ctx.receiver = &receiver;
    ctx.transmitter = &transmitter;
    ctx.audio_out = &audio_out;
    ctx.audio_in = &audio_in;
    ctx.nav = nullptr;  /* views must tolerate a null nav until a button is pressed */

    ui::Painter painter;
    const ui::Rect area{0, 16, ui::screen_width, ui::screen_height - 16};

    int total = 0, ok = 0, failed = 0, skipped = 0;
    std::vector<std::string> failures;

    /* Snapshot the ids first: creating views must not invalidate the registry. */
    std::vector<const app::AppEntry*> entries;
    for (const auto& e : app::AppRegistry::instance().all()) entries.push_back(&e);

    for (const auto* e : entries) {
        if (!is_radio(e->category)) {
            skipped++;
            continue;
        }
        if (!only_id.empty() && e->id != only_id) continue;  /* single-app mode */
        total++;

        const bool tx = (e->category == Category::Transmit);
        std::printf("  %-14s %-16s [%s] ... ", cat(e->category), e->id.c_str(),
                    tx ? "build" : "run ");
        std::fflush(stdout);

        try {
            auto view = e->factory();
            if (!view) {
                std::printf("FAIL (null view)\n");
                failed++;
                failures.push_back(e->id + " (null view)");
                continue;
            }

            view->set_parent_rect(area);

            if (tx) {
                /* Build + paint only. Never on_show, never transmit. */
                for (int f = 0; f < 3; f++) {
                    painter.paint_widget_tree(view.get());
                    ui::dirty_set();
                }
            } else {
                view->on_show();  /* starts the receiver for RX apps */
                for (int f = 0; f < 40; f++) {
                    view->on_frame_sync();
                    ui::dirty_set();
                    painter.paint_widget_tree(view.get());
                    std::this_thread::sleep_for(12ms);  /* ~0.5 s of live samples */
                }
                view->on_hide();
                receiver.stop();  /* reset for the next RX app */
                std::this_thread::sleep_for(60ms);
            }

            std::printf("ok\n");
            ok++;
        } catch (const std::exception& ex) {
            std::printf("FAIL (%s)\n", ex.what());
            failed++;
            failures.push_back(e->id + " (" + ex.what() + ")");
            receiver.stop();
        } catch (...) {
            std::printf("FAIL (non-std exception)\n");
            failed++;
            failures.push_back(e->id + " (unknown)");
            receiver.stop();
        }
    }

    receiver.stop();
    transmitter.stop();
    audio_in.stop();
    audio_out.stop();
    radio.close();

    std::printf("\n=== %d radio apps: %d ok, %d failed (%d non-radio skipped) ===\n",
                total, ok, failed, skipped);
    if (!failures.empty()) {
        std::printf("failures:\n");
        for (const auto& f : failures) std::printf("  - %s\n", f.c_str());
    }
    return failed == 0 ? 0 : 1;
}
