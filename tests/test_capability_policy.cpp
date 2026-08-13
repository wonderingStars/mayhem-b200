/*
 * mayhem-b200 — capability-driven analog bandwidth and gain tests.
 *
 * Same shape as tests/test_rate_policy.cpp, and for the same reason: both
 * helpers are pure functions from a DeviceCaps to a number, so they can be
 * tested against front ends nobody here owns, exhaustively, with no hardware
 * involved. Sections 1 and 2 do that. Section 3 then checks the wiring — that
 * ReceiverModel and TransmitterModel actually route their requests through the
 * policy — because a perfect policy nothing calls changes no behaviour at all.
 *
 * Analog filter figures used as profiles:
 *   B200        200 kHz - 56 MHz, from the Ettus UHD manual's "USRP B2x0
 *               Series" page and reproduced in radio::default_b200_caps()
 *   HackRF One  the MAX2837 baseband filter, 1.75 - 28 MHz
 *   AD936x      a Pluto-class device publishing the part's range
 *   narrow      a deliberately tiny front end, to force the Narrowed branch
 *   none        an sdrlink server that omitted rx_bandwidth entirely, which
 *               leaves the Range all zeros (network_radio.cpp range_from_json)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "audio_out.hpp"
#include "capability_policy.hpp"
#include "counter_radio.hpp"
#include "network_radio.hpp"
#include "receiver_model.hpp"
#include "transmitter_model.hpp"
#include "usrp_radio.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

radio::DeviceCaps caps_with_rx_bandwidth(radio::Range r) {
    radio::DeviceCaps c;
    c.rx_bandwidth = r;
    return c;
}

/* Failure output that says WHICH profile broke, which a bare CHECK_NEAR over a
 * table cannot. */
std::string labelled_hz(const char* profile, double hz) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s: %.0f Hz", profile, hz);
    return buf;
}

std::string labelled_bw_outcome(const char* profile, radio::BandwidthOutcome o) {
    return std::string{profile} + ": " + radio::bandwidth_outcome_to_string(o);
}

std::string labelled_db(const char* profile, double db) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s: %.3f dB", profile, db);
    return buf;
}

std::string labelled_gain_outcome(const char* profile, radio::GainOutcome o) {
    return std::string{profile} + ": " + radio::gain_outcome_to_string(o);
}

struct FrontEnd {
    const char* name;
    radio::Range rx_bandwidth;
};

const FrontEnd kFrontEnds[] = {
    {"B200",       {200e3, 56e6, 1.0}},
    {"HackRF One", {1.75e6, 28e6, 0.0}},
    {"AD936x",     {200e3, 56e6, 1.0}},
    {"narrow",     {48e3, 192e3, 0.0}},
    {"none",       {0.0, 0.0, 0.0}},
};

struct GainProfile {
    const char* name;
    radio::Range rx_gain;
};

const GainProfile kGainProfiles[] = {
    /* radio::default_b200_caps(), and what a connected B200 reports. */
    {"B200",     {0.0, 76.0, 1.0}},
    /* A HackRF's combined LNA+VGA span, as SoapyHackRF publishes it. */
    {"HackRF",   {0.0, 62.0, 1.0}},
    /* An R820T dongle: a discrete table whose ends are negative and coarse. */
    {"RTL-SDR",  {-9.9, 49.6, 0.0}},
    /* A server that published no gain range at all. */
    {"none",     {0.0, 0.0, 0.0}},
};

}  // namespace

/* =========================================================================
 * 1. Analog RX bandwidth
 * ========================================================================= */

