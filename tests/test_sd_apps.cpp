/*
 * mayhem-b200 — SD-card app tests (SD over USB, Wipe SD card).
 *
 * Both apps port PortaPack features that depend on a microSD card a B200 host
 * does not have. The behaviour under test is precisely that: they are flagged
 * hardware_limited, they report "unavailable", and — critically for Wipe SD —
 * the destructive action is a guaranteed no-op that never touches any disk.
 * There is no upstream value to reproduce; the contract is "does nothing".
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_registry.hpp"
#include "ui_sd_over_usb.hpp"
#include "ui_sd_wipe.hpp"

using app::AppRegistry;
using app::Category;
using mb200test::report_failure;

/* ---- SD over USB: pure decision reports unavailable, starts nothing ---- */

TEST(sdusb_attempt_start_is_a_noop) {
    const auto r = app::SdOverUsbView::attempt_start();
    CHECK(r.started == false);         /* nothing is ever started */
    CHECK(!r.message.empty());         /* and it says why */
}

TEST(sdusb_attempt_start_is_stable_across_calls) {
    /* No hidden state that could "succeed" on a later press. */
    for (int i = 0; i < 5; i++) {
        const auto r = app::SdOverUsbView::attempt_start();
        CHECK(r.started == false);
    }
}

TEST(sdusb_registered_as_hardware_limited_utility) {
    const auto* e = AppRegistry::instance().by_id("sdusb");
    CHECK(e != nullptr);
    if (!e) return;
    CHECK(e->hardware_limited == true);
    CHECK(e->category == Category::Utilities);
    CHECK(e->color.v == ui::Color::yellow().v);  /* upstream icon_color */
    CHECK(static_cast<bool>(e->factory));
    CHECK_STR_EQ(e->display_name, "SD over USB");
}

/* ---- Wipe SD: refuses, wipes zero bytes, never touches the host disk ---- */

TEST(sd_wipe_attempt_wipe_refuses_and_wipes_nothing) {
    const auto r = app::WipeSDView::attempt_wipe();
    CHECK(r.performed == false);   /* the destructive action never runs */
    CHECK_EQ(r.bytes_wiped, 0ull); /* not one byte is written */
    CHECK(!r.message.empty());     /* and it says why it refused */
}

TEST(sd_wipe_attempt_wipe_is_idempotent_noop) {
    /* Pressing "Wipe" repeatedly must never escalate into doing anything. */
    uint64_t total = 0;
    for (int i = 0; i < 10; i++) {
        const auto r = app::WipeSDView::attempt_wipe();
        CHECK(r.performed == false);
        total += r.bytes_wiped;
    }
    CHECK_EQ(total, 0ull);
}

TEST(sd_wipe_registered_as_hardware_limited_utility) {
    const auto* e = AppRegistry::instance().by_id("sd_wipe");
    CHECK(e != nullptr);
    if (!e) return;
    CHECK(e->hardware_limited == true);
    CHECK(e->category == Category::Utilities);
    CHECK(e->color.v == ui::Color::red().v);  /* upstream icon_color */
    CHECK(static_cast<bool>(e->factory));
    CHECK_STR_EQ(e->display_name, "Wipe SD card");
}
