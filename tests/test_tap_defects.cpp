/*
 * mayhem-b200 — regression tests for defects found auditing the gapless tap
 * and the rate policy.
 *
 * Each test here reproduces a specific defect that shipped in the first cut of
 * RawSampleTap / choose_rx_rate(). They are written as reproductions first —
 * every one of them was seen to FAIL against the unfixed code, with the failure
 * text recorded in the change that introduced them — so that a regression is
 * caught by a test that has demonstrably been red at least once.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "audio_out.hpp"
#include "rate_policy.hpp"
#include "receiver_model.hpp"
#include "usrp_radio.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

namespace {

dsp::cfloat sample_at(uint64_t i) {
    return dsp::cfloat{static_cast<float>(i), -static_cast<float>(i)};
}

void fill(std::vector<dsp::cfloat>& block, uint64_t base) {
    for (size_t k = 0; k < block.size(); k++) block[k] = sample_at(base + k);
}

/* A device profile with an explicitly empty step grid, which is what every
 * radio in this tree publishes for rx_rate today. */
radio::DeviceCaps caps_with_rate(double min_hz, double max_hz, double step_hz = 0.0) {
    radio::DeviceCaps caps;
    caps.rx_rate.min = min_hz;
    caps.rx_rate.max = max_hz;
    caps.rx_rate.step = step_hz;
    return caps;
}

radio::RatePreference adsb_like_preference() {
    radio::RatePreference want;
    want.minimum_hz = 2'000'000.0;
    want.ideal_hz = 8'000'000.0;
    want.max_useful_hz = 8'000'000.0;
    want.prefer_multiple_of_hz = 2'000'000.0;
    return want;
}

}  // namespace

/* --- Defect 1: a closed tap must not charge its leftovers to the next
 *               subscriber ------------------------------------------------- */

TEST(tap_close_does_not_leave_a_gap_for_the_next_subscription) {
    /* close() correctly accounts unread samples as dropped, because nobody will
     * ever receive them. What it must NOT do is leave that hole queued in the
     * pending-gap slot, because the slot is read by the NEXT open()'s first
     * read() — a different subscription, on a different stream, which lost
     * nothing. Reproduced as: session A buffers and closes without draining,
     * session B opens and reads, and is told about A's hole.
     *
     * The consequence is not cosmetic. In ui_adsb_rx.cpp a non-zero
     * lost_before also forces demod_.reset(), so the first pump of every
     * relaunch would throw away decoder state for a hole that did not happen
     * to it, and the on-screen "lost N in M" and air_fraction() would open at
     * a lie whose size is a whole ring. */
    radio::RawSampleTap tap;

    CHECK(tap.open(4096));
    std::vector<dsp::cfloat> block(1000);
    fill(block, 0);
    tap.write(block.data(), block.size());
    CHECK_EQ(tap.available(), size_t{1000});

    tap.close();
    /* Still counted as loss — those samples really did go nowhere. */
    CHECK_EQ(tap.dropped(), uint64_t{1000});

    /* A new subscription. */
    tap.reset_stats();
    CHECK(tap.open(4096));

    std::vector<dsp::cfloat> fresh(64);
    fill(fresh, 0);
    tap.write(fresh.data(), fresh.size());

    std::vector<dsp::cfloat> out;
    const auto got = tap.read(out);
    CHECK_EQ(got.samples, size_t{64});
    CHECK_EQ(got.lost_before, uint64_t{0});
    CHECK(got.contiguous());
    CHECK_EQ(tap.dropped(), uint64_t{0});
}

TEST(tap_read_on_a_closed_tap_reports_no_phantom_gap) {
    /* Same slot, reached without a reopen: read() on a closed tap must not hand
     * back a hole either. It has no samples to place it against. */
    radio::RawSampleTap tap;
    CHECK(tap.open(2048));

    std::vector<dsp::cfloat> block(700);
    fill(block, 0);
    tap.write(block.data(), block.size());
    tap.close();

    std::vector<dsp::cfloat> out;
    const auto got = tap.read(out);
    CHECK_EQ(got.samples, size_t{0});
    CHECK_EQ(got.lost_before, uint64_t{0});
}

TEST(tap_restart_still_announces_its_hole_to_the_same_subscriber) {
    /* The complement, and the reason the fix belongs in close() rather than in
     * open(): restart() re-sizes a tap that a consumer is still subscribed to,
     * so the discontinuity is genuinely theirs and must still be reported.
     * A fix that cleared the pending gap on every open() would silently delete
     * this, since restart() is implemented on top of open(). */
    radio::RawSampleTap tap;
    CHECK(tap.open(1024));

    std::vector<dsp::cfloat> block(400);
    fill(block, 0);
    tap.write(block.data(), block.size());

    tap.restart(2048);
    CHECK(tap.is_open());
    CHECK_EQ(tap.capacity(), size_t{2048});

    fill(block, 400);
    tap.write(block.data(), block.size());

    std::vector<dsp::cfloat> out;
    const auto got = tap.read(out);
    CHECK_EQ(got.samples, size_t{400});
    CHECK_EQ(got.lost_before, uint64_t{400});
}

