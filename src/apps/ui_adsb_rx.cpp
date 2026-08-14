/*
 * mayhem-b200 — ADS-B / Mode S receiver.
 *
 * See ui_adsb_rx.hpp for what was ported, from where, and what changed.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2023 Kyle Reed
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_adsb_rx.hpp"

#include "../core/file_path.hpp"
#include "../core/string_format.hpp"
#include "app_context.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace app {

namespace adsb {

namespace {
constexpr double kPi = 3.14159265358979323846;
}  // namespace

/* --- CRC-24 ---------------------------------------------------------------- */

uint32_t mode_s_crc(const uint8_t* data, size_t length) {
    /* Width 24, poly 0xFFF409, init 0, no reflection, no final XOR. That is
     * exactly the polynomial upstream encodes as 0x1205FFF in reversed-offset
     * form; tests/test_adsb.cpp runs upstream's algorithm alongside this one. */
    dsp::Crc<24> crc{kModeSPolynomial, 0, 0};
    crc.process_bytes(data, length);
    return crc.checksum();
}

void AdsbFrame::make_CRC() {
    const uint32_t computed = compute_CRC();
    const size_t crc_pos = data_length();

    raw_data_[crc_pos] = static_cast<uint8_t>((computed >> 16) & 0xFF);
    raw_data_[crc_pos + 1] = static_cast<uint8_t>((computed >> 8) & 0xFF);
    raw_data_[crc_pos + 2] = static_cast<uint8_t>(computed & 0xFF);

    if (index_ < crc_pos + 3) index_ = static_cast<uint8_t>(crc_pos + 3);
}

uint32_t AdsbFrame::check_CRC() const {
    const uint32_t computed = compute_CRC();
    const size_t crc_pos = data_length();

    const uint32_t received = (static_cast<uint32_t>(raw_data_[crc_pos]) << 16) |
                              (static_cast<uint32_t>(raw_data_[crc_pos + 1]) << 8) |
                              static_cast<uint32_t>(raw_data_[crc_pos + 2]);

    return (received ^ computed) & 0xFFFFFF;
}

/* --- CPR ------------------------------------------------------------------- */

double cpr_mod(double a, double b) {
    return a - (b * std::floor(a / b));
}

int cpr_NL(double lat) {
    /* Upstream evaluates the closed form unguarded. Its own comment names
     * dump1090 as the source, and dump1090 carries these special cases: the
     * acos() argument leaves [-1, 1] past 87 degrees, and floor(NaN) is
     * undefined behaviour. */
    if (lat < 0) lat = -lat;  /* the function is symmetric about the equator */

    if (lat == 0.0) return 59;
    if (lat == 87.0) return 2;
    if (lat > 87.0) return 1;

    const double c = std::cos(kPi * lat / 180.0);
    const double arg = 1.0 - ((1.0 - std::cos(kPi / (2.0 * NZ))) / (c * c));
    if (arg <= -1.0) return 1;
    if (arg >= 1.0) return 59;

    return static_cast<int>(std::floor(2.0 * kPi / std::acos(arg)));
}

int cpr_N(double lat, int is_odd) {
    int nl = cpr_NL(lat) - is_odd;
    if (nl < 1) nl = 1;
    return nl;
}

double cpr_Dlon(double lat, int is_odd) {
    return 360.0 / cpr_N(lat, is_odd);
}

/* --- Message decoders ------------------------------------------------------ */

std::string decode_frame_id(const AdsbFrame& frame) {
    const uint8_t* raw_data = frame.get_raw_data();
    uint64_t callsign_coded = 0;

    /* ME bits 9-56: the eight 6-bit characters live in frame bytes 5..10. */
    for (size_t c = 5; c < 11; c++) {
        callsign_coded <<= 8;
        callsign_coded |= raw_data[c];
    }

    std::string callsign;
    callsign.reserve(8);
    for (size_t c = 0; c < 8; c++) {
        callsign.append(1, icao_id_lut[(callsign_coded >> 42) & 0x3F]);
        callsign_coded <<= 6;
    }

    return callsign;
}

int32_t decode_me_altitude(const uint8_t* raw_data, bool& valid) {
    /* ME bits 9-20: 11 altitude bits with a Q bit at bit 8 of the field. */
    if (raw_data[5] & 1) {
        valid = true;
        return static_cast<int32_t>(
                   ((((raw_data[5] & 0xFE) << 3) | ((raw_data[6] & 0xF0) >> 4))) * 25) -
               1000;
    }

    valid = false;
    return 0;
}

int32_t decode_ac13_altitude(const uint8_t* raw_data, bool& valid) {
    const uint8_t m_bit = raw_data[3] & (1 << 6);
    const uint8_t q_bit = raw_data[3] & (1 << 4);

    /* M set means metric, Q clear means Gillham (Gray-coded) — upstream decodes
     * neither. Upstream then still marks the altitude valid and stores 0 ft,
     * which puts the aircraft on the ground on the map; here it stays invalid. */
    if (m_bit || !q_bit) {
        valid = false;
        return 0;
    }

    const int n = ((raw_data[2] & 31) << 6) |
                  ((raw_data[3] & 0x80) >> 2) |
                  ((raw_data[3] & 0x20) >> 1) |
                  (raw_data[3] & 15);

    valid = true;
    return 25 * n - 1000;
}

uint16_t decode_squawk(const uint8_t* s) {
    /* 13-bit identity field, interleaved as C1 A1 C2 A2 C4 A4 X B1 D1 B2 D2 B4
     * D4. The result is the four octal digits read as a decimal number, which
     * is how a squawk is written. */
    uint16_t sqwk = static_cast<uint16_t>(((s[1] & 0x80) >> 5) | ((s[0] & 0x02) >> 0) | ((s[0] & 0x08) >> 3));  // A
    sqwk = static_cast<uint16_t>(sqwk * 10);
    sqwk = static_cast<uint16_t>(sqwk + (((s[1] & 0x02) << 1) | ((s[1] & 0x08) >> 2) | ((s[1] & 0x20) >> 5)));  // B
    sqwk = static_cast<uint16_t>(sqwk * 10);
    sqwk = static_cast<uint16_t>(sqwk + (((s[0] & 0x01) << 2) | ((s[0] & 0x04) >> 1) | ((s[0] & 0x10) >> 4)));  // C
    sqwk = static_cast<uint16_t>(sqwk * 10);
    sqwk = static_cast<uint16_t>(sqwk + (((s[1] & 0x01) << 2) | ((s[1] & 0x04) >> 1) | ((s[1] & 0x10) >> 4)));  // D

    return sqwk;
}

