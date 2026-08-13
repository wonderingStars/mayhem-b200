/*
 * mayhem-b200 — gapless raw sample tap tests.
 *
 * The property under test is the one the spectrum tap cannot offer: a consumer
 * gets every sample the radio delivered since its last read, exactly once and
 * in order, and when it cannot keep up the loss is counted and placed rather
 * than silent. That is proved here against a counter sequence — each sample
 * carries its own stream position, so "exactly once and in order" is checked
 * sample by sample rather than asserted from the shape of the code.
 *
 * The concurrency tests run a real producer thread against a real consumer
 * thread. They cannot prove the absence of a race on their own, but they do
 * exercise every ordering pairing the implementation relies on (see the memory
 * ordering note at the top of the tap's implementation in receiver_model.cpp),
 * and the exact accounting invariant
 *
 *      offered() == delivered() + dropped() + available()
 *
 * is checked after every concurrent run: any lost update to a position or a
 * counter breaks it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "audio_out.hpp"
#include "receiver_model.hpp"
#include "usrp_radio.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

/* Sample i carries its own stream position. Both components are used so a
 * half-copied sample cannot pass. Positions stay below 2^24, where a float
 * still represents every integer exactly. */
dsp::cfloat tap_sample(uint64_t i) {
    return dsp::cfloat{static_cast<float>(i), -static_cast<float>(i)};
}

void fill_block(std::vector<dsp::cfloat>& block, uint64_t base) {
    for (size_t k = 0; k < block.size(); k++) block[k] = tap_sample(base + k);
}

/* Walks the stream a consumer is handed and checks it against the counter
 * sequence: every block must start exactly where the previous block ended plus
 * the hole the tap reported, and must be internally contiguous. */
struct SequenceChecker {
    uint64_t expected{0};       /* stream position the next sample must carry */
    uint64_t received{0};
    uint64_t reported_lost{0};
    size_t bad_blocks{0};
    size_t size_mismatches{0};
    uint64_t first_bad_at{UINT64_MAX};

    void accept(const radio::RawSampleTap::Block& block,
                const std::vector<dsp::cfloat>& out) {
        if (out.size() != block.samples) size_mismatches++;

        expected += block.lost_before;
        reported_lost += block.lost_before;

        for (size_t k = 0; k < out.size(); k++) {
            if (out[k] != tap_sample(expected + k)) {
                bad_blocks++;
                if (first_bad_at == UINT64_MAX) first_bad_at = expected + k;
                break;
            }
        }

        expected += block.samples;
        received += block.samples;
    }

    void check_clean() const {
        CHECK_EQ(bad_blocks, size_t{0});
        CHECK_EQ(size_mismatches, size_t{0});
        CHECK_EQ(first_bad_at, UINT64_MAX);
    }
};

/* offered == delivered + dropped + still buffered, at any instant with the
 * producer stopped. */
void check_accounting(const radio::RawSampleTap& tap) {
    CHECK_EQ(tap.offered(),
             tap.delivered() + tap.dropped() + static_cast<uint64_t>(tap.available()));
}

}  // namespace

/* --- Sizing ---------------------------------------------------------------- */

