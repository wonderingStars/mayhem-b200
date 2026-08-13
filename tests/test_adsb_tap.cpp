/*
 * mayhem-b200 — ADS-B on the gapless tap, at a capability-chosen sample rate.
 *
 * Three things changed in src/apps/ui_adsb_rx.{hpp,cpp} and each is a claim
 * that can be wrong in a way no compiler catches, so each is measured here
 * rather than asserted from the shape of the code:
 *
 *  1. The app drains radio::RawSampleTap instead of polling the 4096-sample
 *     spectrum snapshot, so the demodulator now keeps its state across calls
 *     and a frame straddling two UI frames decodes. The risk that buys is the
 *     opposite one: when the tap DOES lose samples, carrying that state across
 *     the hole is worse than useless. Both halves are tested — that a
 *     contiguous split costs nothing, and that a gap without a reset really
 *     does swallow the next good frame (which is why pump() resets on one).
 *
 *  2. The rate comes from radio::choose_rx_rate() against the connected
 *     device's caps. Tested against device profiles nobody here owns.
 *
 *  3. Raising the rate is only a gain if the extra samples are INTEGRATED
 *     rather than thrown away. ReceiverModel opens the analog front end to
 *     0.8 * the sample rate, so 8 Msps admits four times the noise power of
 *     2 Msps; subsampling back down keeps all of it. The decimator that makes
 *     the higher rate pay is checked by measuring the signal level each path
 *     needs to decode, on the same waveform and the same noise realisation.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_context.hpp"
#include "audio_out.hpp"
#include "receiver_model.hpp"
#include "ui_adsb_rx.hpp"
#include "ui_navigation.hpp"
#include "usrp_radio.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace {

/* A real DF17 extended squitter, the same vector tests/test_adsb.cpp uses. */
const char* kFrame = "8D40621D58C382D690C8AC2863A7";

std::vector<uint8_t> bytes_from_hex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

std::string hex_of(const app::adsb::AdsbFrame& f, size_t bytes) {
    static const char* digits = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < bytes; i++) {
        s.push_back(digits[(f.get_raw_data()[i] >> 4) & 0xF]);
        s.push_back(digits[f.get_raw_data()[i] & 0xF]);
    }
    return s;
}

/* The 0/1 envelope of a Mode S burst at the native 2 Msps: 8 us of preamble
 * with 0.5 us pulses at 0, 1.0, 3.5 and 4.5 us, then one bit per microsecond
 * with the energy in the first half for a 1 and the second half for a 0. */
std::vector<float> ppm_envelope(const std::vector<uint8_t>& bytes,
                                size_t bit_count,
                                size_t lead = 64,
                                size_t tail = 64) {
    static const int preamble[16] = {1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0};

    std::vector<float> out(lead, 0.0f);
    for (int i = 0; i < 16; i++) out.push_back(preamble[i] ? 1.0f : 0.0f);

    for (size_t b = 0; b < bit_count; b++) {
        const bool bit = ((bytes[b >> 3] >> (7 - (b & 7))) & 1) != 0;
        out.push_back(bit ? 1.0f : 0.0f);
        out.push_back(bit ? 0.0f : 1.0f);
    }

    out.insert(out.end(), tail, 0.0f);
    return out;
}

/* The envelope carried on complex baseband at `factor` times the native rate,
 * each native sample held for `factor` fast samples. Holding rather than
 * band-limiting is deliberate: it makes the signal IDENTICAL under both
 * reductions being compared (subsampling picks one of the four, averaging
 * takes their mean, and the four are equal), so the only difference the noise
 * test can measure is what happens to the NOISE.
 *
 * `noise_rms` is the per-component sigma AT THE FAST RATE, which is how the
 * radio delivers it: the front end is open to 0.8 * the sample rate, so four
 * times the rate carries four times the noise power per sample. */
std::vector<dsp::cfloat> fast_burst(const std::vector<float>& envelope,
                                    size_t factor,
                                    float signal,
                                    float noise_rms,
                                    uint32_t seed) {
    std::mt19937 rng{seed};
    std::normal_distribution<float> noise{0.0f, noise_rms};

    std::vector<dsp::cfloat> out;
    out.reserve(envelope.size() * factor);

    for (float e : envelope) {
        for (size_t k = 0; k < factor; k++) {
            const float re = (noise_rms > 0.0f ? noise(rng) : 0.0f) + (signal * e);
            const float im = (noise_rms > 0.0f ? noise(rng) : 0.0f);
            out.emplace_back(re, im);
        }
    }

    return out;
}