adsb_pos decode_frame_pos(const AdsbFrame& frame_even, const AdsbFrame& frame_odd) {
    adsb_pos position{};

    const uint32_t time_even = frame_even.get_rx_timestamp();
    const uint32_t time_odd = frame_odd.get_rx_timestamp();
    const uint8_t* frame_data_even = frame_even.get_raw_data();
    const uint8_t* frame_data_odd = frame_odd.get_raw_data();

    /* Altitude comes from whichever frame is the more recent. */
    const bool even_is_newer = (time_even > time_odd);
    const uint8_t* raw_data = even_is_newer ? frame_data_even : frame_data_odd;

    bool alt_valid = false;
    const int32_t altitude = decode_me_altitude(raw_data, alt_valid);
    if (alt_valid) {
        position.altitude = altitude;
        position.alt_valid = true;
    }

    /* ME bits 23-39 (latitude) and 40-56 (longitude), 17 bits each. */
    const uint32_t latcprE = static_cast<uint32_t>(((frame_data_even[6] & 3) << 15) |
                                                   (frame_data_even[7] << 7) |
                                                   (frame_data_even[8] >> 1));
    const uint32_t loncprE = static_cast<uint32_t>(((frame_data_even[8] & 1) << 16) |
                                                   (frame_data_even[9] << 8) |
                                                   frame_data_even[10]);
    const uint32_t latcprO = static_cast<uint32_t>(((frame_data_odd[6] & 3) << 15) |
                                                   (frame_data_odd[7] << 7) |
                                                   (frame_data_odd[8] >> 1));
    const uint32_t loncprO = static_cast<uint32_t>(((frame_data_odd[8] & 1) << 16) |
                                                   (frame_data_odd[9] << 8) |
                                                   frame_data_odd[10]);

    const double cpr_lat_even = latcprE / CPR_MAX_VALUE;
    const double cpr_lat_odd = latcprO / CPR_MAX_VALUE;
    const double cpr_lon_even = loncprE / CPR_MAX_VALUE;
    const double cpr_lon_odd = loncprO / CPR_MAX_VALUE;

    /* Latitude zone index. */
    const double j = std::floor(((59.0 * cpr_lat_even) - (60.0 * cpr_lat_odd)) + 0.5);
    double latE = (360.0 / 60.0) * (cpr_mod(j, 60) + cpr_lat_even);
    double latO = (360.0 / 59.0) * (cpr_mod(j, 59) + cpr_lat_odd);

    if (latE >= 270) latE -= 360;
    if (latO >= 270) latO -= 360;

    /* Both frames have to sit in the same latitude zone, or the pair cannot be
     * combined and the aircraft has to wait for a fresher one. */
    if (cpr_NL(latE) != cpr_NL(latO))
        return position;

    double longitude = 0.0;

    if (even_is_newer) {
        const int ni = cpr_N(latE, 0);
        const double Dlon = 360.0 / ni;
        const double m = std::floor((cpr_lon_even * (cpr_NL(latE) - 1)) -
                                    (cpr_lon_odd * cpr_NL(latE)) + 0.5);

        longitude = Dlon * (cpr_mod(m, ni) + cpr_lon_even);
        position.latitude = static_cast<float>(latE);
    } else {
        const int ni = cpr_N(latO, 1);
        const double Dlon = 360.0 / ni;
        const double m = std::floor((cpr_lon_even * (cpr_NL(latO) - 1)) -
                                    (cpr_lon_odd * cpr_NL(latO)) + 0.5);

        longitude = Dlon * (cpr_mod(m, ni) + cpr_lon_odd);
        position.latitude = static_cast<float>(latO);
    }

    if (longitude >= 180) longitude -= 360;
    position.longitude = static_cast<float>(longitude);
    position.pos_valid = true;

    return position;
}

adsb_vel decode_frame_velo(const AdsbFrame& frame) {
    adsb_vel velo{};

    const uint8_t* frame_data = frame.get_raw_data();
    const uint8_t velo_type = frame.get_msg_sub();

    if (velo_type >= 1 && velo_type <= 4) {
        /* Vertical rate is present in every subtype. */
        velo.v_rate = ((((frame_data[8] & 0x07) << 6) | (frame_data[9] >> 2)) - 1) * 64;
        if (frame_data[8] & 0x8) velo.v_rate *= -1;
    }

    if (velo_type == 1 || velo_type == 2) {
        /* Ground speed: signed east-west and north-south components. */
        const int32_t raw_ew = ((frame_data[5] & 0x03) << 8) | frame_data[6];
        const int32_t raw_ns = ((frame_data[7] & 0x7f) << 3) | (frame_data[8] >> 5);

        if (raw_ew && raw_ns) {
            /* Both components are offset by one; zero means "not available". */
            int32_t velo_ew = raw_ew - 1;
            int32_t velo_ns = raw_ns - 1;

            if (velo_type == 2) {  /* supersonic: quarter-knot -> knot scaling */
                velo_ew = velo_ew << 2;
                velo_ns = velo_ns << 2;
            }

            if (frame_data[5] & 0x04) velo_ew *= -1;  /* 1 = east to west  */
            if (frame_data[7] & 0x80) velo_ns *= -1;  /* 1 = north to south */

            /* Upstream uses fast_int_magnitude/int_atan2, integer estimators
             * for a Cortex-M0 with no FPU. The magnitude estimator is ~4% low,
             * so the host uses exact maths. */
            velo.speed = static_cast<int32_t>(
                std::lround(std::hypot(static_cast<double>(velo_ns),
                                       static_cast<double>(velo_ew))));

            if (velo.speed) {
                long heading = std::lround(std::atan2(static_cast<double>(velo_ew),
                                                      static_cast<double>(velo_ns)) *
                                           180.0 / kPi);
                if (heading < 0) heading += 360;
                if (heading >= 360) heading -= 360;
                velo.heading = static_cast<uint16_t>(heading);

                velo.valid = true;
                velo.type = SPD_GND;
            }
        }
    } else if (velo_type == 3 || velo_type == 4) {
        /* Airspeed: a 10-bit magnetic heading scaled by 360/1024 = 45/128. */
        velo.valid = (frame_data[5] & (1 << 2)) != 0;
        velo.heading = static_cast<uint16_t>(
            ((((frame_data[5] & 0x03) << 8) | frame_data[6]) * 45) >> 7);

        const int32_t raw = ((frame_data[7] & 0x7F) << 3) | (frame_data[8] >> 5);
        if (raw) {
            velo.speed = raw - 1;
            velo.type = (frame_data[7] & 0x80) ? SPD_TAS : SPD_IAS;

            if (velo_type == 4) velo.speed *= 4;  /* supersonic */
        }
    }

    return velo;
}

/* --- Rate conversion ------------------------------------------------------- */

void MagnitudeResampler::set_rate(double input_hz, double output_hz) {
    if (input_hz <= 0.0 || output_hz <= 0.0) {
        bypass_ = true;
        step_ = 1.0;
    } else {
        /* Within a hertz counts as equal: the B200 hits exact rates when the
         * master clock divides evenly, and bypassing keeps the samples the
         * radio actually delivered. */
        bypass_ = std::fabs(input_hz - output_hz) <= 1.0;
        step_ = input_hz / output_hz;
    }
    reset();
}

void MagnitudeResampler::reset() {
    frac_ = 0.0;
    prev_ = 0.0f;
    primed_ = false;
}

void MagnitudeResampler::process(const float* in, size_t count, std::vector<float>& out) {
    if (bypass_) {
        out.insert(out.end(), in, in + count);
        return;
    }

    for (size_t i = 0; i < count; i++) {
        const float cur = in[i];

        if (!primed_) {
            prev_ = cur;
            primed_ = true;
            frac_ = 0.0;
            continue;
        }

        /* frac_ is where the next output sits between prev_ and cur, measured
         * in input samples. Emitting until it passes cur handles upsampling;
         * subtracting one afterwards handles downsampling. */
        while (frac_ < 1.0) {
            const float f = static_cast<float>(frac_);
            out.push_back(prev_ + ((cur - prev_) * f));
            frac_ += step_;
        }
        frac_ -= 1.0;
        prev_ = cur;
    }
}

/* --- Demodulator ----------------------------------------------------------- */