TEST(gapless_tap_capacity_is_sized_in_time_not_in_samples) {
    using Tap = radio::RawSampleTap;

    /* The same 250 ms is a different sample count at every rate — which is the
     * entire point, and the opposite of the fixed 4096 the spectrum tap uses. */
    CHECK_EQ(Tap::capacity_for(2'000'000.0, 0.25), size_t{500'000});
    CHECK_EQ(Tap::capacity_for(2'400'000.0, 0.25), size_t{600'000});
    CHECK_EQ(Tap::capacity_for(8'000'000.0, 0.25), size_t{2'000'000});
    CHECK_EQ(Tap::capacity_for(16'000'000.0, 0.25), size_t{4'000'000});

    /* 16 Msps is the B200's USB-2 ceiling: 250 ms there is 32 MiB of cfloat,
     * which must sit under the byte cap rather than being clamped. */
    CHECK(Tap::capacity_for(16'000'000.0, 0.25) < Tap::kMaxCapacitySamples);

    /* UHD's published 61.44 Msps would want 15.4 Msamples; the cap holds it to
     * 64 MiB, which is still 136 ms of air. */
    CHECK_EQ(Tap::capacity_for(61'440'000.0, 0.25), Tap::kMaxCapacitySamples);
    CHECK(static_cast<double>(Tap::capacity_for(61'440'000.0, 0.25)) / 61'440'000.0 > 0.13);

    /* A slow device still gets a usable window rather than a few samples. */
    CHECK_EQ(Tap::capacity_for(48'000.0, 0.25), Tap::kMinCapacitySamples);

    /* Nonsense in, floor out — never zero, which would make write() a silent
     * no-op on an open tap. */
    CHECK_EQ(Tap::capacity_for(0.0, 0.25), Tap::kMinCapacitySamples);
    CHECK_EQ(Tap::capacity_for(2'000'000.0, 0.0), Tap::kMinCapacitySamples);
    CHECK_EQ(Tap::capacity_for(-1.0, -1.0), Tap::kMinCapacitySamples);
}

/* --- Closed tap ------------------------------------------------------------ */

TEST(gapless_tap_is_inert_until_it_is_opened) {
    radio::RawSampleTap tap;
    CHECK(!tap.is_open());
    CHECK_EQ(tap.capacity(), size_t{0});
    CHECK_EQ(tap.available(), size_t{0});

    std::vector<dsp::cfloat> block(1000);
    fill_block(block, 0);
    tap.write(block.data(), block.size());

    /* A closed tap does not even count: the 29 apps that never open it must not
     * pay for it, and its counters must not report loss that never happened. */
    CHECK_EQ(tap.offered(), uint64_t{0});
    CHECK_EQ(tap.dropped(), uint64_t{0});
    CHECK_EQ(tap.overflows(), uint32_t{0});

    std::vector<dsp::cfloat> out;
    const auto got = tap.read(out);
    CHECK_EQ(got.samples, size_t{0});
    CHECK_EQ(got.lost_before, uint64_t{0});
    CHECK(out.empty());

    CHECK(!tap.open(0));
    CHECK(!tap.is_open());
}

/* --- The core contract ----------------------------------------------------- */

TEST(gapless_tap_delivers_every_sample_exactly_once_and_in_order) {
    radio::RawSampleTap tap;
    CHECK(tap.open(16'384));
    CHECK_EQ(tap.capacity(), size_t{16'384});

    /* Block sizes that are coprime with the capacity, so the ring wraps at a
     * different offset every time and a wrap bug cannot hide. */
    const size_t sizes[] = {1, 3, 997, 4096, 8192, 511};

    SequenceChecker seq;
    std::vector<dsp::cfloat> block;
    std::vector<dsp::cfloat> out;

    uint64_t written = 0;
    size_t i = 0;
    while (written < 200'000) {
        const size_t n = sizes[i++ % (sizeof(sizes) / sizeof(sizes[0]))];
        block.resize(n);
        fill_block(block, written);
        tap.write(block.data(), n);
        written += n;

        seq.accept(tap.read(out), out);
    }

    seq.check_clean();
    CHECK_EQ(seq.received, written);
    CHECK_EQ(seq.reported_lost, uint64_t{0});
    CHECK_EQ(tap.dropped(), uint64_t{0});
    CHECK_EQ(tap.overflows(), uint32_t{0});
    CHECK_EQ(tap.delivered(), written);
    CHECK_EQ(tap.offered(), written);
    check_accounting(tap);

    /* The positions ran well past the capacity, so the modulo wrap was
     * exercised repeatedly rather than incidentally. */
    CHECK(written > 10 * tap.capacity());
}

TEST(gapless_tap_read_returns_everything_since_the_previous_read) {
    /* This is the difference from take_spectrum_samples(), stated as a test:
     * three blocks arrive between two polls and all three come back, rather
     * than the newest fixed-size window of them. */
    radio::RawSampleTap tap;
    CHECK(tap.open(8192));

    std::vector<dsp::cfloat> block(1000);
    for (uint64_t b = 0; b < 3; b++) {
        fill_block(block, b * 1000);
        tap.write(block.data(), block.size());
    }
    CHECK_EQ(tap.available(), size_t{3000});

    std::vector<dsp::cfloat> out;
    const auto got = tap.read(out);
    CHECK_EQ(got.samples, size_t{3000});
    CHECK(got.contiguous());
    CHECK_EQ(out.size(), size_t{3000});
    CHECK(out.front() == tap_sample(0));
    CHECK(out.back() == tap_sample(2999));

    /* And the read empties it: a second poll with nothing new returns nothing,
     * not a stale copy of what was already handed over. */
    const auto again = tap.read(out);
    CHECK_EQ(again.samples, size_t{0});
    CHECK(out.empty());
    check_accounting(tap);
}

/* --- Loss ------------------------------------------------------------------ */

TEST(gapless_tap_counts_the_loss_and_places_it_in_the_stream) {
    radio::RawSampleTap tap;
    CHECK(tap.open(1024));

    std::vector<dsp::cfloat> block(768);
    std::vector<dsp::cfloat> out;

    /* 768 fit. */
    fill_block(block, 0);
    tap.write(block.data(), block.size());
    CHECK_EQ(tap.dropped(), uint64_t{0});

    /* Of the next 768 only 256 fit; the ring tears at position 1024. */
    fill_block(block, 768);
    tap.write(block.data(), block.size());
    CHECK_EQ(tap.dropped(), uint64_t{512});
    CHECK_EQ(tap.overflows(), uint32_t{1});

    /* Everything offered while torn is lost too, but it is the same hole, so it
     * adds samples to dropped() and not events to overflows(). */
    fill_block(block, 1536);
    tap.write(block.data(), block.size());
    CHECK_EQ(tap.dropped(), uint64_t{512 + 768});
    CHECK_EQ(tap.overflows(), uint32_t{1});
    check_accounting(tap);

    /* The first read gets the whole pre-tear run, contiguous, with no hole
     * announced: the hole is after these samples, not before them. */
    auto got = tap.read(out);
    CHECK_EQ(got.samples, size_t{1024});
    CHECK(got.contiguous());
    CHECK(out.front() == tap_sample(0));
    CHECK(out.back() == tap_sample(1023));

    /* Draining to the seam lets the producer start again. */
    fill_block(block, 2304);
    tap.write(block.data(), block.size());

    got = tap.read(out);
    CHECK_EQ(got.samples, size_t{768});
    CHECK_EQ(got.lost_before, uint64_t{1280});
    /* 1024 delivered + 1280 lost = 2304: the hole is reported at exactly the
     * position where it happened, which is what lets a demodulator reset its
     * state then and only then. */
    CHECK(out.front() == tap_sample(2304));
    CHECK(out.back() == tap_sample(3071));

    CHECK_EQ(tap.delivered(), uint64_t{1024 + 768});
    check_accounting(tap);
}

TEST(gapless_tap_loss_is_bounded_by_what_the_ring_cannot_hold) {
    /* A consumer that never reads must lose (offered - capacity) samples, not
     * an unbounded or unknown amount, and must still be handed a full ring of
     * contiguous samples when it finally does read. */
    radio::RawSampleTap tap;
    CHECK(tap.open(4096));

    std::vector<dsp::cfloat> block(4096);
    const uint64_t rounds = 100;
    for (uint64_t b = 0; b < rounds; b++) {
        fill_block(block, b * block.size());
        tap.write(block.data(), block.size());
    }

    const uint64_t offered = rounds * 4096;
    CHECK_EQ(tap.offered(), offered);
    CHECK_EQ(tap.available(), size_t{4096});
    CHECK_EQ(tap.dropped(), offered - 4096);
    CHECK(tap.dropped() < tap.offered());  /* bounded, not total */
    check_accounting(tap);

    std::vector<dsp::cfloat> out;
    const auto got = tap.read(out);
    CHECK_EQ(got.samples, size_t{4096});
    CHECK(got.contiguous());
    CHECK(out.front() == tap_sample(0));
    CHECK(out.back() == tap_sample(4095));
    check_accounting(tap);
}

TEST(gapless_tap_write_never_waits_for_the_consumer) {
    /* Non-blocking is a liveness property: a blocking implementation would hang
     * this test rather than fail it. The elapsed bound is deliberately loose —
     * it is there to turn a hang into a failure on a loaded machine, not to
     * measure throughput. */
    radio::RawSampleTap tap;
    CHECK(tap.open(1024));

    std::vector<dsp::cfloat> block(4096);
    fill_block(block, 0);

    const auto t0 = std::chrono::steady_clock::now();

    /* Phase 1: nobody ever reads. Every write after the first must return
     * immediately having dropped its samples. */
    for (int i = 0; i < 20'000; i++) tap.write(block.data(), block.size());

    /* Phase 2: a consumer that drains between writes, so the copy path runs
     * rather than the torn early-out. */
    std::vector<dsp::cfloat> out;
    for (int i = 0; i < 20'000; i++) {
        tap.write(block.data(), block.size());
        tap.read(out);
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
    std::printf("    40000 writes with no back-pressure: %lld ms\n",
                static_cast<long long>(ms));
    CHECK(ms < 5000);

    CHECK(tap.dropped() > 0);
    check_accounting(tap);
}

/* --- Control --------------------------------------------------------------- */

TEST(gapless_tap_restart_resizes_and_reports_what_it_discards) {
    radio::RawSampleTap tap;
    CHECK(tap.open(1024));

    std::vector<dsp::cfloat> block(500);
    fill_block(block, 0);
    tap.write(block.data(), block.size());
    CHECK_EQ(tap.available(), size_t{500});

    /* A rate change re-sizes the ring. The buffered samples are not adjacent in
     * time to what follows, so they are loss and are reported as such rather
     * than being quietly spliced onto the new stream. */
    tap.restart(2048);
    CHECK_EQ(tap.capacity(), size_t{2048});
    CHECK_EQ(tap.available(), size_t{0});
    CHECK_EQ(tap.dropped(), uint64_t{500});

    fill_block(block, 500);
    tap.write(block.data(), block.size());

    std::vector<dsp::cfloat> out;
    const auto got = tap.read(out);
    CHECK_EQ(got.samples, size_t{500});
    CHECK_EQ(got.lost_before, uint64_t{500});
    CHECK(out.front() == tap_sample(500));
    check_accounting(tap);

    /* restart() on a closed tap does not open one. */
    radio::RawSampleTap closed;
    closed.restart(4096);
    CHECK(!closed.is_open());
    CHECK_EQ(closed.capacity(), size_t{0});
}

TEST(gapless_tap_restart_during_an_overflow_still_reports_the_whole_hole) {
    /* A hole is only announced by the samples that follow it. Restarting while
     * one is open means those samples never come, so the restart has to carry
     * the announcement — otherwise dropped() would know about loss the consumer
     * was never told about, and a decoder would splice across it. */
    radio::RawSampleTap tap;
    CHECK(tap.open(1024));

    std::vector<dsp::cfloat> block(768);
    fill_block(block, 0);
    tap.write(block.data(), block.size());     /* 768 in */
    fill_block(block, 768);
    tap.write(block.data(), block.size());     /* 256 in, 512 lost, tears */
    fill_block(block, 1536);
    tap.write(block.data(), block.size());     /* 768 lost, same hole */

    CHECK_EQ(tap.available(), size_t{1024});
    CHECK_EQ(tap.dropped(), uint64_t{1280});

    /* 1024 buffered but never read, plus the 1280 already lost. */
    tap.restart(2048);
    CHECK_EQ(tap.dropped(), uint64_t{2304});

    fill_block(block, 2304);
    tap.write(block.data(), block.size());

    std::vector<dsp::cfloat> out;
    const auto got = tap.read(out);
    CHECK_EQ(got.samples, size_t{768});
    CHECK_EQ(got.lost_before, uint64_t{2304});
    CHECK(out.front() == tap_sample(2304));
    CHECK_EQ(tap.dropped(), got.lost_before);
    check_accounting(tap);
}

TEST(gapless_tap_close_stops_delivery_and_releases_the_ring) {
    radio::RawSampleTap tap;
    CHECK(tap.open(1024));

    std::vector<dsp::cfloat> block(500);
    fill_block(block, 0);
    tap.write(block.data(), block.size());

    tap.close();
    CHECK(!tap.is_open());
    CHECK_EQ(tap.capacity(), size_t{0});
    CHECK_EQ(tap.available(), size_t{0});
    CHECK_EQ(tap.dropped(), uint64_t{500});

    const uint64_t offered_before = tap.offered();
    fill_block(block, 500);
    tap.write(block.data(), block.size());
    CHECK_EQ(tap.offered(), offered_before);

    std::vector<dsp::cfloat> out;
    const auto got = tap.read(out);
    CHECK_EQ(got.samples, size_t{0});
    check_accounting(tap);

    tap.reset_stats();
    CHECK_EQ(tap.offered(), uint64_t{0});
    CHECK_EQ(tap.dropped(), uint64_t{0});
    CHECK_EQ(tap.delivered(), uint64_t{0});
    CHECK_EQ(tap.overflows(), uint32_t{0});
}

/* --- Concurrency ----------------------------------------------------------- */

TEST(gapless_tap_concurrent_stream_is_lossless_when_the_consumer_keeps_up) {
    /* A real producer thread against a real consumer thread. The throttle is in
     * the test, never in the tap: the producer waits for space before offering,
     * so any drop here is a bug in the ring rather than a slow consumer. */
    radio::RawSampleTap tap;
    const size_t capacity = 1u << 16;
    CHECK(tap.open(capacity));

    const uint64_t total = 1'000'000;
    const size_t chunk = 4096;

    std::atomic<bool> producing{true};

    std::thread producer([&] {
        std::vector<dsp::cfloat> block(chunk);
        uint64_t sent = 0;
        while (sent < total) {
            const size_t n = static_cast<size_t>(
                std::min<uint64_t>(chunk, total - sent));
            block.resize(n);
            fill_block(block, sent);
            while (tap.available() + n > capacity) std::this_thread::yield();
            tap.write(block.data(), n);
            sent += n;
        }
        producing.store(false);
    });

    SequenceChecker seq;
    std::thread consumer([&] {
        std::vector<dsp::cfloat> out;
        while (producing.load() || tap.available() != 0)
            seq.accept(tap.read(out), out);
        seq.accept(tap.read(out), out);
    });

    producer.join();
    consumer.join();

    seq.check_clean();
    CHECK_EQ(seq.received, total);
    CHECK_EQ(seq.reported_lost, uint64_t{0});
    CHECK_EQ(tap.dropped(), uint64_t{0});
    CHECK_EQ(tap.overflows(), uint32_t{0});
    CHECK_EQ(tap.delivered(), total);
    CHECK_EQ(tap.offered(), total);
    check_accounting(tap);
}

TEST(gapless_tap_concurrent_slow_consumer_loses_samples_but_never_order) {
    /* The producer runs flat out into a ring that holds a fraction of what it
     * sends, and the consumer sleeps between reads. Every block it does get
     * must still be internally contiguous and must start exactly where the
     * reported hole says it should — a spliced block would be far worse than a
     * missing one for a preamble detector. */
    radio::RawSampleTap tap;
    CHECK(tap.open(8192));

    const uint64_t total = 2'000'000;
    const size_t chunk = 1024;

    std::atomic<bool> producing{true};

    std::thread producer([&] {
        std::vector<dsp::cfloat> block(chunk);
        uint64_t sent = 0;
        size_t blocks = 0;
        while (sent < total) {
            const size_t n = static_cast<size_t>(std::min<uint64_t>(chunk, total - sent));
            block.resize(n);
            fill_block(block, sent);
            tap.write(block.data(), n);
            sent += n;
            /* Pace it so the two threads genuinely interleave for a while
             * instead of the producer finishing before the first read. */
            if ((++blocks % 64) == 0)
                std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        producing.store(false);
    });

    SequenceChecker seq;
    std::thread consumer([&] {
        std::vector<dsp::cfloat> out;
        while (producing.load()) {
            seq.accept(tap.read(out), out);
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });

    producer.join();
    consumer.join();

    /* Drain what is left, then offer one more sample: the hole still open when
     * the producer stopped can only be announced by the data that follows it,
     * so a final write is what settles the books. */
    std::vector<dsp::cfloat> out;
    for (int i = 0; i < 4; i++) seq.accept(tap.read(out), out);

    const uint64_t offered_before_flush = tap.offered();
    const dsp::cfloat sentinel = tap_sample(total);
    tap.write(&sentinel, 1);
    seq.accept(tap.read(out), out);

    seq.check_clean();
    CHECK_EQ(seq.expected, total + 1);
    CHECK(seq.reported_lost > 0);   /* it really did fall behind */
    CHECK(seq.received > 0);        /* and really did get samples anyway */

    /* Everything offered before the flush is accounted for as either delivered
     * or reported lost — no sample vanished without a name. */
    CHECK_EQ(seq.reported_lost + seq.received, offered_before_flush + 1);
    CHECK_EQ(tap.dropped(), seq.reported_lost);
    CHECK(tap.overflows() > 0);
    check_accounting(tap);

    std::printf("    slow consumer: offered %llu, delivered %llu, lost %llu in %u holes\n",
                static_cast<unsigned long long>(tap.offered()),
                static_cast<unsigned long long>(tap.delivered()),
                static_cast<unsigned long long>(tap.dropped()),
                tap.overflows());
}

TEST(gapless_tap_survives_open_and_close_under_a_running_producer) {
    /* The control path reallocates the buffer the DSP thread writes into. The
     * handshake has to make the producer either skip the buffer entirely or
     * finish before the free, and it must be the caller that waits. */
    radio::RawSampleTap tap;
    CHECK(tap.open(4096));

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> writes{0};

    std::thread producer([&] {
        std::vector<dsp::cfloat> block(2048);
        fill_block(block, 0);
        while (!stop.load()) {
            tap.write(block.data(), block.size());
            writes.fetch_add(1);
        }
    });

    std::vector<dsp::cfloat> out;
    for (int i = 0; i < 200; i++) {
        tap.read(out);
        tap.restart(4096 + static_cast<size_t>(i) * 16);
        tap.read(out);
        tap.close();
        CHECK(tap.open(8192));
    }

    stop.store(true);
    producer.join();

    CHECK(writes.load() > 0);
    CHECK(tap.is_open());
    CHECK_EQ(tap.capacity(), size_t{8192});
    check_accounting(tap);
}

/* --- ReceiverModel surface ------------------------------------------------- */

TEST(receiver_raw_tap_is_off_by_default_and_sized_from_the_rate) {
    radio::UsrpRadio r;
    audio::AudioOut a;
    radio::ReceiverModel rx{r, a};

    /* Off unless an app asks: at 16 Msps a copy of every sample is 128 MB/s,
     * and 29 apps have no use for it. */
    CHECK(!rx.raw_tap_enabled());
    CHECK_EQ(rx.raw_tap().capacity(), size_t{0});

    const double rate = rx.sampling_rate();
    CHECK(rate > 0.0);

    CHECK(rx.enable_raw_tap());
    CHECK(rx.raw_tap_enabled());
    CHECK_EQ(rx.raw_tap().capacity(),
             radio::RawSampleTap::capacity_for(
                 rate, radio::RawSampleTap::kDefaultHistorySeconds));

    /* A longer history is a bigger ring at the same rate. */
    CHECK(rx.enable_raw_tap(0.5));
    CHECK_EQ(rx.raw_tap().capacity(),
             radio::RawSampleTap::capacity_for(rate, 0.5));
    CHECK(rx.raw_tap().capacity() > radio::RawSampleTap::capacity_for(
                                        rate, radio::RawSampleTap::kDefaultHistorySeconds));

    /* A nonsense history falls back to the default rather than to a zero-length
     * ring that would silently swallow the stream. */
    CHECK(rx.enable_raw_tap(0.0));
    CHECK_EQ(rx.raw_tap().capacity(),
             radio::RawSampleTap::capacity_for(
                 rate, radio::RawSampleTap::kDefaultHistorySeconds));

    rx.disable_raw_tap();
    CHECK(!rx.raw_tap_enabled());
    CHECK_EQ(rx.raw_tap().capacity(), size_t{0});
}

TEST(receiver_raw_tap_does_not_disturb_the_spectrum_tap) {
    /* The 28 other apps keep take_spectrum_samples() exactly as it was: the two
     * taps share the DSP thread's samples and nothing else. */
    radio::UsrpRadio r;
    audio::AudioOut a;
    radio::ReceiverModel rx{r, a};

    std::vector<dsp::cfloat> out;
    CHECK(!rx.take_spectrum_samples(out, 4096));

    CHECK(rx.enable_raw_tap());
    CHECK(!rx.take_spectrum_samples(out, 4096));

    /* And a zero count is still rejected, tap or no tap. */
    CHECK(!rx.take_spectrum_samples(out, 0));

    rx.disable_raw_tap();
    CHECK(!rx.take_spectrum_samples(out, 4096));
}