/* Every `factor`-th sample, starting at the first: exactly what the magnitude
 * resampler does at an integer ratio, and therefore exactly what this app did
 * at a raised sample rate before the decimator existed. Subsampling the
 * complex stream and subsampling the magnitudes are the same thing — |s|^2 is
 * pointwise — so this reproduces the old path without needing it to still be
 * reachable. */
std::vector<dsp::cfloat> subsampled(const std::vector<dsp::cfloat>& in, size_t factor) {
    std::vector<dsp::cfloat> out;
    out.reserve((in.size() / factor) + 1);
    for (size_t i = 0; i < in.size(); i += factor) out.push_back(in[i]);
    return out;
}

struct Capture {
    std::vector<app::adsb::AdsbFrame> frames;
    std::vector<float> amps;

    app::adsb::AdsbDemod::FrameHandler handler() {
        return [this](const app::adsb::AdsbFrame& f, float amp) {
            frames.push_back(f);
            amps.push_back(amp);
        };
    }

    /* A frame is only "decoded" if the parity checks AND the bytes are the
     * ones that were transmitted. Preambles fire on noise all the time — that
     * is what the CRC is for — so counting callbacks would count nothing. */
    bool got(const char* expected) const {
        for (const auto& f : frames)
            if (f.check_CRC() == 0u && hex_of(f, 14) == std::string{expected}) return true;
        return false;
    }

    /* Amplitude reported alongside the first valid frame matching `expected`,
     * or -1 if it was never decoded. The amplitude is the sum of the four
     * preamble magnitudes, so it is the cheapest observable that moves when
     * the decimator's sampling phase moves. */
    float amp_of(const char* expected) const {
        for (size_t i = 0; i < frames.size(); i++)
            if (frames[i].check_CRC() == 0u &&
                hex_of(frames[i], 14) == std::string{expected})
                return amps[i];
        return -1.0f;
    }
};

/* One trial: does this burst decode, reduced to the native rate the given
 * way? Both paths see the same samples and the same noise realisation. */
bool decodes_via_decimator(const std::vector<dsp::cfloat>& fast, double fast_rate) {
    app::adsb::AdsbDemod demod;
    demod.set_input_rate(fast_rate);

    Capture cap;
    demod.process(fast.data(), fast.size(), cap.handler());
    return cap.got(kFrame);
}

bool decodes_via_subsampling(const std::vector<dsp::cfloat>& fast, size_t factor) {
    const auto thin = subsampled(fast, factor);

    app::adsb::AdsbDemod demod;
    demod.set_input_rate(app::adsb::AdsbDemod::kNativeSampleRate);

    Capture cap;
    demod.process(thin.data(), thin.size(), cap.handler());
    return cap.got(kFrame);
}

/* The lowest signal amplitude on a fixed ladder at which a path decodes at
 * least `need` of `trials` bursts. Returns 0 if it never does. A ladder rather
 * than a single level because the quantity under test is a RATIO of two
 * sensitivities, and one pass/fail at one hand-picked level would only pin the
 * level. */
constexpr size_t kLadderSteps = 16;
constexpr double kLadderBase = 0.75;
constexpr double kLadderRatio = 1.25;

double sensitivity_threshold(bool (*decode)(const std::vector<dsp::cfloat>&, double),
                             double arg,
                             size_t factor,
                             float noise_rms,
                             size_t trials,
                             size_t need) {
    const auto envelope = ppm_envelope(bytes_from_hex(kFrame), 112);

    double level = kLadderBase;
    for (size_t step = 0; step < kLadderSteps; step++) {
        size_t hits = 0;
        for (size_t t = 0; t < trials; t++) {
            const auto fast = fast_burst(envelope, factor, static_cast<float>(level),
                                         noise_rms, static_cast<uint32_t>(1000 + t));
            if (decode(fast, arg)) hits++;
        }
        if (hits >= need) return level;
        level *= kLadderRatio;
    }
    return 0.0;
}

bool decode_decimator_8m(const std::vector<dsp::cfloat>& fast, double) {
    return decodes_via_decimator(fast, 8'000'000.0);
}

