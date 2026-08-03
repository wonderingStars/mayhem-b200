/*
 * mayhem-b200 — tests for the scanning / finding cluster:
 * Scanner, Signal Hunter, Detector RX and Fox hunt.
 *
 * Expected values are derived from the upstream firmware, not from what this
 * port happens to produce:
 *
 *   Scanner        firmware/application/external/scanner/ui_scanner.cpp —
 *                  ScannerThread::run() (list and range stepping, wrap,
 *                  index_stepper priority, freq_lock inhibit, deferred delete)
 *                  and ScannerView::on_statistics_update() with its
 *                  MAX_FREQ_LOCK / browse_wait / lock_wait timing at
 *                  STATISTICS_UPDATES_PER_SEC.
 *
 *   Signal Hunter  firmware/baseband/proc_signal_hunter.cpp — the
 *                  ((I*I + Q*Q) >> 16) energy, the 64-entry sliding mean, the
 *                  hangtime_ms * 250 sample conversion with its zero guard, and
 *                  the IDLE/AWAITING_STREAM/RECORDING/HANGTIME/AWAITING_CLOSE
 *                  transitions. The FFT peak search is a host addition and is
 *                  tested against a synthesised tone whose bin is known from
 *                  the transform definition.
 *
 *   Detector RX    firmware/application/external/detector_rx/ui_detector_rx.cpp —
 *                  map(), the beep pitch mapping, format_freq_mhz(),
 *                  init_current_entry() and on_timer().
 *
 *   Fox hunt       the RSSI smoothing / peak-hold / trend / threshold logic that
 *                  replaces upstream's GPS+compass map on a B200 (see
 *                  src/apps/ui_foxhunt_rx.hpp); checked against the closed-form
 *                  EMA and the documented hysteresis rules.
 *
 * No radio and no screen are involved anywhere in this file. End-to-end RF
 * reception is NOT covered — no hardware is attached.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "freqman_db.hpp"
#include "ui_detector_rx.hpp"
#include "ui_foxhunt_rx.hpp"
#include "ui_scanner.hpp"
#include "ui_signal_hunter.hpp"

#include "../src/dsp/fft.hpp"

#include <cmath>
#include <memory>
#include <vector>

using app::scanner::BigDisplayColor;
using app::scanner::ScannerEngine;
using app::scanner::ScannerRange;
using app::scanner::ScannerStepper;

using app::signal_hunter::HuntDetector;
using app::signal_hunter::HuntState;

using app::detector_rx::DetectorScanner;
using app::detector_rx::LevelHistory;

using app::foxhunt::BearingLog;
using app::foxhunt::FoxhuntEngine;
using app::foxhunt::Trend;

namespace {

/* --- freqman helpers ---------------------------------------------------- */

core::freqman_entry make_single(int64_t hz, const std::string& desc = "") {
    core::freqman_entry e;
    e.type = core::freqman_type::Single;
    e.frequency_a = hz;
    e.description = desc;
    e.step = core::freqman_invalid_index;
    return e;
}

core::freqman_entry make_range(int64_t a, int64_t b,
                               core::freqman_index_t step = core::freqman_invalid_index) {
    core::freqman_entry e;
    e.type = core::freqman_type::Range;
    e.frequency_a = a;
    e.frequency_b = b;
    e.step = step;
    return e;
}

core::freqman_db make_db(std::initializer_list<core::freqman_entry> entries) {
    core::freqman_db db;
    for (const auto& e : entries) db.push_back(std::make_unique<core::freqman_entry>(e));
    return db;
}

/* Index of the freqman step option whose value is `hz`, or the invalid index. */
core::freqman_index_t step_index_for(int32_t hz) {
    for (size_t i = 0; i < core::freqman_step_count(); i++) {
        const auto idx = static_cast<core::freqman_index_t>(i);
        if (core::freqman_entry_get_step_value(idx) == hz) return idx;
    }
    return core::freqman_invalid_index;
}

/* --- scanner helpers ---------------------------------------------------- */

constexpr int32_t kSquelch = -50;
constexpr int32_t kLoud = 0;      /* > squelch */
constexpr int32_t kQuiet = -100;  /* < squelch */

ScannerEngine make_engine(std::vector<int64_t> freqs) {
    ScannerEngine eng;
    eng.set_squelch(kSquelch);
    eng.set_browse_wait(5);  /* 5 s -> 50 statistics updates */
    eng.set_lock_wait(2);    /* 2 s -> 20 statistics updates */
    eng.stepper().start_list(std::move(freqs), /*fwd*/ true);
    return eng;
}

}  // namespace

/* ======================================================================== *
 *  Scanner — ScannerThread::run() stepping                                  *
 * ======================================================================== */

TEST(scanner_list_steps_forward_and_wraps) {
    /* Upstream starts frequency_index at `size` when stepping forward, so the
     * first pass wraps to entry 0. */
    ScannerStepper s;
    s.start_list({100, 200, 300}, /*fwd*/ true);

    CHECK(s.running());
    CHECK_EQ(s.size(), 3);

    auto t = s.tick();
    CHECK(t.emitted);
    CHECK(t.retuned);
    CHECK_EQ(t.freq, static_cast<int64_t>(100));
    CHECK_EQ(t.index, 0u);

    CHECK_EQ(s.tick().freq, static_cast<int64_t>(200));
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(300));
    /* wrap */
    t = s.tick();
    CHECK_EQ(t.freq, static_cast<int64_t>(100));
    CHECK_EQ(t.index, 0u);
}

