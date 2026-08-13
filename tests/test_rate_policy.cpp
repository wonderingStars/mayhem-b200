/*
 * mayhem-b200 — capability-driven RX sample-rate selection tests.
 *
 * The point of radio::choose_rx_rate() is that an app states constraints and
 * gets a rate suited to whatever radio is plugged in, so the tests are written
 * the same way: a table of real device profiles crossed with real app
 * preferences. No hardware is involved — the whole helper is a function from a
 * DeviceCaps to a number, which is exactly why it can be tested against
 * dongles nobody here owns.
 *
 * Rate figures used as profiles:
 *   B200        the attached device, as tools/hw_selftest.cpp read it back on
 *               a USB 2 port (README.md: 42 MHz - 6 GHz, up to 16 Msps)
 *   B200 (spec) radio::default_b200_caps(), the published range used when
 *               nothing is connected: 200 kHz - 61.44 Msps, 1 Hz step
 *   RTL-SDR     225.001 kHz floor; 2.56 Msps is the usual drop-free ceiling,
 *               3.2 Msps the absolute one
 *   HackRF One  2 - 20 Msps
 *   AD936x      a Pluto-class device publishing the part's full range, far
 *               above what its USB link can actually carry
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "rate_policy.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

/* ADS-B: 1 Mbit/s PPM. Two samples per bit is the bare minimum to decode at
 * all, eight is where the gain flattens, and the demodulator resamples to
 * 2 Msps so an exact integer ratio is worth having. */
radio::RatePreference adsb_preference() {
    radio::RatePreference want;
    want.ideal_hz = 8e6;
    want.minimum_hz = 2e6;
    want.max_useful_hz = 8e6;
    want.prefer_multiple_of_hz = 2e6;
    return want;
}

radio::DeviceCaps caps_with_rx_rate(radio::Range r) {
    radio::DeviceCaps c;
    c.rx_rate = r;
    return c;
}

/* Failure output that says WHICH profile broke, which a bare CHECK_NEAR over a
 * table cannot. Every expected rate in the tables below is a whole number of
 * hertz, so formatting to %.0f loses nothing. */
std::string labelled_hz(const char* profile, double hz) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s: %.0f Hz", profile, hz);
    return buf;
}

std::string labelled_outcome(const char* profile, radio::RateOutcome o) {
    return std::string{profile} + ": " + radio::rate_outcome_to_string(o);
}

struct Profile {
    const char* name;
    radio::Range rx_rate;
};

const Profile kProfiles[] = {
    {"B200",        {31.25e3, 16e6, 0.0}},
    {"B200 (spec)", {200e3, 61.44e6, 1.0}},
    {"RTL-SDR",     {225001.0, 2.56e6, 0.0}},
    {"RTL-SDR max", {225001.0, 3.2e6, 0.0}},
    {"HackRF One",  {2e6, 20e6, 0.0}},
    {"AD936x",      {520833.0, 61.44e6, 0.0}},
    {"narrowband",  {48e3, 192e3, 0.0}},
    {"empty caps",  {0.0, 0.0, 0.0}},
};

}  // namespace