bool decode_subsample_4(const std::vector<dsp::cfloat>& fast, double) {
    return decodes_via_subsampling(fast, 4);
}

/* --- Device profiles ------------------------------------------------------ */

radio::DeviceCaps caps_with_rx_rate(double min_hz, double max_hz, double step_hz = 0.0) {
    radio::DeviceCaps c;
    c.rx_rate = {min_hz, max_hz, step_hz};
    return c;
}

/* --- A live view over a receiver with no hardware behind it ---------------- */

/* The app under test, on the navigation stack, with globals() wired to a
 * ReceiverModel whose radio is closed. Nothing streams: ReceiverModel::start()
 * fails at start_rx() and never spawns a DSP thread, so the test itself plays
 * the part of the producer and writes into the tap by hand. That is the whole
 * point — it makes the tap's timing deterministic, which a real radio's never
 * is, so "a gap happened here" can be arranged exactly.
 *
 * A closed UsrpRadio still reports the published B200 caps (usrp_radio.cpp's
 * default_b200_caps), and clamps a rate request against them, so the rate the
 * view chooses here is the one it would choose on a connected B200. */
struct ViewHarness {
    radio::UsrpRadio radio{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{radio, audio};
    ui::NavigationView nav{{0, 0, 240, 304}};

    radio::RadioDevice* saved_radio{app::globals().radio};
    radio::ReceiverModel* saved_receiver{app::globals().receiver};
    ui::NavigationView* saved_nav{app::globals().nav};

    app::AdsbRxView* view{nullptr};

    ViewHarness() {
        app::globals().radio = &radio;
        app::globals().receiver = &receiver;
        app::globals().nav = &nav;

        auto owned = std::make_unique<app::AdsbRxView>();
        view = owned.get();
        nav.push(std::move(owned));
        nav.service(); /* applies the push, which calls on_show() */
    }

    ~ViewHarness() {
        /* Globals go back first; the view is destroyed with `nav` below, and
         * its destructor uses its own stored receiver pointer, not these. */
        app::globals().radio = saved_radio;
        app::globals().receiver = saved_receiver;
        app::globals().nav = saved_nav;
    }

    ViewHarness(const ViewHarness&) = delete;
    ViewHarness& operator=(const ViewHarness&) = delete;

    /* Stand in for the DSP thread. */
    void produce(const std::vector<dsp::cfloat>& block) {
        receiver.raw_tap().write(block.data(), block.size());
    }

    void produce_silence(size_t count) {
        static const std::vector<dsp::cfloat> zeros(4096, dsp::cfloat{0.0f, 0.0f});
        while (count != 0) {
            const size_t n = (count < zeros.size()) ? count : zeros.size();
            receiver.raw_tap().write(zeros.data(), n);
            count -= n;
        }
    }
};

/* A noiseless burst on the harness's stream, at whatever rate the view chose.
 * Noiseless so that detection is certain and the test measures only the tap
 * and reset behaviour it is about. */
std::vector<dsp::cfloat> clean_burst(double rate_hz, float signal, size_t bits = 112) {
    const size_t factor =
        static_cast<size_t>(rate_hz / app::adsb::AdsbDemod::kNativeSampleRate + 1e-9);
    const auto envelope = ppm_envelope(bytes_from_hex(kFrame), bits);
    return fast_burst(envelope, (factor == 0) ? 1 : factor, signal, 0.0f, 1);
}

}  // namespace

/* =========================================================================
 * 1. Rate selection
 * ========================================================================= */

TEST(adsb_rate_preference_states_what_mode_s_actually_needs) {
    const auto want = app::AdsbRxView::rate_preference();

    /* 1 Mbit/s PPM decoded two samples to the bit: below 2 Msps there is no
     * decoder, only the appearance of one. */
    CHECK_NEAR(want.minimum_hz, 2'000'000.0, 1.0);

    /* Eight samples per bit is the target, and also the point past which
     * nothing is bought — so the two are equal, and a 61.44 Msps device is not
     * asked for 61.44 Msps. */
    CHECK_NEAR(want.ideal_hz, 8'000'000.0, 1.0);
    CHECK_NEAR(want.max_useful_hz, 8'000'000.0, 1.0);

    /* Whole multiples of the native rate keep the decimator exact and let the
     * magnitude interpolator bypass. */
    CHECK_NEAR(want.prefer_multiple_of_hz, 2'000'000.0, 1.0);
}

