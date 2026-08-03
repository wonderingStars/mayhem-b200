/*
 * mayhem-b200 — Recon state-machine tests.
 *
 * The expected values here are derived from the firmware's
 * application/apps/ui_recon.cpp on_statistics_update() and its stepping block:
 * the lock/dwell/hold timing (RECON_MATCH_CONTINUOUS vs SPARSE, positive vs
 * negative `wait`), the range/list stepping with end wrap, and the
 * !continuous stop-at-loop behaviour. The lock-out set is a host addition
 * (see ui_recon.hpp) and is tested as a plain set plus its effect on stepping.
 *
 * Everything is driven by injecting the channel level (`db`) and the elapsed
 * time (`interval_ms`) into ReconEngine::process(), exactly the way the firmware
 * was driven by a ChannelStatistics message every STATS_UPDATE_INTERVAL ms.
 * No radio, no UI.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "freqman_db.hpp"
#include "ui_recon.hpp"

#include <memory>
#include <utility>

using core::freqman_db;
using core::freqman_entry;
using core::freqman_invalid_index;
using core::freqman_type;

using app::ReconEngine;
using app::ReconStatus;

namespace {

constexpr int32_t kInterval = app::kStatsUpdateIntervalMs;  /* 100 ms */

/* A comfortable squelch so ACTIVE clears it and INACTIVE does not. */
constexpr int32_t kSquelch = -50;
constexpr int32_t kActive = 0;      /* > squelch  → signal present */
constexpr int32_t kInactive = -100; /* < squelch  → no signal      */

freqman_entry single(int64_t hz) {
    freqman_entry e;
    e.type = freqman_type::Single;
    e.frequency_a = hz;
    e.step = freqman_invalid_index;
    return e;
}

freqman_entry range(int64_t a, int64_t b) {
    freqman_entry e;
    e.type = freqman_type::Range;
    e.frequency_a = a;
    e.frequency_b = b;
    e.step = freqman_invalid_index;
    return e;
}

freqman_db make_db(std::initializer_list<freqman_entry> entries) {
    freqman_db db;
    for (const auto& e : entries)
        db.push_back(std::make_unique<freqman_entry>(e));
    return db;
}

/* An engine over `entries`, default step 25 kHz, continuous loop, prepared and
 * paused so stepping can be driven deterministically with request_step(). */
ReconEngine paused_engine(std::initializer_list<freqman_entry> entries,
                          int32_t def_step = 25'000) {
    ReconEngine eng;
    eng.set_default_step(def_step);
    eng.set_continuous(true);
    eng.set_list(make_db(entries));
    eng.prepare_start();
    eng.pause();
    return eng;
}

}  // namespace

/* ======================================================================== */
/* recon_step_range — the pure range-stepping helper                         */
/* ======================================================================== */

TEST(recon_step_range_forward_mid) {
    auto r = app::recon_step_range(100, 100, 200, 25, /*fwd*/ true);
    CHECK_EQ(r.freq, 125);
    CHECK(!r.wrapped);
}

TEST(recon_step_range_forward_wraps_at_max) {
    /* Landing exactly on max is not a wrap. */
    auto at_max = app::recon_step_range(175, 100, 200, 25, true);
    CHECK_EQ(at_max.freq, 200);
    CHECK(!at_max.wrapped);

    /* One more step overshoots → wrap to min. */
    auto over = app::recon_step_range(200, 100, 200, 25, true);
    CHECK_EQ(over.freq, 100);
    CHECK(over.wrapped);

    /* A step that overshoots without hitting max exactly still wraps. */
    auto over2 = app::recon_step_range(190, 100, 200, 25, true);
    CHECK_EQ(over2.freq, 100);
    CHECK(over2.wrapped);
}

TEST(recon_step_range_reverse_wraps_at_min) {
    auto mid = app::recon_step_range(150, 100, 200, 25, false);
    CHECK_EQ(mid.freq, 125);
    CHECK(!mid.wrapped);

    auto under = app::recon_step_range(100, 100, 200, 25, false);
    CHECK_EQ(under.freq, 200);
    CHECK(under.wrapped);

    auto under2 = app::recon_step_range(110, 100, 200, 25, false);
    CHECK_EQ(under2.freq, 200);
    CHECK(under2.wrapped);
}

TEST(recon_step_range_step_larger_than_span_wraps) {
    auto r = app::recon_step_range(100, 100, 150, 100, true);
    CHECK_EQ(r.freq, 100);
    CHECK(r.wrapped);
}