TEST(rate_policy_picks_the_right_rate_per_device_profile) {
    struct Expect {
        const char* profile;
        double rate_hz;
        radio::RateOutcome outcome;
    };

    /* One preference — ADS-B's — across every profile. This is the whole point
     * of the helper in one table: the same app gets 8 Msps on a B200 and
     * 2 Msps on a dongle, without either number appearing in the app. */
    const Expect expected[] = {
        /* 16 Msps available, but ADS-B gains nothing past 8. */
        {"B200", 8e6, radio::RateOutcome::Ideal},
        /* 61.44 Msps published; the ceiling still holds. */
        {"B200 (spec)", 8e6, radio::RateOutcome::Ideal},
        /* Cannot reach 8, so the largest multiple of 2 it can hold. Two
         * samples per bit: degraded, but ADS-B still decodes. */
        {"RTL-SDR", 2e6, radio::RateOutcome::Reduced},
        {"RTL-SDR max", 2e6, radio::RateOutcome::Reduced},
        {"HackRF One", 8e6, radio::RateOutcome::Ideal},
        {"AD936x", 8e6, radio::RateOutcome::Ideal},
        /* 192 ksps cannot carry a 1 Mbit/s signal at all. */
        {"narrowband", 192e3, radio::RateOutcome::BelowMinimum},
        /* Nothing to go on: hand back what the app asked for and say so. */
        {"empty caps", 8e6, radio::RateOutcome::CapsUnusable},
    };

    static_assert(sizeof(expected) / sizeof(expected[0]) ==
                      sizeof(kProfiles) / sizeof(kProfiles[0]),
                  "one expectation per device profile");
    const size_t n = sizeof(kProfiles) / sizeof(kProfiles[0]);

    for (size_t i = 0; i < n; ++i) {
        const Profile& p = kProfiles[i];
        const Expect& e = expected[i];
        CHECK_STR_EQ(p.name, e.profile);

        const auto got = radio::choose_rx_rate(caps_with_rx_rate(p.rx_rate),
                                               adsb_preference());
        CHECK_STR_EQ(labelled_hz(p.name, got.rate_hz),
                     labelled_hz(p.name, e.rate_hz));
        CHECK_STR_EQ(labelled_outcome(p.name, got.outcome),
                     labelled_outcome(p.name, e.outcome));
    }
}

TEST(rate_policy_never_leaves_the_device_range) {
    /* The one hard guarantee: whatever the app asks for, the answer is a rate
     * the device says it can do. Crossed over deliberately awkward requests —
     * far too high, far too low, contradictory, and empty. */
    radio::RatePreference requests[5];
    requests[0] = adsb_preference();
    requests[1].ideal_hz = 200e6;                          /* absurdly high */
    requests[2].ideal_hz = 1.0;                            /* absurdly low */
    requests[3].ideal_hz = 5e6;
    requests[3].minimum_hz = 40e6;                         /* floor over ceiling */
    requests[3].max_useful_hz = 1e6;
    requests[4] = radio::RatePreference{};                 /* no opinion at all */

    for (const auto& p : kProfiles) {
        if (p.rx_rate.max <= 0.0) continue; /* empty caps: no range to be in */
        for (const auto& want : requests) {
            const auto got = radio::choose_rx_rate(caps_with_rx_rate(p.rx_rate), want);
            CHECK(std::isfinite(got.rate_hz));
            CHECK(got.rate_hz > 0.0);

            const bool in_range =
                got.rate_hz >= p.rx_rate.min && got.rate_hz <= p.rx_rate.max;
            const std::string ok = std::string{p.name} + ": in range";
            CHECK_STR_EQ(in_range ? ok : labelled_hz(p.name, got.rate_hz), ok);
        }
    }
}

TEST(rate_policy_respects_max_useful_on_a_faster_radio) {
    /* A B200 will happily stream 16 Msps. ADS-B decodes no better for it and
     * the extra samples cost CPU and USB bandwidth, so the ceiling holds. */
    const auto b200 = caps_with_rx_rate({31.25e3, 16e6, 0.0});
    const auto got = radio::choose_rx_rate(b200, adsb_preference());
    CHECK_NEAR(got.rate_hz, 8e6, 1.0);
    CHECK(got.outcome == radio::RateOutcome::Ideal);
    CHECK(got.usable());

    /* Same device, an app that genuinely wants everything: no ceiling given,
     * so it gets the device maximum rather than a constant. */
    radio::RatePreference greedy;
    greedy.ideal_hz = 100e6;
    greedy.minimum_hz = 1e6;
    const auto wide = radio::choose_rx_rate(b200, greedy);
    CHECK_NEAR(wide.rate_hz, 16e6, 1.0);
    CHECK(wide.outcome == radio::RateOutcome::Reduced);
    CHECK(wide.usable());
}