namespace {

/* Weight given to a new block in the noise-floor average: about a ten-block
 * time constant, which is a sixth of a second at the 60 Hz rate the app pumps
 * at. Slow enough that one burst-heavy block cannot move the floor, quick
 * enough to follow a gain change. */
constexpr float kNoiseFloorAlpha = 0.1f;

/* At most this many magnitudes per block go into the median. */
constexpr size_t kNoiseFloorTaps = 512;

/* For circularly symmetric Gaussian noise the median of |s|^2 is ln(2) times
 * its mean, so dividing the median by this gives the mean noise power. */
constexpr float kMedianOverMeanPower = 0.6931472f;

}  // namespace

void AdsbDemod::reset() {
    frame_.clear();
    decoding_ = false;
    msg_len_ = 112;
    prev_mag_ = 0.0f;
    amp_ = 0.0f;
    bit_count_ = 0;
    sample_count_ = 0;
    byte_ = 0;
    for (size_t i = 0; i <= kPreambleLength; i++) shifter_[i] = 0.0f;

    /* A partial boxcar belongs to the samples before the gap. Averaging it
     * with the ones after would build one output sample out of two moments —
     * the same splice the preamble detector must never be shown. */
    decim_acc_ = dsp::cfloat{0.0f, 0.0f};
    decim_n_ = 0;

    resampler_.reset();
    mag_scratch_.clear();

    /* noise_floor_power_ and reference_amplitude_ survive on purpose — see the
     * note on reset() in the header. */
}

void AdsbDemod::set_reference_amplitude(float amplitude) {
    reference_amplitude_ = (amplitude > 0.0f) ? amplitude : 0.0f;
}

float AdsbDemod::noise_floor() const {
    return (noise_floor_power_ > 0.0f) ? std::sqrt(noise_floor_power_) : 0.0f;
}

float AdsbDemod::input_scale() const {
    if (reference_amplitude_ > 0.0f) return kInt8FullScale / reference_amplitude_;
    if (noise_floor_power_ > 0.0f) return kNoiseFloorCounts / std::sqrt(noise_floor_power_);

    /* Nothing measured yet: upstream's literal mapping of full scale to 127. */
    return kInt8FullScale;
}

void AdsbDemod::update_noise_floor(const float* mags, size_t count) {
    if (count == 0) return;

    /* The median, not the mean: a burst inside the block must not lift the
     * floor it is being measured against. A 112-bit frame is 120 us, well
     * under 1% of the ~16 ms of air one pump now hands over, and the mean
     * climbs with every frame received; the median does not move until half
     * the block is signal. Strided so the cost is bounded whatever the block
     * size — which now varies with both the sample rate and the frame time. */
    const size_t stride = ((count + kNoiseFloorTaps - 1) / kNoiseFloorTaps);

    noise_scratch_.clear();
    for (size_t i = 0; i < count; i += stride) noise_scratch_.push_back(mags[i]);
    if (noise_scratch_.empty()) return;

    const size_t mid = noise_scratch_.size() / 2;
    std::nth_element(noise_scratch_.begin(),
                     noise_scratch_.begin() + static_cast<ptrdiff_t>(mid),
                     noise_scratch_.end());

    const float median = noise_scratch_[mid];

    /* A block that is more than half exact silence says nothing about the
     * noise floor — a synthesised waveform, or a stream that has not started.
     * Keep whatever was measured before. */
    if (!(median > 0.0f)) return;

    const float power = median / kMedianOverMeanPower;

    noise_floor_power_ = (noise_floor_power_ > 0.0f)
                             ? noise_floor_power_ + (kNoiseFloorAlpha * (power - noise_floor_power_))
                             : power;
}

void AdsbDemod::set_input_rate(double hz) {
    if (hz <= 0.0) hz = kNativeSampleRate;
    if (hz == input_rate_) return;

    input_rate_ = hz;

    /* Whole multiples of the native rate go through the complex boxcar, which
     * filters as well as decimates; only the leftover fraction reaches the
     * magnitude interpolator. 8 Msps: decimate by 4, interpolator bypasses.
     * 2.4 Msps: decimate by 1, interpolator does all of it, exactly as before.
     * 5 Msps: decimate by 2 to 2.5, then interpolate. */
    decim_ = 1;
    const double ratio = hz / kNativeSampleRate;
    if (ratio >= 2.0) {
        /* A shade of slack so a rate that IS an exact multiple cannot land one
         * ULP low and decimate by one less than it should. */
        decim_ = static_cast<size_t>(std::floor(ratio + 1e-9));
    }

    decim_acc_ = dsp::cfloat{0.0f, 0.0f};
    decim_n_ = 0;

    resampler_.set_rate(hz / static_cast<double>(decim_), kNativeSampleRate);
}

void AdsbDemod::process(const dsp::cfloat* samples, size_t count, const FrameHandler& on_frame) {
    mag_raw_.clear();

    if (decim_ > 1) {
        /* Average the complex samples over each output period, then square.
         * Doing it in this order is the whole point — see input_decimation()
         * in the header. The accumulator carries across calls, so chunking the
         * stream does not move where the boxcar boundaries fall. */
        mag_raw_.reserve((count / decim_) + 1);

        const float inv = 1.0f / static_cast<float>(decim_);
        for (size_t i = 0; i < count; i++) {
            decim_acc_ += samples[i];
            if (++decim_n_ < decim_) continue;

            const float re = decim_acc_.real() * inv;
            const float im = decim_acc_.imag() * inv;
            mag_raw_.push_back((re * re) + (im * im));

            decim_acc_ = dsp::cfloat{0.0f, 0.0f};
            decim_n_ = 0;
        }
    } else {
        mag_raw_.reserve(count);
        for (size_t i = 0; i < count; i++) {
            const float re = samples[i].real();
            const float im = samples[i].imag();
            mag_raw_.push_back((re * re) + (im * im));
        }
    }

    /* Measure before scaling: the floor is a property of the input, not of the
     * units the magnitudes are about to be expressed in. */
    if (reference_amplitude_ <= 0.0f) update_noise_floor(mag_raw_.data(), mag_raw_.size());

    /* Into the int8 units the firmware's magnitudes are in, so the per-frame
     * amplitude reported to the UI is on upstream's scale. Magnitudes are
     * powers, so an amplitude scale enters squared. */
    const float scale = input_scale();
    const float power_scale = scale * scale;
    for (float& m : mag_raw_) m *= power_scale;

    mag_scratch_.clear();
    resampler_.process(mag_raw_.data(), mag_raw_.size(), mag_scratch_);

    process_magnitudes(mag_scratch_.data(), mag_scratch_.size(), on_frame);
}

void AdsbDemod::process_magnitudes(const float* mags, size_t count, const FrameHandler& on_frame) {
    for (size_t i = 0; i < count; i++) process_one(mags[i], on_frame);
}