TEST(bandwidth_policy_follows_the_rate_on_every_front_end) {
    struct Expect {
        const char* profile;
        double bandwidth_hz;
        radio::BandwidthOutcome outcome;
        bool apply;
    };

    /* One rate at a time across every front end. This is the whole point of the
     * helper in three tables: the same app gets a filter that fits, without any
     * of these numbers appearing in the app. */

    /* 8 Msps — what ADS-B chooses on anything modern. */
    const Expect at_8m[] = {
        {"B200", 8e6, radio::BandwidthOutcome::Matched, true},
        {"HackRF One", 8e6, radio::BandwidthOutcome::Matched, true},
        {"AD936x", 8e6, radio::BandwidthOutcome::Matched, true},
        /* A 192 kHz front end cannot open to 8 MHz: the captured band is rolled
         * off at the edges, and the caller is told so rather than assuming its
         * waterfall is flat. */
        {"narrow", 192e3, radio::BandwidthOutcome::Narrowed, true},
        /* Nothing published: do not touch the analog filter at all. */
        {"none", 0.0, radio::BandwidthOutcome::CapsUnusable, false},
    };

    /* 1 Msps — narrow enough that a HackRF's filter floor gets in the way. */
    const Expect at_1m[] = {
        {"B200", 1e6, radio::BandwidthOutcome::Matched, true},
        /* The MAX2837 will not close below 1.75 MHz, so 750 kHz of neighbours
         * fold in whatever we ask for. Widened says exactly that. */
        {"HackRF One", 1.75e6, radio::BandwidthOutcome::Widened, true},
        {"AD936x", 1e6, radio::BandwidthOutcome::Matched, true},
        {"narrow", 192e3, radio::BandwidthOutcome::Narrowed, true},
        {"none", 0.0, radio::BandwidthOutcome::CapsUnusable, false},
    };

    /* 61.44 Msps — UHD's published B200 ceiling, above its 56 MHz filter. */
    const Expect at_61m[] = {
        {"B200", 56e6, radio::BandwidthOutcome::Narrowed, true},
        {"HackRF One", 28e6, radio::BandwidthOutcome::Narrowed, true},
        {"AD936x", 56e6, radio::BandwidthOutcome::Narrowed, true},
        {"narrow", 192e3, radio::BandwidthOutcome::Narrowed, true},
        {"none", 0.0, radio::BandwidthOutcome::CapsUnusable, false},
    };

    const size_t n = sizeof(kFrontEnds) / sizeof(kFrontEnds[0]);
    static_assert(sizeof(at_8m) / sizeof(at_8m[0]) ==
                      sizeof(kFrontEnds) / sizeof(kFrontEnds[0]),
                  "one expectation per front end");
    static_assert(sizeof(at_1m) / sizeof(at_1m[0]) ==
                      sizeof(kFrontEnds) / sizeof(kFrontEnds[0]),
                  "one expectation per front end");
    static_assert(sizeof(at_61m) / sizeof(at_61m[0]) ==
                      sizeof(kFrontEnds) / sizeof(kFrontEnds[0]),
                  "one expectation per front end");

    struct Sweep {
        double rate_hz;
        const Expect* expected;
    };
    const Sweep sweeps[] = {{8e6, at_8m}, {1e6, at_1m}, {61.44e6, at_61m}};

    for (const auto& sweep : sweeps) {
        for (size_t i = 0; i < n; i++) {
            const FrontEnd& fe = kFrontEnds[i];
            const Expect& e = sweep.expected[i];
            CHECK_STR_EQ(fe.name, e.profile);

            const std::string tag =
                std::string{fe.name} + " @ " + std::to_string(static_cast<long long>(sweep.rate_hz));

            const auto got = radio::choose_rx_bandwidth(
                caps_with_rx_bandwidth(fe.rx_bandwidth), sweep.rate_hz);

            CHECK_STR_EQ(labelled_hz(tag.c_str(), got.bandwidth_hz),
                         labelled_hz(tag.c_str(), e.bandwidth_hz));
            CHECK_STR_EQ(labelled_bw_outcome(tag.c_str(), got.outcome),
                         labelled_bw_outcome(tag.c_str(), e.outcome));
            CHECK_STR_EQ(tag + (got.should_apply() ? ": apply" : ": skip"),
                         tag + (e.apply ? ": apply" : ": skip"));
        }
    }
}

