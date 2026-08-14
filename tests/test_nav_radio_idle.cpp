/*
 * mayhem-b200 — leaving an app leaves the radio idle.
 *
 * Reported from live use (2026-08-14): back out of any receive app to the
 * menu and the B200 keeps streaming. Scanner's destructor even documented
 * the old behaviour — "Leave the radio streaming for the menu; navigation
 * mutes audio" — which mutes the SPEAKER while the radio, the USB bus and
 * the host thread all keep working on samples nobody is decoding. The
 * handheld this ports stops its baseband when an app closes; the menu
 * must mean idle here too.
 *
 * The stop is central — NavigationView::service() after a batch of ops
 * lands at the root — precisely so 90+ apps need no edits and a future app
 * cannot forget. These tests drive the REAL navigation service loop with a
 * real ReceiverModel over the test radio.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_context.hpp"
#include "audio_out.hpp"
#include "counter_radio.hpp"
#include "receiver_model.hpp"
#include "ui_navigation.hpp"
#include "ui_widget.hpp"

#include <memory>

namespace {

/* A view that starts the receiver on show, the way every RX app does. */
class RxAppStub : public ui::View {
   public:
    explicit RxAppStub(radio::ReceiverModel& rx) : rx_{rx} {}
    std::string title() const override { return "RxAppStub"; }
    void on_show() override {
        ui::View::on_show();
        if (!rx_.running()) rx_.start();
    }

   private:
    radio::ReceiverModel& rx_;
};

struct NavHarness {
    mb200test::CounterRadio counter_radio{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{counter_radio, audio};
    ui::NavigationView nav{{0, 0, 240, 304}};

    radio::RadioDevice* saved_radio{app::globals().radio};
    radio::ReceiverModel* saved_receiver{app::globals().receiver};
    ui::NavigationView* saved_nav{app::globals().nav};

    NavHarness() {
        app::globals().radio = &counter_radio;
        app::globals().receiver = &receiver;
        app::globals().nav = &nav;
        /* A root view, as main.cpp installs the menu: the stack is never
         * empty in real life, and "root only" is what the menu looks like. */
        nav.push(std::make_unique<ui::View>());
        nav.service();
    }

    ~NavHarness() {
        receiver.stop();
        app::globals().radio = saved_radio;
        app::globals().receiver = saved_receiver;
        app::globals().nav = saved_nav;
    }

    NavHarness(const NavHarness&) = delete;
    NavHarness& operator=(const NavHarness&) = delete;

    void open_rx_app() {
        nav.push(std::make_unique<RxAppStub>(receiver));
        nav.service();
    }
};

}  // namespace

TEST(leaving_an_app_for_the_menu_stops_the_receiver) {
    NavHarness h;
    h.open_rx_app();
    CHECK(h.receiver.running());

    h.nav.pop_to_root();
    h.nav.service();

    CHECK(h.nav.is_root());
    CHECK(!h.receiver.running());
}

TEST(an_app_to_app_switch_does_not_bounce_the_radio_through_a_stop) {
    /* drain_launch_queue() queues PopToRoot+Push as ONE batch; the stack only
     * momentarily passes through the root inside service(). Stopping there
     * would add a needless stop/start cycle to every app switch — the rule
     * is about where the batch LANDS, not what it passed through. */
    NavHarness h;
    h.open_rx_app();
    CHECK(h.receiver.running());

    h.nav.pop_to_root();
    h.nav.push(std::make_unique<RxAppStub>(h.receiver));
    h.nav.service(); /* one batch, lands on an app */

    CHECK(!h.nav.is_root());
    CHECK(h.receiver.running());
}

TEST(a_sub_view_pop_inside_an_app_does_not_stop_the_receiver) {
    /* Backing out of an app's own detail page (AIS ship detail, a GeoMap)
     * returns to the APP, not the menu; the radio must keep running. */
    NavHarness h;
    h.open_rx_app();
    h.nav.push(std::make_unique<ui::View>()); /* a detail page */
    h.nav.service();
    CHECK(h.receiver.running());

    h.nav.pop();
    h.nav.service();

    CHECK(!h.nav.is_root());
    CHECK(h.receiver.running());
}