void AdsbDemod::process_one(float mag, const FrameHandler& on_frame) {
    /* One bit is two samples and the bit value is the transition between them:
     * high then low is a 1, low then high is a 0. */
    if (decoding_) {
        if ((sample_count_ & 1) == 1) {
            uint8_t bit;

            if (bit_count_ >= msg_len_) {
                if (on_frame) on_frame(frame_, amp_);
                frames_++;
                decoding_ = false;
                bit = (prev_mag_ > mag) ? 1 : 0;
            } else {
                bit = (prev_mag_ > mag) ? 1 : 0;
            }

            byte_ = static_cast<uint8_t>(bit | (byte_ << 1));
            bit_count_++;

            if ((bit_count_ & 0x7) == 0) {
                frame_.push_byte(byte_);

                /* DF >= 16 (top bit of the first byte) means a long frame. */
                if (bit_count_ == 8)
                    msg_len_ = (byte_ & 0x80) ? 112 : 56;
            }
        }

        sample_count_++;
    }

    /* Keep hunting for a preamble even mid-frame, and switch to the new one if
     * it is stronger than the frame currently being decoded. */
    for (size_t c = 0; c < kPreambleLength; c++) shifter_[c] = shifter_[c + 1];
    shifter_[kPreambleLength] = mag;

    /* The 8 us preamble is 16 samples with pulses at 0, 1.0, 3.5 and 4.5 us:
     *    0123456789ABCDEF
     *    _-_-____-_-_____
     * shifter_[1] is the first pulse, shifter_[16] the sample just processed,
     * so the first data bit is the sample after next. */
    if (shifter_[0] < shifter_[1] &&
        shifter_[1] > shifter_[2] &&
        shifter_[2] < shifter_[3] &&
        shifter_[3] > shifter_[4] &&
        shifter_[4] < shifter_[1] &&
        shifter_[5] < shifter_[1] &&
        shifter_[6] < shifter_[1] &&
        shifter_[7] < shifter_[1] &&
        shifter_[8] > shifter_[9] &&
        shifter_[9] < shifter_[10] &&
        shifter_[10] > shifter_[11]) {
        /* The quiet samples between and after the spikes must sit below the
         * average spike level. Samples adjacent to a spike are not tested,
         * because an out-of-phase signal spreads energy into them. */
        const float this_amp = shifter_[1] + shifter_[3] + shifter_[8] + shifter_[10];
        const float high = this_amp / 9.0f;

        if (shifter_[5] < high &&
            shifter_[6] < high &&
            shifter_[12] < high &&
            shifter_[13] < high &&
            shifter_[14] < high) {
            if (!decoding_ || (this_amp > amp_)) {
                decoding_ = true;
                preambles_++;
                amp_ = this_amp;
                sample_count_ = 0;
                bit_count_ = 0;
                byte_ = 0;
                msg_len_ = 112;
                frame_.clear();
            }
        }
    }

    prev_mag_ = mag;
}

}  // namespace adsb

/* =========================================================================
 * Aircraft tracking
 * ========================================================================= */

AircraftRecentEntry::AircraftRecentEntry(uint32_t address)
    : ICAO_address{address} {
    icao_str = to_string_hex(address, 6);
}

uint32_t AircraftRecentEntry::amp_display() const {
    /* Upstream's amp >> 9, on a value that is a float here. The comparison is
     * written so that a NaN amplitude reads as zero rather than as whatever
     * the conversion happens to produce. */
    if (!(amp > 0.0f)) return 0;

    const float scaled = amp / 512.0f;
    if (scaled >= 4294967296.0f) return 0xFFFFFFFFu;

    return static_cast<uint32_t>(scaled);
}

void AircraftRecentEntry::set_frame_pos(const adsb::AdsbFrame& frame, uint32_t parity) {
    if (!parity)
        frame_pos_even = frame;
    else
        frame_pos_odd = frame;

    if (!frame_pos_even.empty() && !frame_pos_odd.empty()) {
        /* Upstream subtracts two uint32 timestamps and passes the result to
         * abs(), which is meaningless when the odd frame is the newer one.
         * Signed arithmetic here. */
        const int64_t delta =
            static_cast<int64_t>(frame_pos_even.get_rx_timestamp()) -
            static_cast<int64_t>(frame_pos_odd.get_rx_timestamp());

        if (std::llabs(delta) < static_cast<int64_t>(adsb::O_E_FRAME_TIMEOUT))
            pos = adsb::decode_frame_pos(frame_pos_even, frame_pos_odd);
    }
}

void AircraftRecentEntry::set_frame_velo(const adsb::AdsbFrame& frame) {
    velo = adsb::decode_frame_velo(frame);
}

int32_t AircraftRecentEntry::get_ground_speed() const {
    if (!velo.valid) return 0;

    if (velo.type == adsb::SPD_GND) return velo.speed;

    if (velo.type == adsb::SPD_IAS) {
        /* Rule of thumb: true airspeed runs about 2% above indicated per 1000
         * ft. Without an altitude there is nothing to correct with. */
        if (!pos.alt_valid) return velo.speed;
        return static_cast<int32_t>(velo.speed * (1.0f + (0.02f * (pos.altitude / 1000.0f))));
    }

    if (velo.type == adsb::SPD_TAS) return velo.speed;  /* wind is unknown */

    return 0;
}

void AircraftRecentEntry::inc_age(uint32_t delta) {
    age += delta;

    if (age < ADSBAgeLimit::Current)
        state = pos.pos_valid ? ADSBAgeState::Current : ADSBAgeState::Invalid;
    else if (age < ADSBAgeLimit::Recent)
        state = ADSBAgeState::Recent;
    else if (age < ADSBAgeLimit::Expired)
        state = ADSBAgeState::Old;
    else
        state = ADSBAgeState::Expired;
}

std::string format_adsb_log_line(const AdsbLogEntry& entry) {
    static const char speed_type_msg[][6] = {" Spd:", " IAS:", " TAS:"};

    std::string log_line;
    log_line.reserve(100);

    log_line = entry.raw_data;
    log_line += " ICAO:" + entry.icao;

    if (entry.sqwk)
        log_line += " Squawk:" + to_string_dec_uint(entry.sqwk, 4, '0');

    if (!entry.callsign.empty())
        log_line += " " + entry.callsign;

    if (entry.pos.alt_valid)
        log_line += " Alt:" + to_string_dec_int(entry.pos.altitude);

    if (entry.pos.pos_valid)
        log_line += " Lat:" + to_string_decimal(entry.pos.latitude, 7) +
                    " Lon:" + to_string_decimal(entry.pos.longitude, 7);

    if (entry.vel.valid)
        log_line += " Type:" + to_string_dec_uint(entry.vel_type) +
                    " Hdg:" + to_string_dec_uint(entry.vel.heading) +
                    speed_type_msg[entry.vel.type] +
                    to_string_dec_int(entry.vel.speed) +
                    " Vrate:" + to_string_dec_int(entry.vel.v_rate);

    if (entry.sil != 0)
        log_line += " Sil:" + to_string_dec_uint(entry.sil);

    return log_line;
}

/* --- AircraftTracker ------------------------------------------------------- */

AircraftRecentEntry* AircraftTracker::find(uint32_t icao) {
    auto it = ui::find_entry(recent_, icao);
    return (it == recent_.end()) ? nullptr : &(*it);
}

const AircraftRecentEntry* AircraftTracker::find(uint32_t icao) const {
    auto it = ui::find_entry(recent_, icao);
    return (it == recent_.end()) ? nullptr : &(*it);
}

void AircraftTracker::clear() {
    recent_.clear();
    frames_seen_ = 0;
    frames_accepted_ = 0;
}

AircraftRecentEntry& AircraftTracker::find_or_create_entry(uint32_t ICAO_address) {
    auto it = ui::find_entry(recent_, ICAO_address);
    if (it != recent_.end()) return *it;

    return recent_.emplace_front(ICAO_address);
}

void AircraftTracker::sort_entries_by_state() {
    recent_.sort([](const AircraftRecentEntry& left, const AircraftRecentEntry& right) {
        return left.state < right.state;
    });
}