TEST(bandwidth_policy_is_the_sample_rate_where_the_device_allows_it) {
    /* The relationship itself, stated once. Complex sampling at R covers R Hz
     * of spectrum, so the analog filter that matches it is R wide — narrower
     * rolls off inside the +/-0.4 * R window ReceiverModel tunes within, wider
     * folds the neighbours in. */
    CHECK_NEAR(radio::kRxAnalogBandwidthRatio, 1.0, 1e-12);

    const auto b200 = caps_with_rx_bandwidth({200e3, 56e6, 1.0});
    const double rates[] = {250e3, 1e6, 2e6, 2.4e6, 8e6, 16e6, 20e6, 56e6};
    for (double rate : rates) {
        const auto got = radio::choose_rx_bandwidth(b200, rate);
        CHECK(got.should_apply());
        CHECK(got.exact());
        CHECK_STR_EQ(labelled_hz("B200", got.bandwidth_hz),
                     labelled_hz("B200", rate * radio::kRxAnalogBandwidthRatio));
    }
}

TEST(bandwidth_policy_never_leaves_the_device_range) {
    /* The one hard guarantee: whatever rate arrives, a bandwidth that is
     * applied is one the device says its filter can reach. Crossed over
     * deliberately awkward rates. */
    const double rates[] = {1.0, 1e3, 250e3, 8e6, 61.44e6, 1e9, 1e12};

    for (const auto& fe : kFrontEnds) {
        for (double rate : rates) {
            const auto got =
                radio::choose_rx_bandwidth(caps_with_rx_bandwidth(fe.rx_bandwidth), rate);
            CHECK(std::isfinite(got.bandwidth_hz));
            CHECK(got.bandwidth_hz >= 0.0);

            if (!got.should_apply()) {
                /* The only reason to skip here is an unpublished filter. */
                CHECK(got.outcome == radio::BandwidthOutcome::CapsUnusable);
                continue;
            }

            const bool in_range = got.bandwidth_hz >= fe.rx_bandwidth.min &&
                                  got.bandwidth_hz <= fe.rx_bandwidth.max;
            const std::string ok = std::string{fe.name} + ": in range";
            CHECK_STR_EQ(in_range ? ok : labelled_hz(fe.name, got.bandwidth_hz), ok);
        }
    }
}

TEST(bandwidth_policy_skips_cleanly_when_the_caps_say_nothing_usable) {
    /* An sdrlink server that omits rx_bandwidth leaves the range all zeros. The
     * analog filter is then whatever the far side chose, and guessing at it
     * would be a value that can be rejected outright with nobody the wiser. */
    const auto empty = radio::choose_rx_bandwidth(radio::DeviceCaps{}, 8e6);
    CHECK(!empty.should_apply());
    CHECK(!empty.exact());
    CHECK(empty.outcome == radio::BandwidthOutcome::CapsUnusable);
    CHECK_NEAR(empty.bandwidth_hz, 0.0, 1e-9);

    /* Garbage ranges are treated the same way, not clamped against. Every one
     * of these is reachable: DeviceCaps is built from whatever JSON a remote
     * server sends. */
    const radio::Range bad[] = {
        {0.0, 0.0, 0.0},
        {56e6, 200e3, 0.0},                  /* inverted */
        {-1e6, 56e6, 0.0},                   /* negative floor */
        {200e3, -1.0, 0.0},                  /* negative ceiling */
        {std::nan(""), 56e6, 0.0},
        {200e3, std::nan(""), 0.0},
        {200e3, INFINITY, 0.0},
        {-INFINITY, INFINITY, 0.0},
    };
    for (const auto& r : bad) {
        const auto got = radio::choose_rx_bandwidth(caps_with_rx_bandwidth(r), 8e6);
        CHECK(std::isfinite(got.bandwidth_hz));
        CHECK(!got.should_apply());
        CHECK(got.outcome == radio::BandwidthOutcome::CapsUnusable);
    }
}