TEST(adsb_rate_choice_across_device_profiles) {
    struct Profile {
        const char* name;
        radio::DeviceCaps caps;
        double expect_hz;
        radio::RateOutcome expect_outcome;
    };

    const Profile profiles[] = {
        /* The radio this port is built around. Its published range reaches
         * 61.44 Msps and it is deliberately NOT asked for it. */
        {"B200 published", caps_with_rx_rate(200e3, 61.44e6, 1.0), 8e6,
         radio::RateOutcome::Ideal},

        /* The same device as the hardware self-test actually measured it on a
         * USB 2 port. */
        {"B200 on USB 2", caps_with_rx_rate(200e3, 16e6, 1.0), 8e6,
         radio::RateOutcome::Ideal},

        /* An RTL-SDR tops out around 2.4 Msps. It can still decode — 2 Msps is
         * the minimum, not a courtesy — but it is a degraded rate and the
         * caller is told so. */
        {"RTL-SDR", caps_with_rx_rate(225e3, 2.4e6), 2e6, radio::RateOutcome::Reduced},

        /* HackRF's floor is 2 Msps, so its lowest rate is exactly the
         * demodulator's native one and its ceiling is far above what helps. */
        {"HackRF One", caps_with_rx_rate(2e6, 20e6), 8e6, radio::RateOutcome::Ideal},

        /* A device that cannot reach two samples per bit at all. It still gets
         * the closest rate it has — a spectrum is better than a blank screen —
         * but the outcome says the decoder cannot work. */
        {"1 Msps toy", caps_with_rx_rate(250e3, 1e6), 1e6,
         radio::RateOutcome::BelowMinimum},

        /* No radio attached: DeviceCaps{} carries no rx_rate at all, and the
         * app's own ideal comes back rather than zero. */
        {"nothing attached", radio::DeviceCaps{}, 8e6, radio::RateOutcome::CapsUnusable},
    };

    for (const auto& p : profiles) {
        const auto choice = radio::choose_rx_rate(p.caps, app::AdsbRxView::rate_preference());

        if (std::fabs(choice.rate_hz - p.expect_hz) > 1.0) {
            std::printf("  profile %s: got %.0f Hz, expected %.0f Hz\n", p.name,
                        choice.rate_hz, p.expect_hz);
        }
        CHECK_NEAR(choice.rate_hz, p.expect_hz, 1.0);

        if (choice.outcome != p.expect_outcome) {
            std::printf("  profile %s: outcome '%s', expected '%s'\n", p.name,
                        radio::rate_outcome_to_string(choice.outcome),
                        radio::rate_outcome_to_string(p.expect_outcome));
        }
        CHECK(choice.outcome == p.expect_outcome);

        /* Only the toy device is unusable, and it is the only one flagged. */
        CHECK_EQ(choice.usable(), p.expect_outcome != radio::RateOutcome::BelowMinimum);
    }
}

/* =========================================================================
 * 2. The decimator
 * ========================================================================= */

TEST(adsb_input_decimation_tracks_the_rate) {
    struct Case {
        double rate_hz;
        size_t expect;
    };

    /* Only whole multiples decimate. A fractional rate is left entirely to the
     * magnitude interpolator, exactly as before, because a boxcar over a
     * non-integer number of samples would move the output grid around. */
    const Case cases[] = {
        {2'000'000.0, 1},   /* native */
        {2'400'000.0, 1},   /* the rate the hardware self-test used */
        {3'999'999.0, 1},   /* just under 2x: not a multiple, no decimation */
        {4'000'000.0, 2},
        {5'000'000.0, 2},   /* decimate to 2.5 Msps, interpolate the rest */
        {8'000'000.0, 4},
        {16'000'000.0, 8},
        {1'000'000.0, 1},   /* below native: upsampled, never decimated */
    };

    for (const auto& c : cases) {
        app::adsb::AdsbDemod demod;
        demod.set_input_rate(c.rate_hz);

        if (demod.input_decimation() != c.expect) {
            std::printf("  %.0f Hz: decimation %zu, expected %zu\n", c.rate_hz,
                        demod.input_decimation(), c.expect);
        }
        CHECK_EQ(demod.input_decimation(), c.expect);
    }
}