TEST(adsb_relaunch_does_not_inherit_the_previous_run_s_loss) {
    /* The defect as the app actually reaches it: ~AdsbRxView calls
     * disable_raw_tap() and a later on_show() calls enable_raw_tap(), on the
     * one process-lifetime ReceiverModel. Driven through ReceiverModel rather
     * than RawSampleTap so the wiring is covered too. */
    radio::UsrpRadio r;
    audio::AudioOut a;
    radio::ReceiverModel rx{r, a};

    CHECK(rx.enable_raw_tap(0.25));
    CHECK(rx.raw_tap_enabled());

    std::vector<dsp::cfloat> block(5000);
    fill(block, 0);
    rx.raw_tap().write(block.data(), block.size());

    rx.disable_raw_tap();
    CHECK(!rx.raw_tap_enabled());

    CHECK(rx.enable_raw_tap(0.25));
    std::vector<dsp::cfloat> out;
    const auto first = rx.raw_tap().read(out);
    CHECK_EQ(first.samples, size_t{0});
    CHECK_EQ(first.lost_before, uint64_t{0});
    CHECK(first.contiguous());
}

/* --- Defect 2: available() underflowed when read off the consumer thread --- */

TEST(tap_available_never_underflows_when_polled_from_another_thread) {
    /* write_ and read_ are two atomics loaded one after the other, so an
     * observer that is neither the producer nor the consumer can hold a stale
     * write_ next to a fresher read_. The subtraction then wraps and reports
     * about 1.8e19 samples buffered in a 4096-slot ring.
     *
     * Before the fix this reproduced within a few hundred thousand polls, with
     * a worst observed value of 2^64 - 64 — exactly one producer block of
     * underflow. The test is a race, so it is written to be conclusive when it
     * catches the bug and merely silent when it does not: any single impossible
     * reading fails it. */
    radio::RawSampleTap tap;
    const size_t capacity = 4096;
    CHECK(tap.open(capacity));

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> polls{0};
    std::atomic<uint64_t> impossible{0};
    std::atomic<uint64_t> worst{0};

    std::thread producer([&] {
        std::vector<dsp::cfloat> block(64);
        uint64_t sent = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            fill(block, sent);
            tap.write(block.data(), block.size());
            sent += block.size();
        }
    });

    std::thread consumer([&] {
        std::vector<dsp::cfloat> out;
        while (!stop.load(std::memory_order_relaxed)) tap.read(out);
    });

    std::thread observer([&] {
        uint64_t local_worst = 0;
        uint64_t local_bad = 0;
        uint64_t local_polls = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            const size_t n = tap.available();
            local_polls++;
            if (n > capacity) {
                local_bad++;
                if (static_cast<uint64_t>(n) > local_worst)
                    local_worst = static_cast<uint64_t>(n);
            }
        }
        polls.store(local_polls);
        impossible.store(local_bad);
        worst.store(local_worst);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop.store(true);
    producer.join();
    consumer.join();
    observer.join();

    std::printf("      available(): %llu polls, %llu impossible, worst %llu (capacity %zu)\n",
                static_cast<unsigned long long>(polls.load()),
                static_cast<unsigned long long>(impossible.load()),
                static_cast<unsigned long long>(worst.load()), capacity);

    CHECK_EQ(impossible.load(), uint64_t{0});
    CHECK(polls.load() > 0);
}

TEST(tap_available_stays_exact_on_the_consumer_thread) {
    /* The fix clamps to capacity, so it must not blunt the reading a
     * single-threaded consumer gets — which is the one the accounting invariant
     * offered() == delivered() + dropped() + available() is checked against. */
    radio::RawSampleTap tap;
    CHECK(tap.open(1024));
    CHECK_EQ(tap.available(), size_t{0});

    std::vector<dsp::cfloat> block(300);
    fill(block, 0);
    tap.write(block.data(), block.size());
    CHECK_EQ(tap.available(), size_t{300});

    std::vector<dsp::cfloat> out;
    tap.read(out);
    CHECK_EQ(tap.available(), size_t{0});

    /* Exactly full, which is where a clamp could wrongly bite. */
    std::vector<dsp::cfloat> big(1024);
    fill(big, 300);
    tap.write(big.data(), big.size());
    CHECK_EQ(tap.available(), size_t{1024});
    CHECK_EQ(tap.offered(), uint64_t{1324});
    CHECK_EQ(tap.dropped(), uint64_t{0});

    /* And an overflow: the ring holds its capacity, no more, and the surplus is
     * dropped rather than counted as buffered. */
    tap.write(big.data(), big.size());
    CHECK_EQ(tap.available(), size_t{1024});
    CHECK_EQ(tap.dropped(), uint64_t{1024});

    tap.close();
    CHECK_EQ(tap.available(), size_t{0});
}

/* --- Defect 3: choose_rx_rate could return inf, -0, or an out-of-range rate - */