TEST(bandwidth_policy_refuses_a_rate_that_is_not_a_rate) {
    /* ReceiverModel reads its rate back FROM the radio, so this is whatever the
     * hardware returned — including 0.0 from a device that is not open. There
     * is no filter width to derive from that, and inventing one would put a
     * fabricated number on the wire. */
    const auto b200 = caps_with_rx_bandwidth({200e3, 56e6, 1.0});
    const double nonsense[] = {0.0, -1.0, -8e6, std::nan(""), INFINITY, -INFINITY};
    for (double rate : nonsense) {
        const auto got = radio::choose_rx_bandwidth(b200, rate);
        CHECK(std::isfinite(got.bandwidth_hz));
        CHECK(!got.should_apply());
        CHECK(got.outcome == radio::BandwidthOutcome::NoRate);
    }

    /* NoRate is not CapsUnusable: one says the device published nothing, the
     * other says the caller asked nothing. A status line has to distinguish
     * them or the operator debugs the wrong end. */
    CHECK(radio::choose_rx_bandwidth(radio::DeviceCaps{}, 8e6).outcome ==
          radio::BandwidthOutcome::CapsUnusable);
}

TEST(bandwidth_policy_clamps_at_both_ends) {
    /* A filter that will not close far enough, and one that will not open far
     * enough, on the same device shape. */
    const auto hackrf = caps_with_rx_bandwidth({1.75e6, 28e6, 0.0});

    const auto too_narrow = radio::choose_rx_bandwidth(hackrf, 500e3);
    CHECK_NEAR(too_narrow.bandwidth_hz, 1.75e6, 1.0);
    CHECK(too_narrow.outcome == radio::BandwidthOutcome::Widened);
    CHECK(too_narrow.should_apply());
    CHECK(!too_narrow.exact());

    const auto too_wide = radio::choose_rx_bandwidth(hackrf, 40e6);
    CHECK_NEAR(too_wide.bandwidth_hz, 28e6, 1.0);
    CHECK(too_wide.outcome == radio::BandwidthOutcome::Narrowed);
    CHECK(too_wide.should_apply());
    CHECK(!too_wide.exact());

    /* Exactly on each bound is Matched, not a clamp: a device asked for a
     * bandwidth it published is a device that got what it was asked for. */
    CHECK(radio::choose_rx_bandwidth(hackrf, 1.75e6).outcome ==
          radio::BandwidthOutcome::Matched);
    CHECK(radio::choose_rx_bandwidth(hackrf, 28e6).outcome ==
          radio::BandwidthOutcome::Matched);
}

TEST(bandwidth_policy_outcome_strings_are_distinct) {
    /* These land on a status line, so they have to be readable and different
     * from one another. */
    const radio::BandwidthOutcome all[] = {
        radio::BandwidthOutcome::Matched,  radio::BandwidthOutcome::Widened,
        radio::BandwidthOutcome::Narrowed, radio::BandwidthOutcome::NoRate,
        radio::BandwidthOutcome::CapsUnusable,
    };
    const size_t n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; i++) {
        const std::string a = radio::bandwidth_outcome_to_string(all[i]);
        CHECK(!a.empty());
        for (size_t j = i + 1; j < n; j++)
            CHECK(a != radio::bandwidth_outcome_to_string(all[j]));
    }

    radio::BandwidthChoice choice;
    choice.outcome = radio::BandwidthOutcome::Narrowed;
    CHECK_STR_EQ(choice.text(), "narrower than rate");
}

/* =========================================================================
 * 2. Gain
 * ========================================================================= */