void AircraftTracker::remove_expired_entries() {
    /* Entries are sorted with the oldest last, so walk back from the end. */
    auto it = recent_.rbegin();
    const auto end = recent_.rend();

    while (it != end) {
        if (it->state != ADSBAgeState::Expired) break;
        std::advance(it, 1);
    }

    recent_.erase(it.base(), recent_.end());
}

void AircraftTracker::age_entries(uint32_t delta, size_t max_entries) {
    for (auto& entry : recent_) entry.inc_age(delta);

    sort_entries_by_state();
    ui::truncate_entries(recent_, max_entries);
    remove_expired_entries();
}

bool AircraftTracker::handle_frame(adsb::AdsbFrame& frame,
                                   float amp,
                                   uint32_t rx_timestamp,
                                   AdsbLogEntry* log_out,
                                   bool* logged_out) {
    frames_seen_++;
    if (logged_out) *logged_out = false;

    uint32_t ICAO_address;
    const uint32_t crc = frame.check_CRC();

    if (crc != 0) {
        /* DF 0/4/5/16/20/21 overlay the address on the parity, so a clean frame
         * from a known aircraft leaves its ICAO address as the syndrome. An
         * unknown syndrome is just a corrupt frame. */
        if (ui::find_entry(recent_, crc) != recent_.end())
            ICAO_address = crc;
        else
            return false;
    } else {
        ICAO_address = frame.get_ICAO_address();
        if (ICAO_address == 0) return false;
    }

    frames_accepted_++;

    frame.set_rx_timestamp(rx_timestamp);

    AircraftRecentEntry& entry = find_or_create_entry(ICAO_address);
    const bool first_hit = (entry.hits == 0);
    entry.inc_hit();
    entry.reset_age();

    /* Smoothed amplitude, upstream's 1/16 exponential average — in float, so
     * that a run of sub-count frames averages instead of flooring to zero. */
    if (amp < 0.0f) amp = 0.0f;
    entry.amp = first_hit ? amp : (((entry.amp * 15.0f) + amp) / 16.0f);

    const uint8_t df = frame.get_DF();

    /* DF11 (all-call reply) arrives far too often to be worth a log line. */
    if (df == adsb::DF_ALL_CALL) return true;

    AdsbLogEntry log_entry;
    const uint8_t* raw_data = frame.get_raw_data();

    if (df & 0x10)
        log_entry.raw_data = to_string_hex_array(raw_data, 14);
    else {
        log_entry.raw_data = to_string_hex_array(raw_data, 7);
        log_entry.raw_data.append(14, ' ');
    }

    log_entry.icao = entry.icao_str;

    /* 17: extended squitter. 18: extended squitter, non-transponder. */
    if (df == adsb::DF_ADSB) {
        const uint8_t msg_type = frame.get_msg_type();
        const uint8_t msg_sub = frame.get_msg_sub();

        if (msg_type == 0) {
            /* No horizontal position, but altitude may still be there. */
            bool alt_valid = false;
            const int32_t altitude = adsb::decode_me_altitude(raw_data, alt_valid);
            if (alt_valid) {
                log_entry.pos.altitude = entry.pos.altitude = altitude;
                log_entry.pos.alt_valid = entry.pos.alt_valid = true;
            }
        } else if ((msg_type >= adsb::AIRCRAFT_ID_L) && (msg_type <= adsb::AIRCRAFT_ID_H)) {
            entry.set_callsign(adsb::decode_frame_id(frame));
            log_entry.callsign = entry.callsign;
        } else if (((msg_type >= adsb::AIRBORNE_POS_BARO_L) && (msg_type <= adsb::AIRBORNE_POS_BARO_H)) ||
                   ((msg_type >= adsb::AIRBORNE_POS_GPS_L) && (msg_type <= adsb::AIRBORNE_POS_GPS_H))) {
            entry.set_frame_pos(frame, raw_data[6] & 4);
            log_entry.pos = entry.pos;

            if (entry.pos.pos_valid) {
                entry.set_info_string(
                    "Alt:" + to_string_dec_int(entry.pos.altitude) +
                    " Lat:" + to_string_decimal(entry.pos.latitude, 2) +
                    " Lon:" + to_string_decimal(entry.pos.longitude, 2));
            }
        } else if (msg_type == adsb::AIRBORNE_VEL &&
                   msg_sub >= adsb::VEL_GND_SUBSONIC && msg_sub <= adsb::VEL_AIR_SUPERSONIC) {
            entry.set_frame_velo(frame);
            log_entry.vel = entry.velo;
            log_entry.vel_type = msg_sub;
        } else if (msg_type == adsb::AIRBORNE_OP_STATUS) {
            entry.sil = frame.get_sil_value();
        }
    }

    /* 0: short air-air. 4: surveillance altitude reply. 20: Comm-B altitude. */
    if (df == adsb::DF_SHORT_AIR_AIR || df == adsb::DF_SURV_ALT || df == adsb::DF_COMM_B_ALT) {
        bool alt_valid = false;
        const int32_t altitude = adsb::decode_ac13_altitude(raw_data, alt_valid);
        if (alt_valid) {
            log_entry.pos.altitude = entry.pos.altitude = altitude;
            log_entry.pos.alt_valid = entry.pos.alt_valid = true;
        }
    }

    /* Identity (squawk): DF5, DF21, and the ADS-B emergency/status message. */
    if (df == adsb::DF_SURV_IDENT || df == adsb::DF_EHS_SQUAWK ||
        (df == adsb::DF_ADSB && frame.get_msg_type() == 28 && frame.get_msg_sub() == 1)) {
        const uint8_t* s = (df == adsb::DF_ADSB) ? raw_data + 5 : raw_data + 2;
        log_entry.sqwk = entry.sqwk = adsb::decode_squawk(s);
    }

    if (df == adsb::DF_COMM_B_ALT || df == adsb::DF_EHS_SQUAWK) {
        if (raw_data[4] == 0x20) {  /* BDS 2,0 — aircraft identification */
            const std::string callsign = adsb::decode_frame_id(frame);
            if (callsign.find('#') == std::string::npos) {
                entry.set_callsign(callsign);
                log_entry.callsign = callsign;
            }
        }
    }

    log_entry.sil = entry.sil;

    if (log_out) *log_out = log_entry;
    if (logged_out) *logged_out = true;

    return true;
}

/* =========================================================================
 * Views
 * ========================================================================= */

namespace {

/* Recent-entries row, ported from upstream's RecentEntriesTable<> draw
 * specialization for AircraftRecentEntries. */
void draw_aircraft_row(const AircraftRecentEntry& entry,
                       const ui::Rect& target_rect,
                       ui::Painter& painter,
                       const ui::Style& style,
                       ui::RecentEntriesColumns& columns) {
    std::string entry_string;

    switch (entry.state) {
        case ADSBAgeState::Invalid:
        case ADSBAgeState::Current:
            entry_string = "";
            break;
        case ADSBAgeState::Recent:
            entry_string = STR_COLOR_LIGHT_GREY;
            break;
        default:
            entry_string = STR_COLOR_DARK_GREY;
            break;
    }

    std::string ipc = entry.callsign.empty() ? entry.icao_str + "   " : entry.callsign + " ";
    const size_t firstcolwidth = columns.at(0).second;
    ipc.resize(firstcolwidth, ' ');

    /* Upstream paints a small target bitmap over the last cell of the first
     * column when the position is known. bitmaps.hpp carries no such glyph, so
     * the same cell gets a '*'. */
    if (entry.pos.pos_valid && firstcolwidth > 0) ipc[firstcolwidth - 1] = '*';

    entry_string += ipc + to_string_dec_int(entry.pos.altitude / 100, 4);

    if (entry.velo.type == adsb::SPD_IAS && entry.pos.alt_valid) {
        /* IAS shown corrected to TAS, flagged with '*'. */
        const int32_t tas = entry.velo.speed +
                            (entry.pos.altitude * 2 * entry.velo.speed / 100000);
        entry_string += to_string_dec_int(tas, 4) + '*' +
                        to_string_dec_uint(entry.amp_display(), 3);
    } else {
        entry_string += to_string_dec_int(entry.velo.speed, 4) +
                        to_string_dec_uint(entry.amp_display(), 4);
    }

    entry_string += " " +
                    (entry.hits <= 999 ? to_string_dec_uint(entry.hits, 3) + " " : std::string{"1k+ "}) +
                    to_string_dec_uint(entry.age, 4);

    painter.draw_string(target_rect.location(), style, entry_string);
}

std::string map_tag_for(const AircraftRecentEntry& entry) {
    return trimr(entry.callsign.empty() ? entry.icao_str : entry.callsign);
}

}  // namespace