TEST(scanner_list_steps_reverse_and_wraps) {
    /* Reverse starts at 0, so the first pass wraps to the last entry. */
    ScannerStepper s;
    s.start_list({100, 200, 300}, /*fwd*/ false);

    CHECK_EQ(s.tick().freq, static_cast<int64_t>(300));
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(200));
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(100));
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(300));
}

TEST(scanner_empty_list_does_not_run) {
    ScannerStepper s;
    s.start_list({}, true);
    CHECK(!s.running());
    const auto t = s.tick();
    CHECK(!t.emitted);
    CHECK(!t.retuned);
}

TEST(scanner_range_steps_and_wraps) {
    /* size = (max - min) / step = 500000 / 100000 = 5, so positions are
     * min + {0..4} * step and the top endpoint is never visited — upstream's
     * arithmetic exactly. */
    ScannerStepper s;
    s.start_range(ScannerRange{100'000'000, 100'500'000}, 100'000, /*fwd*/ true);

    CHECK(s.running());
    CHECK(s.manual_search());
    CHECK_EQ(s.size(), 5);

    auto t = s.tick();
    CHECK_EQ(t.freq, static_cast<int64_t>(100'000'000));
    CHECK_EQ(t.index, 0u);  /* range mode reports 0, as upstream */

    CHECK_EQ(s.tick().freq, static_cast<int64_t>(100'100'000));
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(100'200'000));
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(100'300'000));
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(100'400'000));
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(100'000'000));  /* wrap */
}

TEST(scanner_range_steps_reverse) {
    ScannerStepper s;
    s.start_range(ScannerRange{100'000'000, 100'500'000}, 100'000, /*fwd*/ false);

    CHECK_EQ(s.tick().freq, static_cast<int64_t>(100'400'000));
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(100'300'000));
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(100'200'000));
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(100'100'000));
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(100'000'000));
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(100'400'000));  /* wrap */
}

TEST(scanner_range_needs_a_step) {
    ScannerStepper s;
    s.start_range(ScannerRange{100'000'000, 100'500'000}, 0, true);
    CHECK(!s.running());
}

TEST(scanner_freq_lock_inhibits_stepping) {
    /* run(): the index only advances when (_freq_lock == 0), but a Retune is
     * still emitted every pass. */
    ScannerStepper s;
    s.start_list({100, 200, 300}, true);
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(100));

    s.set_freq_lock(1);
    auto t = s.tick();
    CHECK(t.emitted);
    CHECK(!t.retuned);
    CHECK_EQ(t.freq, static_cast<int64_t>(100));

    s.set_freq_lock(0);
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(200));
}

TEST(scanner_index_stepper_overrides_lock_and_pause) {
    /* _index_stepper takes priority over _stepper and works while paused. */
    ScannerStepper s;
    s.start_list({100, 200, 300}, true);
    s.tick();  /* on 100 */

    s.set_scanning(false);
    s.set_freq_lock(5);
    s.set_index_stepper(1);

    auto t = s.tick();
    CHECK(t.emitted);
    CHECK(t.retuned);
    CHECK_EQ(t.freq, static_cast<int64_t>(200));
    /* one-shot: cleared after use */
    CHECK_EQ(s.index_stepper(), 0);

    /* still paused, so nothing more moves */
    t = s.tick();
    CHECK(!t.emitted);
    CHECK_EQ(s.index(), 1);
}

TEST(scanner_index_stepper_reverse_wraps) {
    ScannerStepper s;
    s.start_list({100, 200, 300}, true);
    s.tick();  /* on 100, index 0 */
    s.set_scanning(false);
    s.set_index_stepper(-1);
    CHECK_EQ(s.tick().freq, static_cast<int64_t>(300));
}

TEST(scanner_deferred_delete_removes_frequency) {
    /* Upstream only services _freq_del while the scan is paused. */
    ScannerStepper s;
    s.start_list({100, 200, 300}, true);
    s.tick();

    s.set_scanning(false);
    s.set_freq_del(200);

    const auto t = s.tick();
    CHECK(!t.emitted);
    CHECK_EQ(s.size(), 2);
    CHECK_EQ(s.frequencies().size(), static_cast<size_t>(2));
    CHECK_EQ(s.frequencies()[0], static_cast<int64_t>(100));
    CHECK_EQ(s.frequencies()[1], static_cast<int64_t>(300));
    CHECK_EQ(s.freq_del(), static_cast<int64_t>(0));
}

TEST(scanner_delete_is_ignored_while_scanning) {
    ScannerStepper s;
    s.start_list({100, 200, 300}, true);
    s.set_freq_del(200);
    s.tick();
    CHECK_EQ(s.size(), 3);
    CHECK_EQ(s.freq_del(), static_cast<int64_t>(200));
}

/* ======================================================================== *
 *  Scanner — ScannerView::on_statistics_update() timing                     *
 * ======================================================================== */

TEST(scanner_locks_after_max_freq_lock_updates) {
    /* Upstream: each statistics update above squelch bumps _freq_lock; the scan
     * pauses on the update *after* it reaches MAX_FREQ_LOCK (10), and
     * scan_pause() resets the lock to 0. */
    auto eng = make_engine({100, 200, 300});

    for (int i = 0; i < 10; i++) eng.on_statistics_update(kLoud);
    CHECK_EQ(eng.stepper().is_freq_lock(), app::scanner::kMaxFreqLock);
    CHECK(eng.stepper().is_scanning());
    CHECK_EQ(eng.browse_timer(), 0u);

    eng.on_statistics_update(kLoud);  /* update 11 pauses */
    CHECK(!eng.stepper().is_scanning());
    CHECK_EQ(eng.stepper().is_freq_lock(), 0u);
    CHECK_EQ(eng.browse_timer(), 1u);
    CHECK(eng.audio_enabled());
}