TEST(gain_policy_clamps_into_every_published_range) {
    struct Expect {
        const char* profile;
        double gain_db;
        radio::GainOutcome outcome;
    };

    /* A 40 dB request — main.cpp's startup gain and Settings::rx_gain's
     * default — across devices whose ranges disagree about what 40 means. */
    const Expect at_40[] = {
        {"B200", 40.0, radio::GainOutcome::Applied},
        {"HackRF", 40.0, radio::GainOutcome::Applied},
        {"RTL-SDR", 40.0, radio::GainOutcome::Applied},
        /* No range published: the request goes through untouched, because a
         * radio left at whatever gain it happened to have is deaf or
         * saturated, and every backend clamps against its own caps anyway. */
        {"none", 40.0, radio::GainOutcome::Unvalidated},
    };

    /* Above every real device's ceiling. */
    const Expect at_120[] = {
        {"B200", 76.0, radio::GainOutcome::ClampedHigh},
        {"HackRF", 62.0, radio::GainOutcome::ClampedHigh},
        {"RTL-SDR", 49.6, radio::GainOutcome::ClampedHigh},
        {"none", 120.0, radio::GainOutcome::Unvalidated},
    };

    /* Below it. Note the RTL-SDR floor is NEGATIVE — a gain range is not a
     * frequency range and must not be validated as if it were. */
    const Expect at_minus_50[] = {
        {"B200", 0.0, radio::GainOutcome::ClampedLow},
        {"HackRF", 0.0, radio::GainOutcome::ClampedLow},
        {"RTL-SDR", -9.9, radio::GainOutcome::ClampedLow},
        {"none", -50.0, radio::GainOutcome::Unvalidated},
    };

    const size_t n = sizeof(kGainProfiles) / sizeof(kGainProfiles[0]);
    static_assert(sizeof(at_40) / sizeof(at_40[0]) ==
                      sizeof(kGainProfiles) / sizeof(kGainProfiles[0]),
                  "one expectation per gain profile");

    struct Sweep {
        double request_db;
        const Expect* expected;
    };
    const Sweep sweeps[] = {{40.0, at_40}, {120.0, at_120}, {-50.0, at_minus_50}};

    for (const auto& sweep : sweeps) {
        for (size_t i = 0; i < n; i++) {
            const GainProfile& p = kGainProfiles[i];
            const Expect& e = sweep.expected[i];
            CHECK_STR_EQ(p.name, e.profile);

            const std::string tag =
                std::string{p.name} + " asked " + std::to_string(sweep.request_db);

            const auto got = radio::choose_gain(p.rx_gain, sweep.request_db);
            CHECK_STR_EQ(labelled_db(tag.c_str(), got.gain_db),
                         labelled_db(tag.c_str(), e.gain_db));
            CHECK_STR_EQ(labelled_gain_outcome(tag.c_str(), got.outcome),
                         labelled_gain_outcome(tag.c_str(), e.outcome));
            CHECK(got.should_apply());
        }
    }
}

TEST(gain_policy_refuses_a_request_that_is_not_a_number) {
    const radio::Range b200{0.0, 76.0, 1.0};
    const double nonsense[] = {std::nan(""), INFINITY, -INFINITY};

    for (double db : nonsense) {
        const auto got = radio::choose_gain(b200, db);
        CHECK(!got.should_apply());
        CHECK(!got.from_caps());
        CHECK(got.outcome == radio::GainOutcome::Invalid);
        CHECK(std::isfinite(got.gain_db));
    }

    /* Refused with an unusable range too — the request is what is wrong here,
     * not the caps, so the "pass it through" rule must not rescue it. */
    const auto blank = radio::choose_gain(radio::Range{}, std::nan(""));
    CHECK(!blank.should_apply());
    CHECK(blank.outcome == radio::GainOutcome::Invalid);
}

TEST(gain_policy_nan_would_otherwise_become_invalid_json) {
    /* Why Invalid refuses rather than substituting a number. NetworkRadio
     * clamps against caps only while its link is DOWN; with the link up it
     * formats the request straight into the control message, and
     * format_json_number falls through to "%.17g" for anything not finite.
     * This is the resulting message, parsed by this project's own parser. */
    const std::string encoded = radio::net::format_json_number(std::nan(""));
    const std::string message = "{\"db\":" + encoded + "}";

    radio::net::JsonValue parsed;
    std::string error;
    CHECK(!radio::net::json_parse(message, parsed, error));
    CHECK(!error.empty());

    /* And the same field with a clamped gain in it parses fine, so the policy's
     * output is a message the far side can act on. */
    const auto ok = radio::choose_gain({0.0, 76.0, 1.0}, std::nan(""));
    CHECK(!ok.should_apply()); /* nothing is sent at all, which is the fix */

    const auto clamped = radio::choose_gain({0.0, 76.0, 1.0}, 1e9);
    const std::string good = "{\"db\":" + radio::net::format_json_number(clamped.gain_db) + "}";
    CHECK(radio::net::json_parse(good, parsed, error));
}