/* ======================================================================== */
/* LockoutSet                                                                */
/* ======================================================================== */

TEST(lockout_set_membership) {
    app::LockoutSet lo;
    CHECK(lo.empty());
    CHECK_EQ(lo.size(), 0u);

    lo.add(100);
    lo.add(200);
    lo.add(300);
    CHECK_EQ(lo.size(), 3u);
    CHECK(lo.contains(100));
    CHECK(lo.contains(300));
    CHECK(!lo.contains(150));

    /* Duplicate add does not grow the set. */
    lo.add(100);
    CHECK_EQ(lo.size(), 3u);

    CHECK(lo.remove(200));
    CHECK(!lo.contains(200));
    CHECK_EQ(lo.size(), 2u);

    /* Removing an absent element reports false. */
    CHECK(!lo.remove(999));

    lo.clear();
    CHECK(lo.empty());
    CHECK(!lo.contains(100));
}

/* ======================================================================== */
/* Engine frequency stepping (deterministic, via request_step)               */
/* ======================================================================== */

TEST(engine_list_step_forward_wraps) {
    auto eng = paused_engine({single(100'000'000), single(200'000'000), single(300'000'000)});
    CHECK_EQ(eng.current_index(), 0);
    CHECK_EQ(eng.freq(), 100'000'000);

    eng.request_step(1);
    eng.process(kInactive, kInterval);
    CHECK_EQ(eng.current_index(), 1);
    CHECK_EQ(eng.freq(), 200'000'000);

    eng.request_step(1);
    eng.process(kInactive, kInterval);
    CHECK_EQ(eng.current_index(), 2);
    CHECK_EQ(eng.freq(), 300'000'000);

    /* Wrap back to the start. */
    eng.request_step(1);
    eng.process(kInactive, kInterval);
    CHECK_EQ(eng.current_index(), 0);
    CHECK_EQ(eng.freq(), 100'000'000);
    CHECK(eng.has_looped());
}

TEST(engine_list_step_reverse_wraps) {
    auto eng = paused_engine({single(100'000'000), single(200'000'000), single(300'000'000)});

    /* From index 0 going backwards wraps to the last entry. */
    eng.request_step(-1);
    eng.process(kInactive, kInterval);
    CHECK_EQ(eng.current_index(), 2);
    CHECK_EQ(eng.freq(), 300'000'000);
    CHECK(!eng.forward());
}

TEST(engine_range_step_and_wrap) {
    /* One range 100..200 (units, not Hz), step 25. */
    auto eng = paused_engine({range(100, 200)}, /*def_step*/ 25);
    CHECK_EQ(eng.freq(), 100);
    CHECK_EQ(eng.step(), 25);

    const int64_t expected[] = {125, 150, 175, 200};
    for (int64_t want : expected) {
        eng.request_step(1);
        eng.process(kInactive, kInterval);
        CHECK_EQ(eng.freq(), want);
        CHECK_EQ(eng.current_index(), 0);
    }

    /* Stepping past max wraps to min (single-range list loops on itself). */
    eng.request_step(1);
    eng.process(kInactive, kInterval);
    CHECK_EQ(eng.freq(), 100);
    CHECK(eng.has_looped());
}

TEST(engine_range_sweep_driven_by_timer) {
    /* Integrated path: inactive signal in continuous match mode makes the
     * dwell expire every tick, so the engine advances one step per process(). */
    ReconEngine eng;
    eng.set_default_step(100);
    eng.set_continuous(true);
    eng.set_squelch(0);  /* kInactive (-100) never clears it */
    eng.set_match_mode(app::kReconMatchContinuous);
    eng.set_lock_duration(app::kReconMinLockDuration);
    eng.set_list(make_db({range(100, 300)}));
    eng.prepare_start();
    eng.resume();
    CHECK_EQ(eng.freq(), 100);

    eng.process(kInactive, kInterval);
    CHECK_EQ(eng.freq(), 200);
    eng.process(kInactive, kInterval);
    CHECK_EQ(eng.freq(), 300);
    /* Next step overshoots 300 → wrap to min. */
    eng.process(kInactive, kInterval);
    CHECK_EQ(eng.freq(), 100);
    CHECK_EQ(eng.current_index(), 0);
}

/* ======================================================================== */
/* Lock-out membership affecting stepping                                    */
/* ======================================================================== */