TEST(adsb_demod_decodes_a_clean_burst_at_8msps) {
    const auto envelope = ppm_envelope(bytes_from_hex(kFrame), 112);
    const auto fast = fast_burst(envelope, 4, 1.0f, 0.0f, 1);

    app::adsb::AdsbDemod demod;
    demod.set_input_rate(8'000'000.0);
    CHECK_EQ(demod.input_decimation(), size_t{4});

    Capture cap;
    demod.process(fast.data(), fast.size(), cap.handler());

    CHECK(cap.got(kFrame));
}

TEST(adsb_decimator_carries_across_chunk_boundaries) {
    /* The boxcar accumulator is demodulator state. If it restarted on every
     * call the output grid would shift by up to three samples at each block
     * boundary, which at 8 Msps is most of a half-bit. Split at a position
     * that is deliberately NOT a multiple of the decimation, so a per-call
     * reset would show. */
    const auto envelope = ppm_envelope(bytes_from_hex(kFrame), 112);
    const auto fast = fast_burst(envelope, 4, 1.0f, 0.0f, 1);

    app::adsb::AdsbDemod whole;
    whole.set_input_rate(8'000'000.0);
    Capture one_shot;
    whole.process(fast.data(), fast.size(), one_shot.handler());

    /* The burst is 1472 samples (368 magnitudes at 4x), and the frame completes
     * on the 1216th. So: in the lead-in, mid-payload, and inside the last bit.
     * The third was 4097 until 2026-08-13 — past the end of the burst, so the
     * guard below silently skipped it and this loop only ever ran two splits. */
    for (size_t split : {size_t{101}, size_t{1023}, size_t{1201}}) {
        if (split >= fast.size()) continue;

        app::adsb::AdsbDemod demod;
        demod.set_input_rate(8'000'000.0);

        Capture cap;
        demod.process(fast.data(), split, cap.handler());
        demod.process(fast.data() + split, fast.size() - split, cap.handler());

        if (!cap.got(kFrame)) std::printf("  split at %zu lost the frame\n", split);
        CHECK(cap.got(kFrame));
    }

    CHECK(one_shot.got(kFrame));
}

TEST(adsb_decimation_beats_subsampling_in_noise) {
    /* THE claim that makes a faster radio worth using. Both paths get the same
     * 8 Msps waveform with the same noise realisation; one averages each group
     * of four complex samples before the square law, the other keeps one of
     * the four. Averaging four independent complex noise samples quarters the
     * noise power while leaving the pulse amplitude alone, so the decimating
     * path should need half the signal amplitude — 6 dB — for the same decode
     * rate.
     *
     * Measured as the lowest level on a 1.25x ladder at which 13 of 16 bursts
     * decode CRC-clean. */
    constexpr float kNoiseRms = 1.0f;
    constexpr size_t kTrials = 16;
    constexpr size_t kNeed = 13;

    const double decimating =
        sensitivity_threshold(&decode_decimator_8m, 0.0, 4, kNoiseRms, kTrials, kNeed);
    const double subsampling =
        sensitivity_threshold(&decode_subsample_4, 0.0, 4, kNoiseRms, kTrials, kNeed);

    std::printf("  8 Msps sensitivity: decimating %.3f, subsampling %.3f (ratio %.2f)\n",
                decimating, subsampling,
                (decimating > 0.0) ? subsampling / decimating : 0.0);

    /* Both must work somewhere on the ladder, or the comparison is vacuous. */
    CHECK(decimating > 0.0);
    CHECK(subsampling > 0.0);
    if (decimating <= 0.0 || subsampling <= 0.0) return;

    /* Theory says 2.0. The ladder resolves 1.25x steps and the trial count is
     * finite, so the assertion is the conservative half of that — but it is
     * still far outside what a null result could produce. */
    CHECK(subsampling >= decimating * 1.5);
}

/* =========================================================================
 * 3. What a gap does, and why pump() resets on one
 * ========================================================================= */