TEST(gain_policy_ignores_a_garbage_range_rather_than_clamping_to_it) {
    /* DeviceCaps.rx_gain comes from remote JSON like everything else. A range
     * that is inverted or not a number must not be allowed to pin the gain. */
    const radio::Range bad[] = {
        {0.0, 0.0, 0.0},                     /* unpublished */
        {76.0, 0.0, 0.0},                    /* inverted */
        {40.0, 40.0, 0.0},                   /* degenerate: says nothing */
        {std::nan(""), 76.0, 0.0},
        {0.0, std::nan(""), 0.0},
        {0.0, INFINITY, 0.0},
        {-INFINITY, 76.0, 0.0},
    };
    for (const auto& r : bad) {
        const auto got = radio::choose_gain(r, 40.0);
        CHECK(got.should_apply());
        CHECK(!got.from_caps());
        CHECK(got.outcome == radio::GainOutcome::Unvalidated);
        CHECK_NEAR(got.gain_db, 40.0, 1e-9);
    }
}

TEST(gain_policy_reports_which_end_the_request_hit) {
    /* A UI showing "76 dB" when the user asked for 90 is lying by omission;
     * clamped() and text() are what let it say "76 dB (device maximum)". */
    const radio::Range b200{0.0, 76.0, 1.0};

    const auto high = radio::choose_gain(b200, 90.0);
    CHECK(high.clamped());
    CHECK(high.from_caps());
    CHECK_STR_EQ(high.text(), "at device maximum");

    const auto low = radio::choose_gain(b200, -5.0);
    CHECK(low.clamped());
    CHECK(low.from_caps());
    CHECK_STR_EQ(low.text(), "at device minimum");

    const auto mid = radio::choose_gain(b200, 30.0);
    CHECK(!mid.clamped());
    CHECK(mid.from_caps());
    CHECK_STR_EQ(mid.text(), "applied");

    /* The bounds themselves are inside the range, not clamps. */
    CHECK(radio::choose_gain(b200, 0.0).outcome == radio::GainOutcome::Applied);
    CHECK(radio::choose_gain(b200, 76.0).outcome == radio::GainOutcome::Applied);
}

TEST(gain_policy_reads_the_right_range_for_each_direction) {
    /* A B200's TX gain goes half as high again as its RX gain, so a helper that
     * quietly read rx_gain for both would pass most tests and clamp every
     * transmit request 13.8 dB low. */
    radio::DeviceCaps caps;
    caps.rx_gain = {0.0, 76.0, 1.0};
    caps.tx_gain = {0.0, 89.8, 0.2};

    CHECK_NEAR(radio::choose_rx_gain(caps, 200.0).gain_db, 76.0, 1e-9);
    CHECK_NEAR(radio::choose_tx_gain(caps, 200.0).gain_db, 89.8, 1e-9);
}

TEST(gain_policy_outcome_strings_are_distinct) {
    const radio::GainOutcome all[] = {
        radio::GainOutcome::Applied,     radio::GainOutcome::ClampedLow,
        radio::GainOutcome::ClampedHigh, radio::GainOutcome::Unvalidated,
        radio::GainOutcome::Invalid,
    };
    const size_t n = sizeof(all) / sizeof(all[0]);
    for (size_t i = 0; i < n; i++) {
        const std::string a = radio::gain_outcome_to_string(all[i]);
        CHECK(!a.empty());
        for (size_t j = i + 1; j < n; j++)
            CHECK(a != radio::gain_outcome_to_string(all[j]));
    }
}

/* =========================================================================
 * 3. Wiring
 *
 * CounterRadio is the right fake for this precisely because its setters do NOT
 * clamp: whatever reaches set_rx_gain() is what the shared layer decided, with
 * no backend cleaning up afterwards. That is also the real NetworkRadio case,
 * which clamps against caps only while its link is down.
 * ========================================================================= */