/* --- AdsbRxDetailsView ----------------------------------------------------- */

AdsbRxDetailsView::AdsbRxDetailsView(AdsbRxView& parent, const AircraftRecentEntry& entry)
    : parent_{parent},
      entry_{entry} {
    add_children({&labels_,
                  &text_icao_,
                  &text_callsign_,
                  &text_last_seen_,
                  &text_squawk_,
                  &text_sil_,
                  &text_infos_,
                  &text_info2_,
                  &opt_map_list_,
                  &button_see_map_,
                  &text_frame_pos_even_,
                  &text_frame_pos_odd_,
                  &db_note_});

    text_icao_.set(entry_.icao_str);

    opt_map_list_.on_change = [this](size_t, int32_t mf) {
        map_filter_ = static_cast<uint8_t>(mf);
        pos_history_.clear();
        if (map_view_) map_view_->clear_markers();
    };

    button_see_map_.on_select = [this](ui::Button&) { open_map(); };

    refresh_ui();
}

AdsbRxDetailsView::~AdsbRxDetailsView() {
    /* The map, if any, is owned by the navigation stack above this view and is
     * always popped before this destructor runs; nothing to release. */
}

void AdsbRxDetailsView::focus() {
    button_see_map_.focus();
}

ui::Color AdsbRxDetailsView::altitude_color(int32_t alt_ft) {
    struct Stop {
        int32_t alt;
        uint8_t r, g, b;
    };
    static const Stop stops[] = {
        {0, 220, 220, 220},    /* ground: light grey */
        {2000, 255, 255, 0},   /* yellow */
        {10000, 0, 255, 0},    /* green */
        {20000, 0, 255, 255},  /* cyan */
        {30000, 60, 60, 255},  /* blue, lightened so it reads on black */
        {40000, 255, 0, 255}   /* magenta */
    };
    constexpr int count = static_cast<int>(sizeof(stops) / sizeof(stops[0]));

    if (alt_ft <= stops[0].alt)
        return ui::Color(stops[0].r, stops[0].g, stops[0].b);
    if (alt_ft >= stops[count - 1].alt)
        return ui::Color(stops[count - 1].r, stops[count - 1].g, stops[count - 1].b);

    for (int i = 0; i < count - 1; i++) {
        if (alt_ft < stops[i + 1].alt) {
            const float t = static_cast<float>(alt_ft - stops[i].alt) /
                            static_cast<float>(stops[i + 1].alt - stops[i].alt);
            return ui::Color(
                static_cast<uint8_t>(stops[i].r + (stops[i + 1].r - stops[i].r) * t),
                static_cast<uint8_t>(stops[i].g + (stops[i + 1].g - stops[i].g) * t),
                static_cast<uint8_t>(stops[i].b + (stops[i + 1].b - stops[i].b) * t));
        }
    }

    return ui::Color(stops[0].r, stops[0].g, stops[0].b);
}

void AdsbRxDetailsView::open_map() {
    auto* nav = globals().nav;
    if (!nav) return;

    auto map = std::make_unique<AdsbMapView>(
        map_tag_for(entry_),
        entry_.pos.altitude,
        ui::GeoPos::alt_unit::FEET,
        ui::GeoPos::spd_unit::KNOTS,
        entry_.pos.latitude,
        entry_.pos.longitude,
        entry_.velo.heading,
        [this]() { map_view_ = nullptr; });

    AdsbMapView* raw = map.get();
    /* The host only ticks the top view, so the map has to drive the decoder
     * itself; this view and the app below it are both still alive on the
     * navigation stack while the map is showing. */
    raw->on_tick = [this, raw]() {
        map_view_ = raw;
        parent_.pump();
        tick_++;
        if ((tick_ % 30) == 0) {
            parent_.tick_second();
            refresh_map();
        }
    };

    map_view_ = raw;
    nav->push(std::move(map));
}

void AdsbRxDetailsView::refresh_trail_markers() {
    if (!map_view_) return;

    map_view_->clear_markers();
    for (const auto& p : pos_history_) {
        ui::GeoMarker marker{};
        marker.lat = p.lat;
        marker.lon = p.lon;
        marker.angle = p.heading;
        marker.tag = "";
        marker.color = altitude_color(p.altitude);
        map_view_->store_marker(marker);
    }
}

void AdsbRxDetailsView::add_map_trail(const AircraftRecentEntry& entry) {
    if (!map_view_ || !entry.pos.pos_valid) return;

    if (!pos_history_.empty()) {
        const auto& last = pos_history_.back();
        /* About 1 km; keeps a circling or taxiing aircraft from filling the
         * trail with points on top of each other. */
        constexpr float thresh_deg = 0.009f;
        constexpr float thresh_sq = thresh_deg * thresh_deg;

        const float d_lat = entry.pos.latitude - last.lat;
        const float d_lon = entry.pos.longitude - last.lon;
        if ((d_lat * d_lat) + (d_lon * d_lon) < thresh_sq) return;
    }

    if (pos_history_.size() >= 30) pos_history_.erase(pos_history_.begin());
    pos_history_.push_back({entry.pos.latitude, entry.pos.longitude,
                            entry.velo.heading, entry.pos.altitude});
    refresh_trail_markers();
}

bool AdsbRxDetailsView::add_map_marker(const AircraftRecentEntry& entry) {
    if (!map_view_) return false;
    if (map_filter_ == 1) return false;  /* "only me" */

    ui::GeoMarker marker{};
    marker.lat = entry.pos.latitude;
    marker.lon = entry.pos.longitude;
    marker.angle = entry.velo.heading;
    marker.tag = map_tag_for(entry);
    marker.color = altitude_color(entry.pos.altitude);

    return map_view_->store_marker(marker) == ui::MARKER_STORED;
}

void AdsbRxDetailsView::refresh_map() {
    if (!map_view_) return;

    if (const auto* live = parent_.tracker().find(entry_.ICAO_address))
        entry_ = *live;

    if (map_filter_ == 1) {
        add_map_trail(entry_);
    } else {
        map_view_->clear_markers();
        for (const auto& other : parent_.tracker().entries()) {
            if (other.key() == entry_.ICAO_address) continue;
            if (!other.pos.pos_valid) continue;
            if (other.state > ADSBAgeState::Recent) continue;
            if (!add_map_marker(other)) break;  /* marker list full */
        }
    }

    map_view_->update_tag(map_tag_for(entry_));
    map_view_->update_position(entry_.pos.latitude, entry_.pos.longitude,
                               entry_.velo.heading, entry_.pos.altitude,
                               entry_.get_ground_speed());
}