TEST(adsb_demod_decodes_a_frame_split_across_contiguous_chunks) {
    /* The reason for the migration. Under the snapshot tap the app reset the
     * demodulator on every block, so a burst cut by a block boundary was gone;
     * on a contiguous stream the state carries and it decodes. Split inside
     * the data bits, not in the lead-in. */
    const auto envelope = ppm_envelope(bytes_from_hex(kFrame), 112);
    const auto burst = fast_burst(envelope, 1, 1.0f, 0.0f, 1);

    const size_t split = 64 + 16 + 100; /* lead, preamble, then 50 bits in */
    CHECK(split < burst.size());

    app::adsb::AdsbDemod demod;
    demod.set_input_rate(app::adsb::AdsbDemod::kNativeSampleRate);

    Capture cap;
    demod.process(burst.data(), split, cap.handler());
    demod.process(burst.data() + split, burst.size() - split, cap.handler());

    CHECK(cap.got(kFrame));

    /* And the control: the old per-block reset in the same place loses it. */
    app::adsb::AdsbDemod resetting;
    resetting.set_input_rate(app::adsb::AdsbDemod::kNativeSampleRate);

    Capture cap2;
    resetting.process(burst.data(), split, cap2.handler());
    resetting.reset();
    resetting.process(burst.data() + split, burst.size() - split, cap2.handler());

    CHECK(!cap2.got(kFrame));
}

TEST(adsb_a_gap_without_a_reset_swallows_the_next_frame) {
    /* Why pump() must reset when the tap reports a hole, and the red half of
     * the red/green pair with the test below.
     *
     * A strong burst is cut off mid-frame by the gap. The demodulator is left
     * decoding it, with amp_ set to that strong preamble. process_one() only
     * abandons a frame in progress for a preamble that is STRONGER, so the
     * next burst — a weaker one, arriving after the hole — is not merely
     * missed: its bits are consumed as the interrupted frame's payload. */
    const auto envelope = ppm_envelope(bytes_from_hex(kFrame), 112);
    const auto strong = fast_burst(envelope, 1, 4.0f, 0.0f, 1);
    const auto weak = fast_burst(envelope, 1, 1.0f, 0.0f, 2);

    /* Cut 24 bits in: 88 bits of payload still owed, which is more than the
     * lead-in and preamble of the burst that follows. */
    const size_t cut = 64 + 16 + 48;

    app::adsb::AdsbDemod demod;
    demod.set_input_rate(app::adsb::AdsbDemod::kNativeSampleRate);

    Capture cap;
    demod.process(strong.data(), cut, cap.handler());
    /* ... the hole ... nothing is fed, and nothing is reset ... */
    demod.process(weak.data(), weak.size(), cap.handler());

    CHECK(!cap.got(kFrame));

    /* On its own the weak burst decodes perfectly well, so what was lost was
     * the stale state, not the signal. */
    app::adsb::AdsbDemod control;
    control.set_input_rate(app::adsb::AdsbDemod::kNativeSampleRate);
    Capture control_cap;
    control.process(weak.data(), weak.size(), control_cap.handler());
    CHECK(control_cap.got(kFrame));
}

TEST(adsb_a_reset_after_a_gap_recovers_the_next_frame) {
    /* The green half: the same sequence with the reset pump() performs. */
    const auto envelope = ppm_envelope(bytes_from_hex(kFrame), 112);
    const auto strong = fast_burst(envelope, 1, 4.0f, 0.0f, 1);
    const auto weak = fast_burst(envelope, 1, 1.0f, 0.0f, 2);

    const size_t cut = 64 + 16 + 48;

    app::adsb::AdsbDemod demod;
    demod.set_input_rate(app::adsb::AdsbDemod::kNativeSampleRate);

    Capture cap;
    demod.process(strong.data(), cut, cap.handler());
    demod.reset(); /* what pump() does when Block::lost_before is non-zero */
    demod.process(weak.data(), weak.size(), cap.handler());

    CHECK(cap.got(kFrame));
}