namespace {

struct RxHarness {
    mb200test::CounterRadio dev{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{dev, audio};

    RxHarness() = default;
    RxHarness(const RxHarness&) = delete;
    RxHarness& operator=(const RxHarness&) = delete;
};

}  // namespace

TEST(receiver_points_the_analog_filter_at_the_sample_rate) {
    RxHarness h;
    h.dev.mutable_caps().rx_bandwidth = {200e3, 56e6, 1.0};

    h.receiver.set_sampling_rate(4e6);
    CHECK_NEAR(h.dev.rx_bandwidth(), 4e6, 1.0);
    CHECK(h.receiver.rx_bandwidth_choice().should_apply());
    CHECK(h.receiver.rx_bandwidth_choice().exact());

    /* And it MOVES with the rate. A filter set once at startup and left there
     * is the bug this replaces, one rate change later. */
    h.receiver.set_sampling_rate(16e6);
    CHECK_NEAR(h.dev.rx_bandwidth(), 16e6, 1.0);

    h.receiver.set_sampling_rate(500e3);
    CHECK_NEAR(h.dev.rx_bandwidth(), 500e3, 1.0);
}

TEST(receiver_applies_the_analog_filter_when_it_starts) {
    RxHarness h;
    h.dev.mutable_caps().rx_bandwidth = {200e3, 56e6, 1.0};

    h.receiver.set_sampling_rate(8e6);

    /* Something else moved the filter behind the receiver's back — a previous
     * app, or a reconnect that reset the device. start() must put it back,
     * because that is the moment the rate is pushed to the radio. */
    h.dev.set_rx_bandwidth(1.0e6);
    CHECK_NEAR(h.dev.rx_bandwidth(), 1.0e6, 1.0);

    CHECK(h.receiver.start());
    CHECK_NEAR(h.dev.rx_bandwidth(), 8e6, 1.0);
    h.receiver.stop();
}

TEST(receiver_leaves_an_unpublished_analog_filter_alone) {
    RxHarness h; /* CounterRadio publishes no rx_bandwidth range */
    CHECK_NEAR(h.dev.rx_bandwidth(), 1.6e6, 1.0);

    h.receiver.set_sampling_rate(8e6);

    /* Untouched, and the receiver can say why rather than the operator
     * wondering whether the write silently failed. */
    CHECK_NEAR(h.dev.rx_bandwidth(), 1.6e6, 1.0);
    CHECK(!h.receiver.rx_bandwidth_choice().should_apply());
    CHECK(h.receiver.rx_bandwidth_choice().outcome ==
          radio::BandwidthOutcome::CapsUnusable);
}

TEST(receiver_narrows_the_filter_to_what_the_front_end_can_reach) {
    RxHarness h;
    h.dev.mutable_caps().rx_bandwidth = {1.75e6, 28e6, 0.0}; /* HackRF-shaped */

    h.receiver.set_sampling_rate(40e6);
    CHECK_NEAR(h.dev.rx_bandwidth(), 28e6, 1.0);
    CHECK(h.receiver.rx_bandwidth_choice().outcome == radio::BandwidthOutcome::Narrowed);

    h.receiver.set_sampling_rate(500e3);
    CHECK_NEAR(h.dev.rx_bandwidth(), 1.75e6, 1.0);
    CHECK(h.receiver.rx_bandwidth_choice().outcome == radio::BandwidthOutcome::Widened);
}

TEST(receiver_clamps_a_gain_request_before_the_radio_sees_it) {
    RxHarness h; /* CounterRadio publishes rx_gain 0 - 76 dB and clamps nothing */

    h.receiver.set_gain(40.0);
    CHECK_NEAR(h.dev.rx_gain(), 40.0, 1e-9);
    CHECK(h.receiver.gain_choice().outcome == radio::GainOutcome::Applied);

    h.receiver.set_gain(500.0);
    CHECK_NEAR(h.dev.rx_gain(), 76.0, 1e-9);
    CHECK(h.receiver.gain_choice().outcome == radio::GainOutcome::ClampedHigh);

    h.receiver.set_gain(-30.0);
    CHECK_NEAR(h.dev.rx_gain(), 0.0, 1e-9);
    CHECK(h.receiver.gain_choice().outcome == radio::GainOutcome::ClampedLow);
}

TEST(receiver_does_not_touch_the_radio_for_a_gain_that_is_not_a_number) {
    RxHarness h;

    h.receiver.set_gain(40.0);
    CHECK_NEAR(h.dev.rx_gain(), 40.0, 1e-9);

    h.receiver.set_gain(std::nan(""));
    CHECK_NEAR(h.dev.rx_gain(), 40.0, 1e-9); /* unchanged, not NaN, not 0 */
    CHECK(!h.receiver.gain_choice().should_apply());
    CHECK(h.receiver.gain_choice().outcome == radio::GainOutcome::Invalid);

    h.receiver.set_gain(INFINITY);
    CHECK_NEAR(h.dev.rx_gain(), 40.0, 1e-9);
}

TEST(transmitter_clamps_a_gain_request_before_the_radio_sees_it) {
    mb200test::CounterRadio dev;
    dev.mutable_caps().tx_gain = {0.0, 89.8, 0.2};
    radio::TransmitterModel tx{dev};

    tx.set_gain(50.0);
    CHECK_NEAR(dev.tx_gain(), 50.0, 1e-9);
    CHECK(tx.gain_choice().outcome == radio::GainOutcome::Applied);

    tx.set_gain(500.0);
    CHECK_NEAR(dev.tx_gain(), 89.8, 1e-9);
    CHECK(tx.gain_choice().outcome == radio::GainOutcome::ClampedHigh);

    tx.set_gain(-10.0);
    CHECK_NEAR(dev.tx_gain(), 0.0, 1e-9);
    CHECK(tx.gain_choice().outcome == radio::GainOutcome::ClampedLow);

    /* Not a number: the radio keeps the gain it had. On a transmit chain this
     * is the difference between a rejected control message and a live PA at an
     * unknown setting. */
    tx.set_gain(60.0);
    tx.set_gain(std::nan(""));
    CHECK_NEAR(dev.tx_gain(), 60.0, 1e-9);
    CHECK(!tx.gain_choice().should_apply());
}

TEST(transmitter_passes_a_gain_through_when_no_range_was_published) {
    /* CounterRadio publishes no tx_gain range. The documented decision is that
     * gain still gets written — unlike bandwidth — because leaving a radio at
     * an unknown gain is worse than sending a value the backend will clamp. */
    mb200test::CounterRadio dev;
    radio::TransmitterModel tx{dev};

    tx.set_gain(200.0);
    CHECK_NEAR(dev.tx_gain(), 200.0, 1e-9);
    CHECK(tx.gain_choice().outcome == radio::GainOutcome::Unvalidated);
    CHECK(tx.gain_choice().should_apply());
    CHECK(!tx.gain_choice().from_caps());
}

TEST(receiver_on_a_published_b200_chooses_the_b200_filter) {
    /* End to end against the caps a real backend reports. A closed UsrpRadio
     * still publishes radio::default_b200_caps(), so this is the filter width a
     * connected B200 would be asked for at each rate. */
    radio::UsrpRadio dev{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{dev, audio};

    receiver.set_sampling_rate(8e6);
    CHECK_NEAR(dev.rx_bandwidth(), 8e6, 1.0);
    CHECK(receiver.rx_bandwidth_choice().exact());

    /* At UHD's published 61.44 Msps ceiling the analog filter runs out first:
     * 56 MHz is all there is, and the receiver is told the band it is capturing
     * is wider than the filter in front of it. */
    receiver.set_sampling_rate(61.44e6);
    CHECK_NEAR(dev.rx_bandwidth(), 56e6, 1.0);
    CHECK(receiver.rx_bandwidth_choice().outcome == radio::BandwidthOutcome::Narrowed);

    /* And the gain range is the published one, clamped in the shared layer. */
    receiver.set_gain(500.0);
    CHECK_NEAR(dev.rx_gain(), 76.0, 1e-9);
}