TEST(rate_policy_separates_degraded_from_unusable) {
    /* Both fall short of the ideal. The caller has to be able to tell them
     * apart: one still decodes, the other cannot. */
    const auto dongle = radio::choose_rx_rate(
        caps_with_rx_rate({225001.0, 2.56e6, 0.0}), adsb_preference());
    CHECK(dongle.outcome == radio::RateOutcome::Reduced);
    CHECK(dongle.usable());
    CHECK(dongle.from_caps());

    const auto narrow = radio::choose_rx_rate(
        caps_with_rx_rate({48e3, 192e3, 0.0}), adsb_preference());
    CHECK(narrow.outcome == radio::RateOutcome::BelowMinimum);
    CHECK(!narrow.usable());
    CHECK(narrow.from_caps());

    /* A rate is still reported for the unusable case — the closest the device
     * can get — so a caller that wants a spectrum anyway has a number. It is
     * never fabricated above what the hardware published. */
    CHECK_NEAR(narrow.rate_hz, 192e3, 1.0);
}

TEST(rate_policy_raises_to_the_device_floor) {
    /* Direction is expressed by where the ideal sits, not by a flag. VOR wants
     * the LOWEST rate on offer (src/apps/ui_vor_rx.cpp) because a wider band
     * makes its bursts shorter, so it states a low ideal. */
    radio::RatePreference vorish;
    vorish.ideal_hz = 96e3;
    vorish.minimum_hz = 96e3;
    vorish.max_useful_hz = 96e3;

    const auto b200 = radio::choose_rx_rate(caps_with_rx_rate({31.25e3, 16e6, 0.0}), vorish);
    CHECK_NEAR(b200.rate_hz, 96e3, 1.0);
    CHECK(b200.outcome == radio::RateOutcome::Ideal);

    /* A HackRF cannot go below 2 Msps. The floor wins over max_useful_hz — a
     * physical limit is not a preference — and the caller is told. */
    const auto hackrf = radio::choose_rx_rate(caps_with_rx_rate({2e6, 20e6, 0.0}), vorish);
    CHECK_NEAR(hackrf.rate_hz, 2e6, 1.0);
    CHECK(hackrf.outcome == radio::RateOutcome::Raised);
    CHECK(hackrf.usable());
}

TEST(rate_policy_prefers_an_exact_multiple) {
    radio::RatePreference want;
    want.ideal_hz = 9e6;
    want.minimum_hz = 2e6;
    want.max_useful_hz = 20e6;
    want.prefer_multiple_of_hz = 2e6;

    /* 9 Msps is inside the HackRF's range, but 8 is an exact 4:1 ratio into the
     * demodulator's 2 Msps and costs less CPU, so it wins. Snapping DOWN, not
     * up: it can never break the max_useful ceiling. */
    const auto got = radio::choose_rx_rate(caps_with_rx_rate({2e6, 20e6, 0.0}), want);
    CHECK_NEAR(got.rate_hz, 8e6, 1.0);
    CHECK(got.outcome == radio::RateOutcome::Reduced);

    /* Without the preference the same request keeps its odd rate. */
    want.prefer_multiple_of_hz = 0.0;
    const auto unsnapped = radio::choose_rx_rate(caps_with_rx_rate({2e6, 20e6, 0.0}), want);
    CHECK_NEAR(unsnapped.rate_hz, 9e6, 1.0);
    CHECK(unsnapped.outcome == radio::RateOutcome::Ideal);
}

TEST(rate_policy_snaps_up_only_when_down_would_break_a_floor) {
    radio::RatePreference want;
    want.ideal_hz = 3e6;
    want.minimum_hz = 2e6;
    want.max_useful_hz = 8e6;
    want.prefer_multiple_of_hz = 2e6;

    /* Device floor 2.5 Msps: snapping down to 2 would be below what the radio
     * can do, so go up to 4 — still under the app's ceiling. */
    const auto got = radio::choose_rx_rate(caps_with_rx_rate({2.5e6, 20e6, 0.0}), want);
    CHECK_NEAR(got.rate_hz, 4e6, 1.0);
    CHECK(got.outcome == radio::RateOutcome::Raised);

    /* Same shape, but now the app's own minimum is what blocks the down-snap. */
    want.minimum_hz = 2.5e6;
    const auto by_app_floor =
        radio::choose_rx_rate(caps_with_rx_rate({1e6, 20e6, 0.0}), want);
    CHECK_NEAR(by_app_floor.rate_hz, 4e6, 1.0);

    /* And an up-snap that would break the CPU ceiling is not taken: the
     * preference is dropped rather than forced. */
    want.minimum_hz = 2.5e6;
    want.max_useful_hz = 3e6;
    const auto ceilinged =
        radio::choose_rx_rate(caps_with_rx_rate({2.5e6, 20e6, 0.0}), want);
    CHECK_NEAR(ceilinged.rate_hz, 3e6, 1.0);
    CHECK(ceilinged.outcome == radio::RateOutcome::Ideal);
}