TEST(adsb_reset_clears_the_decimator_accumulator) {
    /* reset() has to drop the boxcar's partial sum too. If it did not, every
     * group after the gap would straddle it, and the whole magnitude stream
     * would sit three input samples out of step with a clean one.
     *
     * Asserting only that the frame still decodes does NOT test this: the
     * preamble detector re-locks and three samples at 8 Msps is well under a
     * bit period, so the decode survives the splice either way. (This test did
     * exactly that until 2026-08-13 and passed with the accumulator reset
     * deleted.) The per-frame amplitude is the observable that moves — it is
     * the sum of the four preamble magnitudes, which changes as soon as a
     * group straddles one of the preamble's edges. So compare against a demod
     * that saw the same burst and nothing else. */
    const auto envelope = ppm_envelope(bytes_from_hex(kFrame), 112);
    const auto fast = fast_burst(envelope, 4, 1.0f, 0.0f, 1);

    /* Pin the scale on both. reset() deliberately keeps the measured noise
     * floor, so without a fixed reference the two demods could differ in
     * scale rather than in phase and the comparison would prove nothing. */
    app::adsb::AdsbDemod clean;
    clean.set_input_rate(8'000'000.0);
    clean.set_reference_amplitude(1.0f);
    Capture clean_cap;
    clean.process(fast.data(), fast.size(), clean_cap.handler());
    CHECK(clean_cap.got(kFrame));

    app::adsb::AdsbDemod demod;
    demod.set_input_rate(8'000'000.0);
    demod.set_reference_amplitude(1.0f);

    Capture cap;
    /* 1023 is 3 samples past a group boundary (4*255 + 3), so 3 are left in
     * the sum, and it stops short of the 1216th sample the frame completes on
     * — the first pass must not be able to decode it on its own.
     *
     * It has to stay inside the burst, which is why the count is checked. This
     * read 4099 samples out of a 1472-sample vector until 2026-08-13: the 21 KB
     * of heap past the end went into update_noise_floor()'s median, and since
     * noise_floor_power_ survives reset() by design, the second pass scaled its
     * threshold off whatever the allocator happened to be holding. The test
     * failed on about one run in six. */
    const size_t partial = 1023;
    CHECK(partial < fast.size());
    demod.process(fast.data(), partial, cap.handler());
    demod.reset();
    demod.process(fast.data(), fast.size(), cap.handler());

    CHECK(cap.got(kFrame));

    /* Same samples, same scale: a demod that was properly reset has to report
     * the frame at exactly the amplitude the clean one did. A retained partial
     * sum shifts the sampling phase and moves it. */
    const float clean_amp = clean_cap.amp_of(kFrame);
    CHECK(clean_amp > 0.0f);
    CHECK_NEAR(cap.amp_of(kFrame), clean_amp, 1e-3);
}

/* =========================================================================
 * 4. The app on the tap, end to end
 * ========================================================================= */

TEST(adsb_view_opens_a_gapless_tap_at_a_caps_chosen_rate) {
    ViewHarness h;
    CHECK(h.view != nullptr);
    if (h.view == nullptr) return;

    /* The published B200 caps a closed UsrpRadio reports reach 61.44 Msps; the
     * app asks for 8 because that is where its own gains stop. */
    CHECK_NEAR(h.view->sample_rate_hz(), 8'000'000.0, 1.0);
    CHECK(h.view->gapless());
    CHECK_EQ(h.receiver.raw_tap_enabled(), true);

    /* Sized in time, not in samples: 250 ms of 8 Msps. */
    CHECK_EQ(h.receiver.raw_tap().capacity(), size_t{2'000'000});

    /* Nothing has happened yet, so nothing has been lost — and the honest
     * answer to "how much of the air did you see" before any air is all of it. */
    CHECK_EQ(h.view->samples_lost(), uint64_t{0});
    CHECK_EQ(h.view->gaps(), 0u);
    CHECK_NEAR(h.view->air_fraction(), 1.0, 1e-12);
}

TEST(adsb_view_decodes_everything_written_to_the_tap) {
    ViewHarness h;
    if (h.view == nullptr) return;

    const auto burst = clean_burst(h.view->sample_rate_hz(), 1.0f);
    h.produce(burst);
    h.view->pump();

    CHECK_EQ(h.view->tracker().frames_accepted(), 1u);
    CHECK_EQ(h.view->samples_lost(), uint64_t{0});
    CHECK_EQ(h.view->gaps(), 0u);
    CHECK_EQ(h.view->samples_decoded(), static_cast<uint64_t>(burst.size()));
    CHECK_NEAR(h.view->air_fraction(), 1.0, 1e-12);
}