TEST(rate_policy_never_returns_a_rate_outside_the_device_range) {
    /* floor_multiple/ceil_multiple divide by prefer_multiple_of_hz. A denormal
     * multiple sends the quotient to infinity and a colossal one sends it to
     * zero, and neither branch checked the result before adopting it. The
     * `up` branch additionally never checked the device floor.
     *
     * Reachable from untrusted input: network_radio.cpp builds DeviceCaps from
     * whatever JSON an sdrlink server sends, so a nonsense rx_rate range is a
     * remote input, not just a hypothetical. */
    struct Case {
        const char* name;
        radio::DeviceCaps caps;
        double multiple;
    };

    const double denormal = std::numeric_limits<double>::denorm_min();

    const Case cases[] = {
        {"denormal device max", caps_with_rate(0.0, denormal), 2'000'000.0},
        {"denormal multiple", caps_with_rate(200'000.0, 61'440'000.0), denormal},
        {"astronomic multiple", caps_with_rate(200'000.0, 61'440'000.0), 1e300},
        {"huge device max", caps_with_rate(200'000.0, 1e308), 2'000'000.0},
        {"tiny multiple", caps_with_rate(200'000.0, 61'440'000.0), 1e-300},
    };

    for (const auto& c : cases) {
        auto want = adsb_like_preference();
        want.prefer_multiple_of_hz = c.multiple;

        const auto got = radio::choose_rx_rate(c.caps, want);

        const bool finite = std::isfinite(got.rate_hz);
        const bool positive = got.rate_hz > 0.0;
        const bool in_range =
            got.rate_hz >= c.caps.rx_rate.min && got.rate_hz <= c.caps.rx_rate.max;

        std::printf("      %-20s -> %-14g %-14s finite=%d positive=%d in_range=%d\n",
                    c.name, got.rate_hz, got.text(), finite ? 1 : 0,
                    positive ? 1 : 0, in_range ? 1 : 0);

        CHECK(finite);
        CHECK(positive);
        CHECK(in_range);
        CHECK(!std::signbit(got.rate_hz));
    }
}

TEST(rate_policy_real_device_profiles_are_unchanged) {
    /* The guard must be invisible to every device that actually exists, or the
     * fix has traded one defect for another. These are the same four profiles
     * tests/test_rate_policy.cpp and tests/test_adsb_tap.cpp pin. */
    const auto want = adsb_like_preference();

    struct Profile {
        const char* name;
        radio::DeviceCaps caps;
        double expect_hz;
        radio::RateOutcome expect_outcome;
    };

    const Profile profiles[] = {
        {"B200", caps_with_rate(200'000.0, 61'440'000.0), 8'000'000.0,
         radio::RateOutcome::Ideal},
        {"B200 on USB 2", caps_with_rate(200'000.0, 16'000'000.0), 8'000'000.0,
         radio::RateOutcome::Ideal},
        {"RTL-SDR", caps_with_rate(225'001.0, 2'400'000.0), 2'000'000.0,
         radio::RateOutcome::Reduced},
        {"HackRF", caps_with_rate(2'000'000.0, 20'000'000.0), 8'000'000.0,
         radio::RateOutcome::Ideal},
        {"sub-Mode-S device", caps_with_rate(100'000.0, 900'000.0), 900'000.0,
         radio::RateOutcome::BelowMinimum},
    };

    for (const auto& p : profiles) {
        const auto got = radio::choose_rx_rate(p.caps, want);
        std::printf("      %-18s -> %-12g %s\n", p.name, got.rate_hz, got.text());
        CHECK_EQ(got.rate_hz, p.expect_hz);
        CHECK(got.outcome == p.expect_outcome);
    }
}

TEST(rate_policy_honours_a_real_step_grid_with_the_new_guards) {
    /* A published step grid is the one case where `up` can legitimately be
     * rejected and `down` legitimately taken, so the added bounds must not
     * disturb it. */
    const auto want = adsb_like_preference();

    /* A 3 MHz grid counted from 1 MHz: 1, 4, 7, 10 ... MHz. The 8 MHz ideal
     * snaps down to 7 MHz, and then NEITHER the multiple below (6 MHz) nor the
     * one above (8 MHz) is on the grid, so the multiple preference is dropped
     * rather than forced off it. Both new bounds (`down <= rate`,
     * `up >= rate && up >= lo`) are satisfied here, so the grid check is what
     * has to do the rejecting — which is the point of the case. */
    const auto caps = caps_with_rate(1'000'000.0, 20'000'000.0, 3'000'000.0);
    const auto got = radio::choose_rx_rate(caps, want);
    std::printf("      3M grid from 1M -> %g %s\n", got.rate_hz, got.text());
    CHECK_EQ(got.rate_hz, 7'000'000.0);
    CHECK(got.outcome == radio::RateOutcome::Reduced);
    CHECK(std::isfinite(got.rate_hz));
    CHECK(got.rate_hz >= caps.rx_rate.min && got.rate_hz <= caps.rx_rate.max);
}