TEST(scanner_browse_wait_forces_resume) {
    /* browse_timer starts at 1 on update 11 and gains 1 per update, so it
     * reaches browse_wait * STATISTICS_UPDATES_PER_SEC = 50 at update 60 and the
     * check at the top of update 61 resumes the scan. */
    auto eng = make_engine({100, 200, 300});

    int updates = 0;
    while (eng.stepper().is_scanning() && updates < 200) {
        eng.on_statistics_update(kLoud);
        updates++;
    }
    CHECK_EQ(updates, 11);

    while (!eng.stepper().is_scanning() && updates < 200) {
        eng.on_statistics_update(kLoud);
        updates++;
    }
    CHECK_EQ(updates, 61);
    CHECK_EQ(eng.browse_timer(), 0u);
    CHECK(!eng.audio_enabled());
}

TEST(scanner_lock_wait_resumes_after_signal_lost) {
    /* Once paused, a quiet channel bumps lock_timer each update and the scan
     * resumes when it reaches lock_wait * STATISTICS_UPDATES_PER_SEC = 20. */
    auto eng = make_engine({100, 200, 300});

    for (int i = 0; i < 11; i++) eng.on_statistics_update(kLoud);
    CHECK(!eng.stepper().is_scanning());

    int quiet = 0;
    while (!eng.stepper().is_scanning() && quiet < 100) {
        eng.on_statistics_update(kQuiet);
        quiet++;
    }
    CHECK_EQ(quiet, 20);
    CHECK_EQ(eng.browse_timer(), 0u);
}

TEST(scanner_lost_signal_before_pause_clears_lock) {
    auto eng = make_engine({100, 200, 300});

    for (int i = 0; i < 3; i++) eng.on_statistics_update(kLoud);
    CHECK_EQ(eng.stepper().is_freq_lock(), 3u);

    eng.on_statistics_update(kQuiet);
    CHECK_EQ(eng.stepper().is_freq_lock(), 0u);
    CHECK(eng.color() == BigDisplayColor::Grey);
    CHECK(eng.stepper().is_scanning());
}

TEST(scanner_squelch_while_paused_needs_three_updates) {
    /* update_squelch_while_paused() only acts when ++color_timer > 2. */
    auto eng = make_engine({100, 200, 300});
    eng.user_pause();
    CHECK(eng.userpause());
    CHECK(!eng.stepper().is_scanning());
    CHECK(eng.audio_enabled());  /* scan_pause() unmutes */

    eng.on_statistics_update(kQuiet);
    eng.on_statistics_update(kQuiet);
    CHECK(eng.audio_enabled());  /* not acted on yet */
    CHECK_EQ(eng.color_timer(), 2u);

    eng.on_statistics_update(kQuiet);
    CHECK(!eng.audio_enabled());
    CHECK(eng.color() == BigDisplayColor::Grey);
    CHECK_EQ(eng.color_timer(), 0u);

    eng.on_statistics_update(kLoud);
    eng.on_statistics_update(kLoud);
    eng.on_statistics_update(kLoud);
    CHECK(eng.audio_enabled());
    CHECK(eng.color() == BigDisplayColor::Green);
}

TEST(scanner_user_resume_arms_the_browse_timer) {
    auto eng = make_engine({100, 200, 300});
    eng.user_pause();
    eng.user_resume();
    CHECK(!eng.userpause());
    /* browse_wait(5) * 10 + 1 */
    CHECK_EQ(eng.browse_timer(), 51u);

    /* The next update sees browse_timer past the limit and resumes. */
    eng.on_statistics_update(kQuiet);
    CHECK(eng.stepper().is_scanning());
    CHECK_EQ(eng.browse_timer(), 0u);
}

TEST(scanner_encoder_restarts_the_browse_timer_to_one) {
    /* Upstream restarts it to 1, not 0: a zero browse_timer means "never
     * paused" to on_statistics_update(), so zeroing it would change the state
     * machine's meaning rather than restart the dwell. */
    auto eng = make_engine({100, 200, 300});

    eng.restart_browse_timer();
    CHECK_EQ(eng.browse_timer(), 0u);  /* not paused: left alone */

    for (int i = 0; i < 11; i++) eng.on_statistics_update(kLoud);
    CHECK_EQ(eng.browse_timer(), 1u);

    for (int i = 0; i < 5; i++) eng.on_statistics_update(kLoud);
    CHECK_EQ(eng.browse_timer(), 6u);

    eng.restart_browse_timer();
    CHECK_EQ(eng.browse_timer(), 1u);
}

TEST(scanner_retune_colour_follows_freq_lock) {
    auto eng = make_engine({100, 200, 300});

    eng.stepper().set_freq_lock(0);
    CHECK(eng.retune_color() == BigDisplayColor::Grey);

    eng.stepper().set_freq_lock(1);
    CHECK(eng.retune_color() == BigDisplayColor::Yellow);

    eng.stepper().set_freq_lock(5);
    CHECK(eng.retune_color() == BigDisplayColor::Keep);

    eng.stepper().set_freq_lock(app::scanner::kMaxFreqLock);
    CHECK(eng.retune_color() == BigDisplayColor::Green);
}