TEST(engine_stepping_skips_locked_out) {
    auto eng = paused_engine({single(100'000'000), single(200'000'000), single(300'000'000)});

    /* Lock out the middle entry. Stepping forward from 0 must land on 2. */
    eng.lockout().add(200'000'000);
    eng.request_step(1);
    eng.process(kInactive, kInterval);
    CHECK_EQ(eng.current_index(), 2);
    CHECK_EQ(eng.freq(), 300'000'000);
}

TEST(engine_lockout_current_adds_and_skips) {
    auto eng = paused_engine({single(100'000'000), single(200'000'000), single(300'000'000)});

    /* Sitting on entry 0, lock it out, then step. */
    eng.lockout_current();
    CHECK(eng.lockout().contains(100'000'000));

    eng.request_step(1);
    eng.process(kInactive, kInterval);
    /* 0→1 is fine (200M not locked). */
    CHECK_EQ(eng.current_index(), 1);
}

/* ======================================================================== */
/* Dwell / lock timing state machine                                         */
/* ======================================================================== */

TEST(engine_continuous_locks_after_nb_match) {
    ReconEngine eng;
    eng.set_squelch(kSquelch);
    eng.set_lock_nb_match(3);
    eng.set_lock_duration(app::kReconMinLockDuration);  /* 100 */
    eng.set_wait(200);
    eng.set_match_mode(app::kReconMatchContinuous);
    eng.set_list(make_db({single(100'000'000)}));
    eng.prepare_start();
    eng.resume();

    eng.process(kActive, kInterval);
    CHECK_EQ(eng.freq_lock(), 1u);
    CHECK(eng.status() == ReconStatus::Locking);

    eng.process(kActive, kInterval);
    CHECK_EQ(eng.freq_lock(), 2u);
    CHECK(eng.status() == ReconStatus::Locking);

    /* Third consecutive match reaches nb_match → LOCKED. */
    eng.process(kActive, kInterval);
    CHECK_EQ(eng.freq_lock(), 3u);
    CHECK(eng.status() == ReconStatus::Locked);
}

TEST(engine_continuous_gap_resets_and_steps) {
    /* Two entries; a gap mid-lock resets the run and steps on (continuous). */
    ReconEngine eng;
    eng.set_squelch(kSquelch);
    eng.set_lock_nb_match(3);
    eng.set_lock_duration(app::kReconMinLockDuration);
    eng.set_wait(200);
    eng.set_match_mode(app::kReconMatchContinuous);
    eng.set_continuous(true);
    eng.set_list(make_db({single(100'000'000), single(200'000'000)}));
    eng.prepare_start();
    eng.resume();

    eng.process(kActive, kInterval);
    eng.process(kActive, kInterval);
    CHECK_EQ(eng.freq_lock(), 2u);
    CHECK_EQ(eng.current_index(), 0);

    /* The gap: continuous mode drops the run to 0 and moves to the next entry. */
    eng.process(kInactive, kInterval);
    CHECK_EQ(eng.freq_lock(), 0u);
    CHECK_EQ(eng.current_index(), 1);
}

TEST(engine_sparse_accumulates_across_gaps) {
    /* Sparse mode keeps partial matches through gaps within the dwell window. */
    ReconEngine eng;
    eng.set_squelch(kSquelch);
    eng.set_lock_nb_match(5);
    eng.set_lock_duration(1000);  /* long window, no expiry during the test */
    eng.set_wait(200);
    eng.set_match_mode(app::kReconMatchSparse);
    eng.set_list(make_db({single(100'000'000)}));
    eng.prepare_start();
    eng.resume();

    eng.process(kActive, kInterval);    /* 1 */
    eng.process(kInactive, kInterval);  /* gap — preserved */
    eng.process(kActive, kInterval);    /* 2 */
    eng.process(kInactive, kInterval);  /* gap — preserved */
    eng.process(kActive, kInterval);    /* 3 */

    CHECK_EQ(eng.freq_lock(), 3u);
    CHECK(eng.status() == ReconStatus::Locking);
    CHECK_EQ(eng.current_index(), 0);  /* not enough to lock, no expiry, no step */
}