TEST(adsb_view_decodes_a_burst_split_across_two_pumps) {
    /* The migration's whole point, at the level the app actually runs at: a
     * frame that arrives half in one UI frame and half in the next. The old
     * snapshot path reset between the two and could not have this. */
    ViewHarness h;
    if (h.view == nullptr) return;

    const auto burst = clean_burst(h.view->sample_rate_hz(), 1.0f);
    const size_t split = burst.size() / 2;

    const auto at = static_cast<std::ptrdiff_t>(split);
    h.produce(std::vector<dsp::cfloat>(burst.begin(), burst.begin() + at));
    h.view->pump();

    h.produce(std::vector<dsp::cfloat>(burst.begin() + at, burst.end()));
    h.view->pump();

    CHECK_EQ(h.view->tracker().frames_accepted(), 1u);
    CHECK_EQ(h.view->gaps(), 0u);
}

TEST(adsb_view_reports_a_tap_overflow_and_recovers_from_it) {
    ViewHarness h;
    if (h.view == nullptr) return;

    const double rate = h.view->sample_rate_hz();
    const size_t cap = h.receiver.raw_tap().capacity();
    CHECK(cap > 0);
    if (cap == 0) return;

    /* Fill the ring so that it ends mid-frame: silence, then a strong burst
     * cut off partway through its payload. This is the state that makes a gap
     * dangerous rather than merely lossy. */
    const auto strong = clean_burst(rate, 4.0f);
    const size_t truncated = strong.size() / 2;
    CHECK(cap > truncated);

    h.produce_silence(cap - truncated);
    h.produce(std::vector<dsp::cfloat>(
        strong.begin(), strong.begin() + static_cast<std::ptrdiff_t>(truncated)));

    /* The ring is now exactly full. Everything offered from here is lost, and
     * the tap counts it rather than quietly overwriting. */
    h.produce_silence(50'000);

    /* First pump drains to the seam. The hole is not announced yet — the tap
     * publishes it with the samples that FOLLOW it, so that a block is never
     * reported as contiguous when it is not. */
    h.view->pump();
    CHECK_EQ(h.view->gaps(), 0u);
    CHECK_EQ(h.view->samples_decoded(), static_cast<uint64_t>(cap));

    /* Now the post-gap traffic: a weaker burst, which the interrupted strong
     * frame would swallow if the demodulator were not reset. */
    const auto weak = clean_burst(rate, 1.0f);
    h.produce(weak);
    h.view->pump();

    /* The loss is reported, placed and counted. */
    CHECK_EQ(h.view->gaps(), 1u);
    CHECK_EQ(h.view->samples_lost(), uint64_t{50'000});

    /* And the frame after the hole is decoded, which is only true because the
     * gap triggered a reset — adsb_a_gap_without_a_reset_swallows_the_next_frame
     * shows what happens otherwise. */
    CHECK_EQ(h.view->tracker().frames_accepted(), 1u);

    /* Air fraction is now honestly below 1 and matches the counters. */
    const double expect = static_cast<double>(h.view->samples_decoded()) /
                          static_cast<double>(h.view->samples_decoded() + 50'000);
    CHECK_NEAR(h.view->air_fraction(), expect, 1e-12);
    CHECK(h.view->air_fraction() < 1.0);
}

TEST(adsb_view_closes_the_tap_when_it_goes_away) {
    /* Nothing drains the tap once the app is gone, so leaving it open would
     * cost the DSP thread a full-rate copy for no reader — 128 MB/s at
     * 16 Msps, paid by every other app the operator opens next. */
    radio::UsrpRadio radio{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{radio, audio};
    ui::NavigationView nav{{0, 0, 240, 304}};

    radio::RadioDevice* saved_radio{app::globals().radio};
    radio::ReceiverModel* saved_receiver{app::globals().receiver};
    ui::NavigationView* saved_nav{app::globals().nav};

    app::globals().radio = &radio;
    app::globals().receiver = &receiver;
    app::globals().nav = &nav;

    /* A root the stack can fall back to: NavigationView never pops the last
     * view, so the app has to sit above something to be poppable at all. */
    nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    nav.service();

    nav.push(std::make_unique<app::AdsbRxView>());
    nav.service();
    CHECK_EQ(nav.depth(), size_t{2});
    CHECK_EQ(receiver.raw_tap_enabled(), true);

    /* Leaving the app destroys the view, whose destructor closes the tap. */
    nav.pop();
    nav.service();
    CHECK_EQ(nav.depth(), size_t{1});
    CHECK_EQ(receiver.raw_tap_enabled(), false);

    app::globals().radio = saved_radio;
    app::globals().receiver = saved_receiver;
    app::globals().nav = saved_nav;
}