TEST(scanner_resume_nudges_the_index_in_the_scan_direction) {
    auto eng = make_engine({100, 200, 300});
    eng.stepper().set_scanning_direction(false);
    eng.scan_pause();
    eng.scan_resume();
    CHECK_EQ(eng.stepper().index_stepper(), -1);
    CHECK(eng.stepper().is_scanning());
}

/* ======================================================================== *
 *  Signal Hunter — energy detector                                          *
 * ======================================================================== */

TEST(hunter_sample_energy_matches_upstream_shift) {
    using app::signal_hunter::sample_energy;
    /* ((I*I + Q*Q) >> 16), int32 accumulator. */
    CHECK_EQ(sample_energy(0, 0), 0u);
    CHECK_EQ(sample_energy(256, 0), 1u);            /* 65536 >> 16 */
    CHECK_EQ(sample_energy(8192, 0), 1024u);        /* 67108864 >> 16 */
    CHECK_EQ(sample_energy(32767, 0), 16383u);      /* 1073676289 >> 16 */
    /* 2 * 32767^2 = 2147352578, which still fits in int32 (max 2147483647) —
     * this is the case upstream's int32 accumulator is sized for. */
    CHECK_EQ(sample_energy(32767, 32767), 32766u);  /* 2147352578 >> 16 */
    CHECK_EQ(sample_energy(-8192, 0), 1024u);       /* sign does not matter */
    CHECK_EQ(sample_energy(100, 100), 0u);          /* 20000 >> 16 rounds to 0 */
}

TEST(hunter_hangtime_conversion_matches_upstream) {
    using app::signal_hunter::hangtime_to_samples;
    /* 1 ms = 250 samples at the 250 kHz post-decimation rate. */
    CHECK_EQ(hangtime_to_samples(500, 250), 125000u);
    CHECK_EQ(hangtime_to_samples(1, 250), 250u);
    /* Upstream's guard against the `--hangtime_counter == 0` underflow. */
    CHECK_EQ(hangtime_to_samples(0, 250), 1u);
    CHECK_EQ(hangtime_to_samples(0, 0), 1u);
}

TEST(hunter_window_average_is_the_mean_of_64_samples) {
    HuntDetector d;
    d.configure(/*threshold*/ 1u << 30, /*hangtime*/ 10);
    d.set_hunting(false);

    /* Energy 1024 per sample; the window fills from zero. */
    for (int i = 1; i <= 32; i++) d.push(int16_t{8192}, int16_t{0});
    CHECK_EQ(d.average(), 512u);  /* 32 * 1024 / 64 */

    for (int i = 33; i <= 64; i++) d.push(int16_t{8192}, int16_t{0});
    CHECK_EQ(d.average(), 1024u);
}

TEST(hunter_does_not_trigger_unless_hunting) {
    HuntDetector d;
    d.configure(500, 10);
    d.set_hunting(false);

    for (int i = 0; i < 64; i++) {
        const auto ev = d.push(int16_t{8192}, int16_t{0});
        CHECK(!ev.trigger);
    }
    CHECK(d.state() == HuntState::Idle);
}

TEST(hunter_full_trigger_hangtime_stop_cycle) {
    HuntDetector d;
    d.configure(/*threshold*/ 500, /*hangtime samples*/ 10);
    d.set_hunting(true);
    CHECK_EQ(d.hangtime_samples(), 10u);

    /* avg after k loud samples is k * 1024 / 64 = 16k, so it first exceeds 500
     * at k = 32 (512). */
    int trigger_at = 0;
    for (int k = 1; k <= 64; k++) {
        const auto ev = d.push(int16_t{8192}, int16_t{0});
        if (ev.trigger && trigger_at == 0) {
            trigger_at = k;
            CHECK_EQ(ev.energy, 512u);
        }
    }
    CHECK_EQ(trigger_at, 32);
    CHECK(d.state() == HuntState::AwaitingStream);

    /* M0 opened the capture. */
    d.begin_recording();
    CHECK(d.state() == HuntState::Recording);

    /* Quiet samples: avg = (64 - k) * 16, which first drops below 500 at k = 33
     * (496), entering HANGTIME. */
    int quiet = 0;
    while (d.state() == HuntState::Recording && quiet < 200) {
        d.push(int16_t{0}, int16_t{0});
        quiet++;
    }
    CHECK_EQ(quiet, 33);
    CHECK(d.state() == HuntState::Hangtime);
    CHECK_EQ(d.hangtime_counter(), 10u);

    /* The counter is pre-decremented, so the stop lands on the 10th sample. */
    int held = 0;
    bool stopped = false;
    while (!stopped && held < 100) {
        const auto ev = d.push(int16_t{0}, int16_t{0});
        held++;
        stopped = ev.stop;
    }
    CHECK_EQ(held, 10);
    CHECK(d.state() == HuntState::AwaitingClose);

    /* AWAITING_CLOSE never self-transitions — only the capture teardown does. */
    for (int i = 0; i < 50; i++) {
        const auto ev = d.push(int16_t{8192}, int16_t{0});
        CHECK(!ev.trigger);
        CHECK(!ev.stop);
    }
    CHECK(d.state() == HuntState::AwaitingClose);

    d.end_recording();
    CHECK(d.state() == HuntState::Idle);
}