TEST(rate_policy_drops_the_multiple_when_the_range_admits_none) {
    /* 3 - 3.5 Msps contains no multiple of 2 Msps at all. Forcing one would
     * leave the device range, so the clamped rate stands. */
    radio::RatePreference want = adsb_preference();
    const auto got = radio::choose_rx_rate(caps_with_rx_rate({3e6, 3.5e6, 0.0}), want);
    CHECK_NEAR(got.rate_hz, 3.5e6, 1.0);
    CHECK(got.outcome == radio::RateOutcome::Reduced);
    CHECK(got.usable());
}

TEST(rate_policy_honours_a_published_step_grid) {
    /* A device that publishes a coarse grid: 2 Msps + k * 2.5 Msps, so 7 Msps
     * is reachable and 8 is not. */
    const radio::Range grid{2e6, 20e6, 2.5e6};

    radio::RatePreference want;
    want.ideal_hz = 8e6;
    want.minimum_hz = 2e6;
    want.max_useful_hz = 8e6;

    const auto got = radio::choose_rx_rate(caps_with_rx_rate(grid), want);
    CHECK_NEAR(got.rate_hz, 7e6, 1.0);
    CHECK(got.outcome == radio::RateOutcome::Reduced);

    /* With a multiple preference on top, neither 6 nor 8 Msps is on the grid,
     * so the grid wins and the preference is dropped. The rate the device is
     * asked for stays one it published. */
    want.prefer_multiple_of_hz = 2e6;
    const auto snapped = radio::choose_rx_rate(caps_with_rx_rate(grid), want);
    CHECK_NEAR(snapped.rate_hz, 7e6, 1.0);

    /* The 1 Hz step in radio::default_b200_caps() must not perturb anything. */
    const auto fine = radio::choose_rx_rate(caps_with_rx_rate({200e3, 61.44e6, 1.0}),
                                            adsb_preference());
    CHECK_NEAR(fine.rate_hz, 8e6, 1.0);
    CHECK(fine.outcome == radio::RateOutcome::Ideal);
}

TEST(rate_policy_falls_back_to_the_ideal_when_caps_are_unusable) {
    /* An sdrlink server that omits rx_rate leaves the range all zeros
     * (network_radio.cpp range_from_json). Clamping to that would pin the app
     * at 0 Hz; instead the app's own ideal comes back, flagged. */
    const auto empty = radio::choose_rx_rate(radio::DeviceCaps{}, adsb_preference());
    CHECK_NEAR(empty.rate_hz, 8e6, 1.0);
    CHECK(empty.outcome == radio::RateOutcome::CapsUnusable);
    CHECK(!empty.from_caps());
    CHECK(empty.usable());

    /* Garbage ranges are treated the same way, not clamped against. */
    const radio::Range bad[] = {
        {0.0, 0.0, 0.0},
        {20e6, 2e6, 0.0},                                       /* inverted */
        {-1e6, 20e6, 0.0},                                      /* negative floor */
        {std::nan(""), 20e6, 0.0},
        {2e6, std::nan(""), 0.0},
        {2e6, INFINITY, 0.0},
    };
    for (const auto& r : bad) {
        const auto got = radio::choose_rx_rate(caps_with_rx_rate(r), adsb_preference());
        CHECK(std::isfinite(got.rate_hz));
        CHECK_NEAR(got.rate_hz, 8e6, 1.0);
        CHECK(got.outcome == radio::RateOutcome::CapsUnusable);
    }
}