void AdsbRxDetailsView::on_frame_sync() {
    View::on_frame_sync();

    /* Reaching here means this view is on top, so any map it pushed has been
     * popped and destroyed. */
    map_view_ = nullptr;

    parent_.pump();

    tick_++;
    if ((tick_ % 60) != 0) return;

    parent_.tick_second();

    if (const auto* live = parent_.tracker().find(entry_.ICAO_address))
        entry_ = *live;

    refresh_ui();
}

void AdsbRxDetailsView::refresh_ui() {
    static const char speed_type_msg[][6] = {" Spd:", " IAS:", " TAS:"};

    text_callsign_.set(entry_.callsign.empty() ? "-" : entry_.callsign);

    if (entry_.age < 60)
        text_last_seen_.set(to_string_dec_uint(entry_.age) + " seconds ago");
    else
        text_last_seen_.set(to_string_dec_uint(entry_.age / 60) + " minutes ago");

    text_squawk_.set(entry_.sqwk ? to_string_dec_uint(entry_.sqwk, 4, '0') : "-");
    text_sil_.set(entry_.sil ? to_string_dec_uint(entry_.sil) : "-");

    text_infos_.set(entry_.info_string.empty() ? "No position yet" : entry_.info_string);

    if (entry_.velo.valid && entry_.velo.heading < 360)
        text_info2_.set("Hdg:" + to_string_dec_uint(entry_.velo.heading) +
                        speed_type_msg[entry_.velo.type] +
                        to_string_dec_int(entry_.velo.speed) +
                        " VS:" + to_string_dec_int(entry_.velo.v_rate));
    else
        text_info2_.set("No velocity yet");

    text_frame_pos_even_.set(to_string_hex_array(entry_.frame_pos_even.get_raw_data(), 14));
    text_frame_pos_odd_.set(to_string_hex_array(entry_.frame_pos_odd.get_raw_data(), 14));
}

/* --- AdsbRxView ------------------------------------------------------------ */

namespace {

/* Fallback block size, used only if the gapless tap could not be opened. The
 * whole spectrum snapshot, as this app polled before the migration. */
constexpr size_t kPumpSamples = 4096;

/* How much air the tap must hold. A UI frame is 16 ms; 250 ms is ~15 frames of
 * slack, which covers the hitches a desktop takes (a map tile decode, a window
 * resize) without pretending to survive a hang. Left at the tap's own default
 * deliberately — this app has no reason to want a different one, and saying so
 * here would be a second constant to keep in step. */
constexpr double kTapSeconds = radio::RawSampleTap::kDefaultHistorySeconds;

std::string format_rate(double hz) {
    if (!(hz > 0.0)) return "-";
    return to_string_decimal(static_cast<float>(hz / 1e6), 2) + "M";
}

}  // namespace

radio::RatePreference AdsbRxView::rate_preference() {
    radio::RatePreference want;

    /* Mode S is 1 Mbit/s pulse-position modulation, so the bit machine needs
     * two samples per bit and cannot work below 2 Msps at all. */
    want.minimum_hz = adsb::AdsbDemod::kNativeSampleRate;

    /* Eight samples per bit. The gain is not in the bit decisions — those are
     * still made at 2 Msps after the decimator — but in where the pulse edges
     * land: a 0.5 us pulse is resolved on a 1/8 us grid instead of a 1/2 us
     * one, so a burst that arrives out of phase with the sampling clock loses
     * far less contrast, and the boxcar averages four samples of noise into
     * each one the detector sees. */
    want.ideal_hz = 4.0 * adsb::AdsbDemod::kNativeSampleRate;

    /* Past 8 Msps there is nothing left to buy. The pulse edge is already
     * resolved to an eighth of its own width, and the extra bandwidth is real
     * cost: 16 Msps is 128 MB/s through the tap and twice the USB traffic, on
     * a bus this radio shares with nothing else. */
    want.max_useful_hz = 4.0 * adsb::AdsbDemod::kNativeSampleRate;

    /* Whole multiples of the native rate keep the decimator exact and let the
     * magnitude interpolator bypass entirely, so no sample is ever built by
     * interpolating between two others. */
    want.prefer_multiple_of_hz = adsb::AdsbDemod::kNativeSampleRate;

    return want;
}

namespace {

/* The rate to ask the receiver for: the best the CONNECTED radio can do within
 * what ADS-B actually needs, rather than the 2 Msps constant that was sized for
 * a PortaPack. A B200 gives 8 Msps, an RTL-SDR 2, and a device that cannot
 * reach 2 at all still gets its own closest rate — the app then decodes badly
 * instead of not at all, and says so on screen. */
double preferred_sampling_rate() {
    const auto want = AdsbRxView::rate_preference();

    if (auto* r = globals().radio)
        return radio::choose_rx_rate(r->caps(), want).rate_hz;

    /* No radio object at all: choose_rx_rate on an empty DeviceCaps hands back
     * the ideal, which is the right thing to have configured if one appears. */
    return radio::choose_rx_rate(radio::DeviceCaps{}, want).rate_hz;
}

}  // namespace

AdsbRxView::AdsbRxView()
    : recent_{tracker_.entries()},
      recent_view_{columns_, recent_} {
    add_children({&labels_,
                  &field_gain_,
                  &level_meter_,
                  &dot_frame_,
                  &dot_good_frame_,
                  &check_log_,
                  &text_status_,
                  &recent_view_,
                  &text_tap_,
                  &text_air_});

    /* Below the two control rows, above the two-line note at the bottom. */
    recent_view_.set_parent_rect({0, 44, ui::screen_width, 228});
    recent_view_.table().on_draw = draw_aircraft_row;

    recent_view_.on_select = [this](const AircraftRecentEntry& entry) {
        if (auto* nav = globals().nav)
            nav->push(std::make_unique<AdsbRxDetailsView>(*this, entry));
    };

    receiver_ = globals().receiver;

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
    }

    if (receiver_) field_gain_.set_value(static_cast<int32_t>(receiver_->gain()), false);
    field_gain_.on_change = [this](int32_t db) {
        if (receiver_) receiver_->set_gain(db);
    };

    check_log_.on_select = [this](ui::Checkbox&, bool v) { set_logging(v); };

    refresh_status();
}

AdsbRxView::~AdsbRxView() {
    /* The receive stream is left running, as the other RX apps do — but the
     * gapless tap is not. Nothing would drain it once this view is gone, so it
     * would fill once and then sit there costing the DSP thread a 128 MB/s
     * copy at 16 Msps for no reader at all. Closed here rather than in
     * on_hide(), which also runs when this view merely pushes its own details
     * page on top of itself and keeps decoding underneath it. */
    if (receiver_) receiver_->disable_raw_tap();

    if (log_file_) log_file_->flush();
}

void AdsbRxView::focus() {
    field_gain_.focus();
}