TEST(hunter_signal_returning_during_hangtime_resumes_recording) {
    HuntDetector d;
    d.configure(500, 1000);
    d.set_hunting(true);

    for (int k = 0; k < 64; k++) d.push(int16_t{8192}, int16_t{0});
    d.begin_recording();

    while (d.state() == HuntState::Recording) d.push(int16_t{0}, int16_t{0});
    CHECK(d.state() == HuntState::Hangtime);

    /* Refill the window with loud samples; once the mean climbs back over the
     * threshold the state returns to RECORDING and no stop is emitted. */
    bool back = false;
    for (int k = 0; k < 64 && !back; k++) {
        const auto ev = d.push(int16_t{8192}, int16_t{0});
        CHECK(!ev.stop);
        back = (d.state() == HuntState::Recording);
    }
    CHECK(back);
}

TEST(hunter_configure_clamps_zero_hangtime) {
    HuntDetector d;
    d.configure(500, 0);
    CHECK_EQ(d.hangtime_samples(), 1u);
}

TEST(hunter_preroll_ring_drains_oldest_first) {
    app::signal_hunter::PreRollRing ring{4};
    ring.clear();

    ring.push(1.0f, 0.0f);
    ring.push(2.0f, 0.0f);
    ring.push(3.0f, 0.0f);

    std::vector<float> out;
    ring.drain(out);
    CHECK_EQ(out.size(), static_cast<size_t>(8));
    /* Slot 3 has not been written yet, so the oldest sample is the zero. */
    CHECK_NEAR(out[0], 0.0f, 1e-6);
    CHECK_NEAR(out[2], 1.0f, 1e-6);
    CHECK_NEAR(out[4], 2.0f, 1e-6);
    CHECK_NEAR(out[6], 3.0f, 1e-6);

    /* Wrap: 5 written into a 4-slot ring keeps 2,3,4,5 oldest-first. */
    ring.push(4.0f, 0.0f);
    ring.push(5.0f, 0.0f);
    ring.drain(out);
    CHECK_NEAR(out[0], 2.0f, 1e-6);
    CHECK_NEAR(out[2], 3.0f, 1e-6);
    CHECK_NEAR(out[4], 4.0f, 1e-6);
    CHECK_NEAR(out[6], 5.0f, 1e-6);
}

/* ======================================================================== *
 *  Signal Hunter — FFT peak search (host addition)                          *
 * ======================================================================== */

TEST(hunter_bin_to_frequency_places_dc_at_the_centre) {
    using app::signal_hunter::bin_to_frequency;
    const int64_t center = 433'920'000;
    const double span = 2'000'000.0;
    const int n = 256;
    const double bw = span / n;  /* 7812.5 Hz */

    CHECK_EQ(bin_to_frequency(center, 128, span, n), center);
    CHECK_EQ(bin_to_frequency(center, 148, span, n),
             center + static_cast<int64_t>(bw * 20));
    CHECK_EQ(bin_to_frequency(center, 98, span, n),
             center + static_cast<int64_t>(bw * -30));
    /* Degenerate bin count returns the centre rather than dividing by zero. */
    CHECK_EQ(bin_to_frequency(center, 5, span, 0), center);
}