TEST(rate_policy_survives_a_nonsense_preference) {
    const auto b200 = caps_with_rx_rate({31.25e3, 16e6, 0.0});

    /* Non-finite and negative fields mean "no opinion", never "zero hertz". */
    radio::RatePreference junk;
    junk.ideal_hz = std::nan("");
    junk.minimum_hz = 2e6;
    junk.max_useful_hz = -1.0;
    junk.prefer_multiple_of_hz = std::nan("");
    const auto got = radio::choose_rx_rate(b200, junk);
    CHECK(std::isfinite(got.rate_hz));
    CHECK_NEAR(got.rate_hz, 2e6, 1.0); /* only the floor was stated */
    CHECK(got.outcome == radio::RateOutcome::Ideal);

    /* Nothing stated at all: still a real rate the device can do, never 0. */
    const auto nothing = radio::choose_rx_rate(b200, radio::RatePreference{});
    CHECK(std::isfinite(nothing.rate_hz));
    CHECK(nothing.rate_hz > 0.0);
    CHECK_NEAR(nothing.rate_hz, 31.25e3, 1.0);

    /* No preference AND no published floor: the ceiling is the only real
     * number in play, and zero is not an answer. */
    const auto floorless =
        radio::choose_rx_rate(caps_with_rx_rate({0.0, 20e6, 0.0}), radio::RatePreference{});
    CHECK(floorless.rate_hz > 0.0);
    CHECK_NEAR(floorless.rate_hz, 20e6, 1.0);

    /* No preference and no caps: nothing to work from, and usable() says so
     * rather than a zero masquerading as a rate. */
    const auto blank = radio::choose_rx_rate(radio::DeviceCaps{}, radio::RatePreference{});
    CHECK(std::isfinite(blank.rate_hz));
    CHECK(!blank.usable());
    CHECK(blank.outcome == radio::RateOutcome::CapsUnusable);
}

TEST(rate_policy_minimum_beats_a_contradictory_ceiling) {
    /* "Cannot work below 4" and "gains nothing above 1" cannot both hold. The
     * functional limit wins, because obeying the CPU preference would leave the
     * app unable to work at all. */
    radio::RatePreference contradictory;
    contradictory.ideal_hz = 2e6;
    contradictory.minimum_hz = 4e6;
    contradictory.max_useful_hz = 1e6;

    const auto got =
        radio::choose_rx_rate(caps_with_rx_rate({31.25e3, 16e6, 0.0}), contradictory);
    CHECK_NEAR(got.rate_hz, 4e6, 1.0);
    CHECK(got.outcome == radio::RateOutcome::Ideal);
    CHECK(got.usable());
}

TEST(rate_policy_outcome_strings_are_distinct) {
    /* These land on a status line, so they have to be readable and different
     * from one another. */
    const radio::RateOutcome all[] = {
        radio::RateOutcome::Ideal,        radio::RateOutcome::Raised,
        radio::RateOutcome::Reduced,      radio::RateOutcome::BelowMinimum,
        radio::RateOutcome::CapsUnusable,
    };
    const size_t n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; ++i) {
        const std::string a = radio::rate_outcome_to_string(all[i]);
        CHECK(!a.empty());
        for (size_t j = i + 1; j < n; ++j)
            CHECK(a != radio::rate_outcome_to_string(all[j]));
    }

    radio::RateChoice choice;
    choice.outcome = radio::RateOutcome::Reduced;
    CHECK_STR_EQ(choice.text(), "below ideal");
}

TEST(rate_policy_hz_shortcut_matches_the_full_result) {
    for (const auto& p : kProfiles) {
        const auto caps = caps_with_rx_rate(p.rx_rate);
        const double shortcut = radio::choose_rx_rate_hz(caps, adsb_preference());
        const double full = radio::choose_rx_rate(caps, adsb_preference()).rate_hz;
        CHECK_STR_EQ(labelled_hz(p.name, shortcut), labelled_hz(p.name, full));
    }
}