void AdsbRxView::on_show() {
    View::on_show();

    receiver_ = globals().receiver;
    if (!receiver_) return;

    /* ADS-B is a fixed service: 1090 MHz, no demodulator in the audio chain —
     * this app does its own. The rate is not fixed: it comes from what the
     * attached radio can actually do. */
    receiver_->set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    receiver_->set_sampling_rate(preferred_sampling_rate());
    /* The 1090 MHz signal is ~2 MHz wide but we sample at up to 8 Msps for
     * pulse-timing resolution, so the capability default (an analog filter
     * about as wide as the rate) passes several times the noise bandwidth the
     * decoder needs. A 4 MHz filter still passes the whole ADS-B pulse
     * spectrum with margin while cutting ~3 dB of noise at 8 Msps — which is
     * exactly the margin a weak, distant aircraft's position frames need to
     * clear CRC. Must follow set_sampling_rate, which clears the hint. */
    receiver_->set_rx_bandwidth_hint(4'000'000.0);
    receiver_->set_target_frequency(kFrequency);
    if (!receiver_->running()) receiver_->start();

    /* Subscribe after start(), which is where the tap is sized from the rate
     * the hardware actually accepted. Guarded because on_show() runs again
     * every time the details or map view is popped back off, and re-opening
     * would throw away whatever had arrived in the meantime and report it,
     * correctly but pointlessly, as a gap. */
    if (!receiver_->raw_tap_enabled()) receiver_->enable_raw_tap(kTapSeconds);

    demod_.set_input_rate(receiver_->sampling_rate());
    field_gain_.set_value(static_cast<int32_t>(receiver_->gain()), false);
    refresh_status();
}

void AdsbRxView::pump() {
    if (!receiver_) return;

    demod_.set_input_rate(receiver_->sampling_rate());

    if (receiver_->raw_tap_enabled()) {
        const auto block = receiver_->raw_tap().read(samples_);

        if (block.lost_before != 0) {
            /* The stream really broke: the machine did not drain the tap in
             * time and samples were discarded. Everything the demodulator is
             * holding — the 17-sample preamble history, a half-decoded frame,
             * the decimator's partial average — refers to samples that are not
             * adjacent to these. Carrying it across would do worse than lose
             * the interrupted frame: a spliced history can satisfy the
             * preamble template and fabricate a burst, and a frame left
             * mid-decode would swallow the first bits of the next real one.
             *
             * So reset, but only here. Under the old snapshot tap every block
             * was a splice and this ran 60 times a second, which is precisely
             * what made a frame straddling two blocks unrecoverable. */
            samples_lost_ += block.lost_before;
            gaps_++;
            demod_.reset();
        }

        if (block.samples == 0) return;
    } else {
        /* No gapless tap — nothing opened it, or it could not be. Fall back to
         * the snapshot every other decoder still uses, which means resetting
         * per block again, because successive snapshots are not adjacent in
         * time. Degraded, not broken, and the status line says which one is in
         * force. */
        if (!receiver_->take_spectrum_samples(samples_, kPumpSamples)) return;
        if (samples_.empty()) return;
        demod_.reset();
    }

    samples_decoded_ += samples_.size();

    demod_.process(samples_.data(), samples_.size(),
                   [this](const adsb::AdsbFrame& f, float amp) {
                       adsb::AdsbFrame frame = f;
                       dot_frame_.toggle();

                       AdsbLogEntry log_entry;
                       bool logged = false;
                       const bool accepted = tracker_.handle_frame(
                           frame, amp, seconds_, &log_entry, &logged);

                       if (!accepted) return;
                       dot_good_frame_.toggle();
                       if (logged && logging_) write_log(log_entry);
                   });
}

void AdsbRxView::tick_second() {
    seconds_++;
    tracker_.age_entries(1);
    dot_frame_.reset();
    dot_good_frame_.reset();
}

void AdsbRxView::on_frame_sync() {
    View::on_frame_sync();

    pump();

    frame_counter_++;

    if ((frame_counter_ % 10) == 0 && receiver_) {
        /* -100 dBFS is roughly a B200's noise floor at moderate gain. */
        const float db = receiver_->rf_level_db();
        const float frac = (db + 100.0f) / 100.0f;
        const long v = std::lround(static_cast<double>(frac) * 255.0);
        level_meter_.set_value(static_cast<uint8_t>(std::clamp<long>(v, 0, 255)));
    }

    if ((frame_counter_ % 60) == 0) {
        tick_second();
        refresh_status();
        recent_view_.table().set_dirty();
    }
}

void AdsbRxView::refresh_status() {
    if (!log_error_.empty()) {
        text_status_.set(log_error_);
    } else {
        text_status_.set("Ok:" + to_string_dec_uint(tracker_.frames_accepted()) +
                         " Raw:" + to_string_dec_uint(tracker_.frames_seen()) +
                         " AC:" + to_string_dec_uint(static_cast<uint64_t>(recent_.size())));
    }

    refresh_tap_status();
}

void AdsbRxView::refresh_tap_status() {
    if (!receiver_) {
        text_tap_.set("No radio.");
        text_air_.set("");
        return;
    }

    /* Line 1: which tap, at what rate, and how the rate is being brought back
     * to the demodulator's native 2 Msps. The rate is chosen from the radio's
     * caps and so is not knowable from the source. */
    const double rate = receiver_->sampling_rate();
    std::string tap = gapless() ? "Gapless " : "Snapshot ";
    tap += format_rate(rate);

    const size_t decim = demod_.input_decimation();
    if (decim > 1) tap += " /" + to_string_dec_uint(static_cast<uint64_t>(decim));

    text_tap_.set(tap);

    /* Line 2: the number the old fixed note guessed at. "Air" is the fraction
     * of the samples the radio delivered that actually reached the
     * demodulator, so the snapshot path's ~12% and a struggling gapless tap
     * are on the same scale and directly comparable. Gaps are counted
     * separately from samples because one long stall and a thousand small ones
     * are different problems. */
    if (!gapless()) {
        /* The snapshot hands over a fixed 4096 samples per UI frame whatever
         * the rate, and there is no counter for what it overwrote in between,
         * so this is arithmetic, not a measurement — and it is labelled as an
         * estimate for exactly that reason. */
        const double duty = (rate > 0.0) ? (static_cast<double>(kPumpSamples) * 60.0 / rate) : 0.0;
        text_air_.set("Air ~" + to_string_decimal(static_cast<float>(duty * 100.0), 1) +
                      "% (no tap)");
        return;
    }

    std::string air = "Air " +
                      to_string_decimal(static_cast<float>(air_fraction() * 100.0), 1) + "%";
    if (samples_lost_ != 0) {
        air += " lost " + to_string_dec_uint(samples_lost_) +
               " in " + to_string_dec_uint(gaps_);
    }
    text_air_.set(air);
}

void AdsbRxView::set_logging(bool enabled) {
    logging_ = enabled;
    log_error_.clear();

    if (!enabled) {
        if (log_file_) {
            log_file_->flush();
            log_file_.reset();
        }
        return;
    }

    const std::string dir = core::data_directory() + "/LOGS";
    if (!core::ensure_directory(dir)) {
        log_error_ = "no log dir";
        logging_ = false;
        return;
    }

    log_path_ = dir + "/ADSB.TXT";
    log_file_ = std::make_unique<std::ofstream>(log_path_, std::ios::app);
    if (!log_file_->is_open()) {
        log_error_ = "cannot open log";
        log_file_.reset();
        logging_ = false;
    }
}

void AdsbRxView::write_log(const AdsbLogEntry& entry) {
    if (!log_file_) return;

    *log_file_ << format_adsb_log_line(entry) << "\n";
    if (!log_file_->good()) {
        log_error_ = "log write failed";
        log_file_.reset();
        logging_ = false;
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_adsbrx{{"adsbrx", "ADS-B", app::Category::Receive,
                                 ui::Color::green(), &ui::bitmap_icon_adsb,
                                 [] { return std::make_unique<app::AdsbRxView>(); }}};
}  // namespace