TEST(hunter_fft_finds_a_synthetic_tone) {
    /* A unit complex sinusoid at exactly bin k0 lands in one FFT bin. dsp::Fft
     * emits -Fs/2..+Fs/2, so bin k0 appears at index N/2 + k0. */
    constexpr int n = 256;
    dsp::Fft fft{n};
    const auto window = dsp::make_window(dsp::WindowType::Rectangular, n);

    const int k0 = 20;
    std::vector<dsp::cfloat> in(n);
    for (int i = 0; i < n; i++) {
        const double a = 2.0 * 3.14159265358979323846 * k0 * i / n;
        in[static_cast<size_t>(i)] =
            dsp::cfloat{static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    }

    std::vector<float> db;
    fft.spectrum_db(in.data(), window, db);
    CHECK_EQ(db.size(), static_cast<size_t>(n));

    const auto peak = app::signal_hunter::find_peak_bin(db);
    CHECK_EQ(peak.bin, n / 2 + k0);
    CHECK_NEAR(peak.db, 0.0f, 0.1f);  /* unit amplitude reads 0 dBFS */

    /* And it maps back to the right absolute frequency. */
    const int64_t center = 433'920'000;
    const double span = 2'000'000.0;
    const int64_t hz = app::signal_hunter::bin_to_frequency(center, peak.bin, span, n);
    CHECK_EQ(hz, center + static_cast<int64_t>((span / n) * k0));
}

TEST(hunter_fft_finds_a_negative_frequency_tone) {
    constexpr int n = 256;
    dsp::Fft fft{n};
    const auto window = dsp::make_window(dsp::WindowType::Rectangular, n);

    const int k0 = -30;
    std::vector<dsp::cfloat> in(n);
    for (int i = 0; i < n; i++) {
        const double a = 2.0 * 3.14159265358979323846 * k0 * i / n;
        in[static_cast<size_t>(i)] =
            dsp::cfloat{static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    }

    std::vector<float> db;
    fft.spectrum_db(in.data(), window, db);

    const auto peak = app::signal_hunter::find_peak_bin(db);
    CHECK_EQ(peak.bin, n / 2 + k0);
}

TEST(hunter_fft_peak_skips_the_dc_spike) {
    /* A big DC term (the AD936x LO leakage) plus a weak tone: the guard band
     * around DC must keep the tone as the reported peak. */
    constexpr int n = 256;
    dsp::Fft fft{n};
    const auto window = dsp::make_window(dsp::WindowType::Rectangular, n);

    const int k0 = 40;
    std::vector<dsp::cfloat> in(n);
    for (int i = 0; i < n; i++) {
        const double a = 2.0 * 3.14159265358979323846 * k0 * i / n;
        in[static_cast<size_t>(i)] =
            dsp::cfloat{1.0f + 0.1f * static_cast<float>(std::cos(a)),
                        0.1f * static_cast<float>(std::sin(a))};
    }

    std::vector<float> db;
    fft.spectrum_db(in.data(), window, db);

    /* DC really is the strongest bin. */
    CHECK(db[static_cast<size_t>(n / 2)] > db[static_cast<size_t>(n / 2 + k0)]);

    const auto peak = app::signal_hunter::find_peak_bin(db);
    CHECK_EQ(peak.bin, n / 2 + k0);
    CHECK_NEAR(peak.db, -20.0f, 0.5f);  /* 0.1 amplitude */
}

TEST(hunter_fft_peak_handles_empty_input) {
    const std::vector<float> empty;
    const auto peak = app::signal_hunter::find_peak_bin(empty);
    CHECK_EQ(peak.bin, -1);
}

/* ======================================================================== *
 *  Detector RX                                                              *
 * ======================================================================== */

TEST(detector_map_matches_upstream_integer_map) {
    using app::detector_rx::map_range;
    /* toLow + (value - fromLow) * (toHigh - toLow) / (fromHigh - fromLow) */
    CHECK_EQ(map_range(-100, -100, 20, 400, 2600), 400);
    CHECK_EQ(map_range(20, -100, 20, 400, 2600), 2600);
    CHECK_EQ(map_range(-40, -100, 20, 400, 2600), 1500);
    CHECK_EQ(map_range(0, -100, 20, 400, 2600), 2233);  /* integer division */
    /* Host guard: a zero-width input range would divide by zero on x86. */
    CHECK_EQ(map_range(5, 10, 10, 400, 2600), 400);
}

TEST(detector_beep_pitch_mapping) {
    using app::detector_rx::beep_frequency;
    CHECK_EQ(beep_frequency(-100), 400);
    CHECK_EQ(beep_frequency(20), 2600);
    CHECK_EQ(beep_frequency(-40), 1500);
}

TEST(detector_format_freq_mhz) {
    using app::detector_rx::format_freq_mhz;
    CHECK_STR_EQ(format_freq_mhz(433'920'000), "< 433.920 MHz >");
    CHECK_STR_EQ(format_freq_mhz(100'000'000), "< 100.000 MHz >");
    CHECK_STR_EQ(format_freq_mhz(1'234'567), "< 1.234 MHz >");
}

TEST(detector_range_scan_steps_by_detector_bw_and_wraps) {
    DetectorScanner s;
    CHECK(s.set_list(make_db({make_range(100'000'000, 101'000'000)})));
    s.set_auto_scan(true);
    s.set_auto_advance(false);

    /* No s= on the entry, so the step is DETECTOR_BW. */
    CHECK_EQ(s.step(), app::detector_rx::kDetectorBw);
    CHECK_EQ(s.frequency(), static_cast<int64_t>(100'000'000));

    CHECK(s.on_timer());
    CHECK_EQ(s.frequency(), static_cast<int64_t>(100'750'000));

    /* 101.5 MHz is past max, and without AUTOADV it wraps to min. */
    CHECK(s.on_timer());
    CHECK_EQ(s.frequency(), static_cast<int64_t>(100'000'000));
}

TEST(detector_entry_step_overrides_detector_bw) {
    const auto step25k = step_index_for(25'000);
    CHECK(core::is_valid(step25k));

    DetectorScanner s;
    CHECK(s.set_list(make_db({make_range(100'000'000, 100'100'000, step25k)})));
    CHECK_EQ(s.step(), 25'000);

    CHECK(s.on_timer());
    CHECK_EQ(s.frequency(), static_cast<int64_t>(100'025'000));
}

TEST(detector_auto_advance_moves_to_the_next_entry) {
    DetectorScanner s;
    CHECK(s.set_list(make_db({make_range(100'000'000, 101'000'000),
                              make_single(200'000'000, "single")})));
    s.set_auto_scan(true);
    s.set_auto_advance(true);

    CHECK_EQ(s.current_index(), static_cast<size_t>(0));
    CHECK(s.on_timer());
    CHECK_EQ(s.frequency(), static_cast<int64_t>(100'750'000));

    /* Past the top of the range with AUTOADV: jump to entry 1. */
    CHECK(s.on_timer());
    CHECK_EQ(s.current_index(), static_cast<size_t>(1));
    CHECK_EQ(s.frequency(), static_cast<int64_t>(200'000'000));
    CHECK_EQ(s.min_frequency(), s.max_frequency());

    /* A single-frequency entry advances every tick under AUTOADV. */
    CHECK(s.on_timer());
    CHECK_EQ(s.current_index(), static_cast<size_t>(0));
    CHECK_EQ(s.frequency(), static_cast<int64_t>(100'000'000));
}

TEST(detector_single_entry_holds_without_auto_advance) {
    DetectorScanner s;
    CHECK(s.set_list(make_db({make_single(433'920'000)})));
    s.set_auto_scan(true);
    s.set_auto_advance(false);

    CHECK(!s.on_timer());
    CHECK_EQ(s.frequency(), static_cast<int64_t>(433'920'000));
}

TEST(detector_auto_scan_off_freezes_the_cursor) {
    DetectorScanner s;
    CHECK(s.set_list(make_db({make_range(100'000'000, 101'000'000)})));
    s.set_auto_scan(false);

    CHECK(!s.on_timer());
    CHECK_EQ(s.frequency(), static_cast<int64_t>(100'000'000));
}

TEST(detector_empty_list_is_reported_and_inert) {
    DetectorScanner s;
    CHECK(!s.set_list(core::freqman_db{}));
    CHECK(s.empty());
    CHECK(!s.on_timer());
    CHECK(!s.step_index(1));
    CHECK(!s.step_frequency(1));
}

TEST(detector_index_encoder_wraps_both_ways) {
    DetectorScanner s;
    CHECK(s.set_list(make_db({make_single(100'000'000), make_single(200'000'000),
                              make_single(300'000'000)})));

    CHECK(s.step_index(-1));
    CHECK_EQ(s.current_index(), static_cast<size_t>(2));
    CHECK_EQ(s.frequency(), static_cast<int64_t>(300'000'000));

    CHECK(s.step_index(1));
    CHECK_EQ(s.current_index(), static_cast<size_t>(0));

    CHECK(!s.step_index(0));
}

TEST(detector_frequency_encoder_wraps_inside_a_range) {
    DetectorScanner s;
    CHECK(s.set_list(make_db({make_range(100'000'000, 101'000'000)})));

    /* Down from the bottom wraps to the top of the range. */
    CHECK(s.step_frequency(-1));
    CHECK_EQ(s.frequency(), static_cast<int64_t>(101'000'000));

    /* Up from the top wraps back to the bottom. */
    CHECK(s.step_frequency(1));
    CHECK_EQ(s.frequency(), static_cast<int64_t>(100'000'000));

    CHECK(s.step_frequency(1));
    CHECK_EQ(s.frequency(), static_cast<int64_t>(100'750'000));
}

TEST(detector_frequency_encoder_is_a_noop_on_a_single_entry) {
    DetectorScanner s;
    CHECK(s.set_list(make_db({make_single(433'920'000)})));
    CHECK(!s.step_frequency(1));
    CHECK_EQ(s.frequency(), static_cast<int64_t>(433'920'000));
}

TEST(detector_level_history_min_avg_max) {
    LevelHistory h{3};
    CHECK(h.empty());
    CHECK_NEAR(h.avg(), 0.0f, 1e-6);

    h.add(-10.0f);
    h.add(-20.0f);
    h.add(-30.0f);
    CHECK_EQ(h.size(), static_cast<size_t>(3));
    CHECK_NEAR(h.min(), -30.0f, 1e-6);
    CHECK_NEAR(h.max(), -10.0f, 1e-6);
    CHECK_NEAR(h.avg(), -20.0f, 1e-6);

    /* Bounded: the oldest value drops off. */
    h.add(-40.0f);
    CHECK_EQ(h.size(), static_cast<size_t>(3));
    CHECK_NEAR(h.min(), -40.0f, 1e-6);
    CHECK_NEAR(h.max(), -20.0f, 1e-6);
    CHECK_NEAR(h.avg(), -30.0f, 1e-6);
}

/* ======================================================================== *
 *  Fox hunt — RSSI smoothing, peak hold, trend, threshold                   *
 * ======================================================================== */

TEST(foxhunt_first_reading_seeds_the_filter) {
    FoxhuntEngine e;
    e.set_smoothing_alpha(0.25f);
    CHECK(!e.primed());

    CHECK_NEAR(e.update(-73.0f), -73.0f, 1e-5);
    CHECK(e.primed());
    CHECK_NEAR(e.smoothed(), -73.0f, 1e-5);
    CHECK_NEAR(e.peak(), -73.0f, 1e-5);
}

TEST(foxhunt_ema_matches_the_closed_form) {
    /* s[n] = s[n-1] + alpha * (x[n] - s[n-1]), s[0] = x[0]. */
    FoxhuntEngine e;
    e.set_smoothing_alpha(0.5f);
    e.set_peak_decay_db(0.0f);

    CHECK_NEAR(e.update(-60.0f), -60.0f, 1e-5);
    CHECK_NEAR(e.update(-40.0f), -50.0f, 1e-5);
    CHECK_NEAR(e.update(-40.0f), -45.0f, 1e-5);
    CHECK_NEAR(e.update(-40.0f), -42.5f, 1e-5);
}

TEST(foxhunt_alpha_is_clamped) {
    FoxhuntEngine e;
    e.set_smoothing_alpha(5.0f);
    CHECK_NEAR(e.smoothing_alpha(), 1.0f, 1e-6);
    e.set_smoothing_alpha(-1.0f);
    CHECK_NEAR(e.smoothing_alpha(), 0.001f, 1e-6);

    /* alpha == 1 tracks the input exactly. */
    e.set_smoothing_alpha(1.0f);
    e.update(-90.0f);
    CHECK_NEAR(e.update(-30.0f), -30.0f, 1e-5);
}

TEST(foxhunt_peak_rises_instantly_and_decays) {
    FoxhuntEngine e;
    e.set_smoothing_alpha(1.0f);
    e.set_peak_decay_db(1.0f);

    e.update(-40.0f);
    CHECK_NEAR(e.peak(), -40.0f, 1e-5);

    /* A weaker reading leaves the peak behind, decaying one dB per update. */
    e.update(-80.0f);
    CHECK_NEAR(e.peak(), -41.0f, 1e-5);
    e.update(-80.0f);
    CHECK_NEAR(e.peak(), -42.0f, 1e-5);
    CHECK_NEAR(e.below_peak_db(), 38.0f, 1e-5);

    /* A stronger reading takes the peak with it immediately. */
    e.update(-20.0f);
    CHECK_NEAR(e.peak(), -20.0f, 1e-5);
    CHECK_NEAR(e.below_peak_db(), 0.0f, 1e-5);
}

TEST(foxhunt_detect_threshold_has_hysteresis) {
    FoxhuntEngine e;
    e.set_smoothing_alpha(1.0f);
    e.set_threshold_db(-50.0f);
    e.set_hysteresis_db(3.0f);

    e.update(-60.0f);
    CHECK(!e.detected());

    e.update(-45.0f);
    CHECK(e.detected());

    /* Inside the hysteresis band the indicator holds. */
    e.update(-51.0f);
    CHECK(e.detected());
    e.update(-52.9f);
    CHECK(e.detected());

    /* Below threshold - hysteresis it releases. */
    e.update(-54.0f);
    CHECK(!e.detected());

    /* Exactly at the threshold does not latch (strictly greater). */
    e.update(-50.0f);
    CHECK(!e.detected());
}

TEST(foxhunt_trend_classifies_closer_steady_farther) {
    FoxhuntEngine e;
    e.set_smoothing_alpha(1.0f);
    e.set_trend_window(2);
    e.set_trend_deadband_db(1.0f);
    e.set_peak_decay_db(0.0f);

    /* Not enough history yet. */
    e.update(-60.0f);
    CHECK(e.trend() == Trend::Unknown);
    e.update(-60.0f);
    CHECK(e.trend() == Trend::Unknown);

    /* Reference is the reading two updates ago: -60 -> -55 is +5 dB. */
    e.update(-55.0f);
    CHECK(e.trend() == Trend::Closer);

    e.update(-55.0f);
    CHECK(e.trend() == Trend::Closer);  /* reference is still -60 */

    e.update(-55.0f);
    CHECK(e.trend() == Trend::Steady);  /* reference has caught up */

    e.update(-70.0f);
    CHECK(e.trend() == Trend::Farther);
}

TEST(foxhunt_trend_deadband_suppresses_small_moves) {
    FoxhuntEngine e;
    e.set_smoothing_alpha(1.0f);
    e.set_trend_window(1);
    e.set_trend_deadband_db(2.0f);

    e.update(-60.0f);
    e.update(-59.0f);  /* +1 dB, inside the 2 dB dead band */
    CHECK(e.trend() == Trend::Steady);

    e.update(-56.0f);  /* +3 dB against the previous reading */
    CHECK(e.trend() == Trend::Closer);
}

TEST(foxhunt_reset_clears_the_filter) {
    FoxhuntEngine e;
    e.set_smoothing_alpha(1.0f);
    e.update(-30.0f);
    e.update(-30.0f);
    CHECK(e.primed());
    CHECK_EQ(e.updates(), 2u);

    e.reset();
    CHECK(!e.primed());
    CHECK_EQ(e.updates(), 0u);
    CHECK(e.trend() == Trend::Unknown);
    CHECK(!e.detected());

    /* And it re-seeds on the next reading rather than ramping from zero. */
    CHECK_NEAR(e.update(-77.0f), -77.0f, 1e-5);
}

TEST(foxhunt_bearing_log_picks_the_strongest_mark) {
    BearingLog log;
    CHECK(log.empty());
    CHECK_EQ(log.best_index(), BearingLog::max_marks);

    log.add(0, -70.0f);
    log.add(90, -55.0f);
    log.add(180, -80.0f);

    CHECK_EQ(log.size(), static_cast<size_t>(3));
    CHECK_EQ(log.best().degrees, static_cast<uint16_t>(90));
    CHECK_NEAR(log.best().level_db, -55.0f, 1e-5);
}

TEST(foxhunt_bearing_log_keeps_the_earlier_of_equal_marks) {
    BearingLog log;
    log.add(45, -50.0f);
    log.add(225, -50.0f);
    CHECK_EQ(log.best_index(), static_cast<size_t>(0));
    CHECK_EQ(log.best().degrees, static_cast<uint16_t>(45));
}

TEST(foxhunt_bearing_log_wraps_degrees_and_is_bounded) {
    BearingLog log;
    log.add(370, -60.0f);
    CHECK_EQ(log.marks()[0].degrees, static_cast<uint16_t>(10));
    log.add(720, -60.0f);
    CHECK_EQ(log.marks()[1].degrees, static_cast<uint16_t>(0));

    log.clear();
    CHECK(log.empty());

    for (uint16_t i = 0; i < BearingLog::max_marks + 5; i++)
        log.add(i, static_cast<float>(-100 + i));
    CHECK_EQ(log.size(), BearingLog::max_marks);
    /* The oldest five were dropped, so the first mark is bearing 5. */
    CHECK_EQ(log.marks().front().degrees, static_cast<uint16_t>(5));
    /* The strongest is the newest. */
    CHECK_EQ(log.best().degrees,
             static_cast<uint16_t>(BearingLog::max_marks + 4));
}