TEST(engine_positive_wait_dwells_then_continues) {
    /* wait >= 0: after a lock, the scan moves on once `wait` elapses even if the
     * signal is still present. */
    ReconEngine eng;
    eng.set_squelch(kSquelch);
    eng.set_lock_nb_match(1);   /* lock on the first match */
    eng.set_lock_duration(app::kReconMinLockDuration);
    eng.set_wait(200);          /* dwell 200 ms then continue */
    eng.set_match_mode(app::kReconMatchContinuous);
    eng.set_continuous(true);
    eng.set_list(make_db({single(100'000'000), single(200'000'000)}));
    eng.prepare_start();
    eng.resume();

    eng.process(kActive, kInterval);  /* locks, index still 0 */
    CHECK(eng.status() == ReconStatus::Locked);
    CHECK_EQ(eng.current_index(), 0);

    /* Signal stays active, but the fixed dwell expires and the scan advances. */
    eng.process(kActive, kInterval);
    eng.process(kActive, kInterval);
    CHECK_EQ(eng.current_index(), 1);
}

TEST(engine_negative_wait_holds_on_activity) {
    /* wait < 0: stay on the frequency as long as the signal is present, then
     * resume once it has been quiet for abs(wait). */
    ReconEngine eng;
    eng.set_squelch(kSquelch);
    eng.set_lock_nb_match(1);
    eng.set_lock_duration(app::kReconMinLockDuration);
    eng.set_wait(-500);
    eng.set_match_mode(app::kReconMatchContinuous);
    eng.set_continuous(true);
    eng.set_list(make_db({single(100'000'000), single(200'000'000)}));
    eng.prepare_start();
    eng.resume();

    /* Held on the active frequency indefinitely. */
    for (int i = 0; i < 12; ++i)
        eng.process(kActive, kInterval);
    CHECK(eng.status() == ReconStatus::Locked);
    CHECK_EQ(eng.current_index(), 0);

    /* Once the signal drops, the hold expires and the scan resumes. */
    bool stepped = false;
    for (int i = 0; i < 12 && !stepped; ++i) {
        eng.process(kInactive, kInterval);
        if (eng.current_index() != 0) stepped = true;
    }
    CHECK(stepped);
}

TEST(engine_pause_freezes_and_resume_restarts) {
    ReconEngine eng;
    eng.set_squelch(kSquelch);
    eng.set_lock_duration(app::kReconMinLockDuration);
    eng.set_match_mode(app::kReconMatchContinuous);
    eng.set_continuous(true);
    eng.set_list(make_db({single(100'000'000), single(200'000'000), single(300'000'000)}));
    eng.prepare_start();
    eng.resume();

    /* Inactive + continuous steps every tick. */
    eng.process(kInactive, kInterval);
    const int32_t after_one = eng.current_index();
    CHECK(after_one != 0);

    eng.pause();
    CHECK(!eng.recon());
    for (int i = 0; i < 10; ++i)
        eng.process(kActive, kInterval);
    CHECK_EQ(eng.current_index(), after_one);  /* frozen while paused */

    eng.resume();
    eng.process(kInactive, kInterval);
    CHECK(eng.current_index() != after_one);  /* scanning again */
}

TEST(engine_non_continuous_stops_at_loop_end) {
    /* With continuous == false the scan pauses when it wraps past the end. */
    ReconEngine eng;
    eng.set_squelch(kSquelch);
    eng.set_lock_duration(app::kReconMinLockDuration);
    eng.set_match_mode(app::kReconMatchContinuous);
    eng.set_continuous(false);
    eng.set_list(make_db({single(100'000'000), single(200'000'000)}));
    eng.prepare_start();
    eng.resume();

    eng.process(kInactive, kInterval);  /* 0 → 1 */
    CHECK_EQ(eng.current_index(), 1);
    CHECK(eng.recon());

    eng.process(kInactive, kInterval);  /* 1 → wraps → pause at boundary */
    CHECK(!eng.recon());
    CHECK_EQ(eng.current_index(), 0);
}

/* ======================================================================== */
/* Config-setter quirks kept from the firmware                               */
/* ======================================================================== */

TEST(engine_wait_minus_100_becomes_minus_200) {
    ReconEngine eng;
    eng.set_wait(-100);
    CHECK_EQ(eng.wait(), -200);  /* firmware anti-freeze kludge */

    eng.set_wait(-300);
    CHECK_EQ(eng.wait(), -300);
}

TEST(engine_nb_match_zero_clamps_to_one) {
    ReconEngine eng;
    eng.set_lock_nb_match(0);
    CHECK_EQ(eng.lock_nb_match(), 1u);
}
