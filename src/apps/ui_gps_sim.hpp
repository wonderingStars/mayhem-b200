/*
 * mayhem-b200 — GPS L1 C/A baseband simulator (transmit).
 *
 * Ported from Mayhem's GPS Sim app (firmware/application/external/gpssim/) and
 * its baseband replay processor (firmware/baseband/proc_gps_sim.cpp).
 *
 * IMPORTANT difference from upstream, stated plainly: on the PortaPack the GPS
 * Sim app is only a *file replayer*. The LPC43xx is far too small to synthesise
 * a GPS constellation, so the app streams a pre-generated ".C8" IQ file that the
 * operator produced on a PC with osqzss/gps-sdr-sim. The signal-generation code
 * lives in gps-sdr-sim, not in the firmware.
 *
 * A B200 host has the CPU that the LPC43xx lacked, so this port does on the host
 * what upstream delegates to gps-sdr-sim: it generates the L1 C/A baseband
 * directly and feeds it to the transmitter as an IqSource. The three pieces the
 * task named are ported faithfully from gps-sdr-sim / IS-GPS-200:
 *
 *   1. C/A Gold-code generation      (codegen(), IS-GPS-200 Table 3-Ia)
 *   2. LNAV nav-message framing       (TLM/HOW words, subframe IDs, parity)
 *   3. per-satellite BPSK modulation  (nav-data XOR C/A, summed to complex IQ)
 *
 * WHAT THIS PORT DOES NOT DO (honesty): gps-sdr-sim also ingests a broadcast
 * ephemeris (RINEX) and propagates each satellite's orbit to turn a static
 * lat/lon/alt into per-satellite pseudoranges, code phases and Doppler, so that
 * a real receiver computes the *chosen* fix. That orbit/geometry pipeline is not
 * reproduced here — it needs ephemeris data this port does not carry, and it
 * cannot be verified without RF and a receiver. This port emits a structurally
 * correct L1 C/A constellation (correct codes, framing, parity, BPSK) with a
 * configurable set of PRNs; it is not a geometrically consistent position fix.
 *
 * LEGALITY: transmitting on the GPS L1 band (1575.42 MHz) is illegal almost
 * everywhere and can disrupt navigation and timing far beyond the operator. The
 * view shows a warning and never transmits without an explicit user action.
 *
 * The encoder (namespace gps) is self-contained and depends only on the standard
 * library, so it can be exercised without the UI. Define MB200_GPS_ENCODER_ONLY
 * before including this header to get just the encoder.
 *
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2020 Shao
 * Copyright (C) 2015-2020 osqzss (gps-sdr-sim algorithms)
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_GPS_SIM_H__
#define __MB200_UI_GPS_SIM_H__

#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gps {

/* --- constants (gps-sdr-sim gpssim.h) ------------------------------------- */

inline constexpr int CA_SEQ_LEN = 1023;        /* chips per C/A code period    */
inline constexpr int N_DWRD_SBF = 10;          /* 30-bit words per subframe    */
inline constexpr int N_SBF = 5;                /* subframes per frame          */
inline constexpr int N_WORDS_FRAME = N_SBF * N_DWRD_SBF;  /* 50               */
inline constexpr int N_BITS_FRAME = N_WORDS_FRAME * 30;   /* 1500 data bits    */
inline constexpr int MAX_PRN = 32;
inline constexpr double CA_CHIP_RATE = 1023000.0;   /* 1.023 Mcps             */
inline constexpr double L1_FREQ = 1575420000.0;     /* Hz                     */
inline constexpr double GPS_PI = 3.14159265358979323846;

/* --- bit counting (gps-sdr-sim countBits(), verbatim) --------------------- */

inline uint32_t count_bits(uint32_t v) {
    static const int S[] = {1, 2, 4, 8, 16};
    static const uint32_t B[] = {0x55555555u, 0x33333333u, 0x0F0F0F0Fu,
                                 0x00FF00FFu, 0x0000FFFFu};
    uint32_t c = v;
    c = ((c >> S[0]) & B[0]) + (c & B[0]);
    c = ((c >> S[1]) & B[1]) + (c & B[1]);
    c = ((c >> S[2]) & B[2]) + (c & B[2]);
    c = ((c >> S[3]) & B[3]) + (c & B[3]);
    c = ((c >> S[4]) & B[4]) + (c & B[4]);
    return c;
}

/* --- C/A Gold code (gps-sdr-sim codegen(), verbatim) ----------------------
 *
 * Two 10-stage LFSRs held as +/-1 (init all -1 == all ones). G1 taps 3,10;
 * G2 taps 2,3,6,8,9,10 (IS-GPS-200). The PRN is selected by the per-PRN delay
 * applied to the G2 sequence. Output chips are 0/1: ca = (1 - g1*g2)/2. */
inline void ca_code(int prn, int8_t* ca) {
    static const int delay[MAX_PRN] = {
        5, 6, 7, 8, 17, 18, 139, 140, 141, 251,
        252, 254, 255, 256, 257, 258, 469, 470, 471, 472,
        473, 474, 509, 512, 513, 514, 515, 516, 859, 860,
        861, 862};

    if (prn < 1 || prn > MAX_PRN) {
        for (int i = 0; i < CA_SEQ_LEN; i++) ca[i] = 0;
        return;
    }

    int g1[CA_SEQ_LEN];
    int g2[CA_SEQ_LEN];
    int r1[N_DWRD_SBF];
    int r2[N_DWRD_SBF];

    for (int i = 0; i < N_DWRD_SBF; i++) r1[i] = r2[i] = -1;

    for (int i = 0; i < CA_SEQ_LEN; i++) {
        g1[i] = r1[9];
        g2[i] = r2[9];
        const int c1 = r1[2] * r1[9];
        const int c2 = r2[1] * r2[2] * r2[5] * r2[7] * r2[8] * r2[9];
        for (int j = 9; j > 0; j--) {
            r1[j] = r1[j - 1];
            r2[j] = r2[j - 1];
        }
        r1[0] = c1;
        r2[0] = c2;
    }

    for (int i = 0, j = CA_SEQ_LEN - delay[prn - 1]; i < CA_SEQ_LEN; i++, j++)
        ca[i] = static_cast<int8_t>((1 - g1[i] * g2[j % CA_SEQ_LEN]) / 2);
}

inline std::array<int8_t, CA_SEQ_LEN> ca_code(int prn) {
    std::array<int8_t, CA_SEQ_LEN> out{};
    ca_code(prn, out.data());
    return out;
}

/* --- nav-message parity ---------------------------------------------------
 *
 * compute_checksum: gps-sdr-sim's computeChecksum(), verbatim. `source` holds
 * the 24 data bits in bits 29..6 and the previous word's last two bits D29*,D30*
 * in bits 31/30. `nib` is set for the 2nd and 10th words of a subframe (the two
 * that carry non-information-bearing bits whose value is solved so parity bits
 * 29/30 come out zero). Returns the 30-bit transmitted word (data inverted if
 * D30* is set, plus the 6 parity bits). */
inline uint32_t compute_checksum(uint32_t source, bool nib) {
    static const uint32_t bmask[6] = {
        0x3B1F3480u, 0x1D8F9A40u, 0x2EC7CD00u,
        0x1763E680u, 0x2BB1F340u, 0x0B7A89C0u};

    uint32_t d = source & 0x3FFFFFC0u;
    const uint32_t D29 = (source >> 31) & 0x1u;
    const uint32_t D30 = (source >> 30) & 0x1u;

    if (nib) {
        if ((D30 + count_bits(bmask[4] & d)) % 2) d ^= (0x1u << 6);
        if ((D29 + count_bits(bmask[5] & d)) % 2) d ^= (0x1u << 7);
    }

    uint32_t D = d;
    if (D30) D ^= 0x3FFFFFC0u;

    D |= ((D29 + count_bits(bmask[0] & d)) % 2) << 5;
    D |= ((D30 + count_bits(bmask[1] & d)) % 2) << 4;
    D |= ((D29 + count_bits(bmask[2] & d)) % 2) << 3;
    D |= ((D30 + count_bits(bmask[3] & d)) % 2) << 2;
    D |= ((D30 + count_bits(bmask[4] & d)) % 2) << 1;
    D |= ((D29 + count_bits(bmask[5] & d)) % 2);

    D &= 0x3FFFFFFFu;
    return D;
}

/* Receiver-side parity check, implemented directly from the IS-GPS-200
 * Table 20-XIV parity equations (independent of compute_checksum's bit-mask
 * form). `word` is a transmitted 30-bit word; D29s/D30s are the previous word's
 * two LSBs. Returns true when the 6 parity bits are self-consistent. */
inline bool parity_ok(uint32_t word, uint32_t D29s, uint32_t D30s) {
    /* IS-GPS-200 Table 20-XIV, in terms of source data bits d1..d24. */
    static const int eq[6][15] = {
        {1, 2, 3, 5, 6, 10, 11, 12, 13, 14, 17, 18, 20, 23, 0},   /* D25 */
        {2, 3, 4, 6, 7, 11, 12, 13, 14, 15, 18, 19, 21, 24, 0},   /* D26 */
        {1, 3, 4, 5, 7, 8, 12, 13, 14, 15, 16, 19, 20, 22, 0},    /* D27 */
        {2, 4, 5, 6, 8, 9, 13, 14, 15, 16, 17, 20, 21, 23, 0},    /* D28 */
        {1, 3, 5, 6, 7, 9, 10, 14, 15, 16, 17, 18, 21, 22, 24},   /* D29 */
        {3, 5, 6, 8, 9, 10, 11, 13, 15, 19, 22, 23, 24, 0, 0}};   /* D30 */
    /* Which previous-word bit each parity bit is seeded with. */
    static const uint32_t seed_is_D29[6] = {1, 0, 1, 0, 0, 1};

    /* Transmitted bit D_n = bit (30-n) of the word (n = 1..30). */
    auto Dn = [word](int n) -> uint32_t { return (word >> (30 - n)) & 0x1u; };
    /* Recover source data bits d_n = D_n XOR D30* (n = 1..24). */
    uint32_t d[25];
    for (int n = 1; n <= 24; n++) d[n] = Dn(n) ^ D30s;

    for (int k = 0; k < 6; k++) {
        uint32_t p = seed_is_D29[k] ? D29s : D30s;
        for (int t = 0; t < 15; t++) {
            const int n = eq[k][t];
            if (n == 0) break;
            p ^= d[n];
        }
        if (p != Dn(25 + k)) return false;
    }
    return true;
}

/* --- LNAV frame assembly --------------------------------------------------
 *
 * Builds one 5-subframe frame (50 words) of LNAV data into dwrd[]. Each 30-bit
 * word carries valid parity, chained through prevword the way the receiver
 * expects: word 0 of subframe 1 is seeded with `prevword`'s two LSBs and each
 * subsequent word with the previous word's.
 *
 * The information payload beyond framing is intentionally minimal — the TLM
 * preamble (0x8B), the HOW (TOW-count + subframe ID), and the transmission week
 * in subframe 1 word 3 — with all other data fields zero. That is enough to be a
 * structurally valid, parity-correct LNAV stream; it is NOT a populated
 * ephemeris/almanac (see the file header). Returns the last word, for chaining
 * the next frame. */
inline uint32_t build_frame(uint32_t* dwrd, uint16_t week, uint32_t tow6_start,
                            uint32_t prevword) {
    uint32_t prev = prevword;
    for (int isbf = 0; isbf < N_SBF; isbf++) {
        /* HOW TOW-count is the count at the start of the next subframe. */
        const uint32_t tow6 = (tow6_start + static_cast<uint32_t>(isbf) + 1) & 0x1FFFFu;
        const uint32_t sfid = static_cast<uint32_t>(isbf) + 1;

        for (int iwrd = 0; iwrd < N_DWRD_SBF; iwrd++) {
            uint32_t data24 = 0;  /* 24 data bits, bit23 = first transmitted */

            if (iwrd == 0) {
                /* TLM: 8-bit preamble 0x8B in the top of the data field. */
                data24 = 0x8B0000u;
            } else if (iwrd == 1) {
                /* HOW: TOW-count (17 bits) at d1..d17, subframe ID at d20..d22. */
                data24 = ((tow6 & 0x1FFFFu) << 7) | ((sfid & 0x7u) << 2);
            } else if (isbf == 0 && iwrd == 2) {
                /* Subframe 1 word 3: transmission week number (10 bits). */
                data24 = (static_cast<uint32_t>(week) & 0x3FFu) << 14;
            }

            uint32_t sbfwrd = (data24 << 6) | ((prev << 30) & 0xC0000000u);
            const bool nib = (iwrd == 1) || (iwrd == 9);
            const uint32_t word = compute_checksum(sbfwrd, nib);

            dwrd[isbf * N_DWRD_SBF + iwrd] = word;
            prev = word;
        }
    }
    return prev;
}

/* --- per-satellite BPSK signal generator ---------------------------------
 *
 * Each satellite BPSK-modulates its nav data bit (50 bps) XORed with its C/A
 * code (1.023 Mcps) onto a carrier at its Doppler offset; the satellites are
 * summed into a complex baseband centred at L1 (the transmitter's LO/NCO put it
 * on 1575.42 MHz). Nav-data and code are mapped to +/-1 as gps-sdr-sim does
 * (bit*2-1, code*2-1). The output is normalised so the summed magnitude stays
 * within unit scale.
 *
 * The nav-data bit stream is shared across satellites (this port broadcasts one
 * generic LNAV frame, not per-SV ephemeris — see the file header); each
 * satellite has its own C/A code, code-phase and carrier-phase state so the
 * per-satellite modulation is independent. */
struct SatConfig {
    int prn{1};
    float power{1.0f};
    double doppler_hz{0.0};
    double code_phase_chips{0.0};
};

class SignalGenerator {
   public:
    void configure(double sample_rate_hz, uint16_t week, uint32_t tow6_start,
                   const std::vector<SatConfig>& sats) {
        sample_rate_ = sample_rate_hz > 0 ? sample_rate_hz : 2600000.0;
        week_ = week;
        tow6_start_ = tow6_start;

        sats_.clear();
        total_power_ = 0.0f;
        for (const auto& c : sats) {
            if (c.prn < 1 || c.prn > MAX_PRN) continue;
            SatState s;
            s.cfg = c;
            ca_code(c.prn, s.code.data());
            s.code_phase = c.code_phase_chips;
            while (s.code_phase >= CA_SEQ_LEN) s.code_phase -= CA_SEQ_LEN;
            while (s.code_phase < 0) s.code_phase += CA_SEQ_LEN;
            /* Carrier as a rotating unit phasor: no per-sample trig. */
            s.carrier = std::complex<double>(1.0, 0.0);
            const double w = 2.0 * GPS_PI * c.doppler_hz / sample_rate_;
            s.carrier_inc = std::complex<double>(std::cos(w), std::sin(w));
            sats_.push_back(std::move(s));
            total_power_ += (c.power > 0 ? c.power : 0.0f);
        }
        if (total_power_ <= 0.0f) total_power_ = 1.0f;

        reset_timing();
    }

    void reset() { reset_timing(); }

    size_t satellite_count() const { return sats_.size(); }
    double sample_rate() const { return sample_rate_; }

    /* Fills out[0..count-1] with complex baseband. Returns count. */
    size_t generate(std::complex<float>* out, size_t count) {
        const double chip_step = CA_CHIP_RATE / sample_rate_;
        const float inv_norm = 1.0f / total_power_;

        for (size_t s = 0; s < count; s++) {
            const float data_pm = nav_bits_[nav_bit_index_] ? 1.0f : -1.0f;

            std::complex<float> acc{0.0f, 0.0f};
            for (auto& sat : sats_) {
                const int chip = sat.code[static_cast<int>(sat.code_phase)];
                const float code_pm = chip ? 1.0f : -1.0f;
                const float sym = data_pm * code_pm;

                acc += sat.cfg.power * sym *
                       std::complex<float>(static_cast<float>(sat.carrier.real()),
                                           static_cast<float>(sat.carrier.imag()));

                sat.code_phase += chip_step * (1.0 + sat.cfg.doppler_hz / L1_FREQ);
                if (sat.code_phase >= CA_SEQ_LEN) sat.code_phase -= CA_SEQ_LEN;

                sat.carrier *= sat.carrier_inc;
            }

            out[s] = acc * inv_norm;

            /* Re-normalise the rotating phasors periodically so rounding does
             * not let their magnitude drift from unity. */
            if (++renorm_counter_ >= 4096) {
                renorm_counter_ = 0;
                for (auto& sat : sats_) {
                    const double m = std::abs(sat.carrier);
                    if (m > 1e-9) sat.carrier /= m;
                }
            }

            /* Advance the shared nav-data timing at the nominal chip rate:
             * 1023 chips = 1 code period (1 ms), 20 code periods = 1 data bit
             * (50 bps), 1500 data bits = 1 frame (30 s). */
            global_code_phase_ += chip_step;
            while (global_code_phase_ >= CA_SEQ_LEN) {
                global_code_phase_ -= CA_SEQ_LEN;
                if (++code_period_ >= 20) {
                    code_period_ = 0;
                    if (++nav_bit_index_ >= N_BITS_FRAME) {
                        nav_bit_index_ = 0;
                        advance_frame();
                    }
                }
            }
        }
        return count;
    }

   private:
    struct SatState {
        SatConfig cfg{};
        std::array<int8_t, CA_SEQ_LEN> code{};
        double code_phase{0.0};
        std::complex<double> carrier{1.0, 0.0};
        std::complex<double> carrier_inc{1.0, 0.0};
    };

    void reset_timing() {
        tow6_ = tow6_start_;
        prevword_ = 0;
        global_code_phase_ = 0.0;
        code_period_ = 0;
        nav_bit_index_ = 0;
        renorm_counter_ = 0;
        build_current_frame();
    }

    void build_current_frame() {
        uint32_t dwrd[N_WORDS_FRAME];
        prevword_ = build_frame(dwrd, week_, tow6_, prevword_);
        unpack_frame(dwrd);
    }

    void advance_frame() {
        tow6_ += N_SBF;  /* one frame = 5 subframes = 5 TOW counts (30 s) */
        build_current_frame();
    }

    void unpack_frame(const uint32_t* dwrd) {
        for (int w = 0; w < N_WORDS_FRAME; w++)
            for (int b = 0; b < 30; b++)
                nav_bits_[w * 30 + b] =
                    static_cast<int8_t>((dwrd[w] >> (29 - b)) & 0x1u);
    }

    double sample_rate_{2600000.0};
    uint16_t week_{0};
    uint32_t tow6_start_{0};
    uint32_t tow6_{0};
    uint32_t prevword_{0};
    float total_power_{1.0f};

    std::array<int8_t, N_BITS_FRAME> nav_bits_{};
    double global_code_phase_{0.0};
    int code_period_{0};
    int nav_bit_index_{0};
    int renorm_counter_{0};

    std::vector<SatState> sats_{};
};

}  // namespace gps

#ifndef MB200_GPS_ENCODER_ONLY

#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_navigation.hpp"
#include "ui_widget.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace app {

/* A thread-safe ring the DSP thread drains and the generator fills. Kept small
 * and local (per the porting rule against new shared files). */
class GpsSampleRing {
   public:
    explicit GpsSampleRing(size_t capacity = 1u << 16) : buf_(capacity) {}

    void clear() {
        std::lock_guard<std::mutex> lock(m_);
        head_ = tail_ = 0;
    }

    /* Producer side (UI/frame thread). */
    size_t write(const std::complex<float>* in, size_t n) {
        std::lock_guard<std::mutex> lock(m_);
        size_t written = 0;
        while (written < n) {
            const size_t next = (head_ + 1) % buf_.size();
            if (next == tail_) break;  /* full */
            buf_[head_] = in[written++];
            head_ = next;
        }
        return written;
    }

    size_t free_space() const {
        std::lock_guard<std::mutex> lock(m_);
        return buf_.size() - 1 - fill_locked();
    }

    /* Consumer side (DSP thread). Short reads are padded with zeros so the
     * transmitter never sees a gap it would treat as an underrun spike. */
    size_t read(std::complex<float>* out, size_t n) {
        std::lock_guard<std::mutex> lock(m_);
        size_t got = 0;
        while (got < n && tail_ != head_) {
            out[got++] = buf_[tail_];
            tail_ = (tail_ + 1) % buf_.size();
        }
        for (size_t i = got; i < n; i++) out[i] = {0.0f, 0.0f};
        return n;
    }

   private:
    size_t fill_locked() const {
        return (head_ + buf_.size() - tail_) % buf_.size();
    }

    mutable std::mutex m_;
    std::vector<std::complex<float>> buf_;
    size_t head_{0};
    size_t tail_{0};
};

class GpsSimView : public ui::View {
   public:
    GpsSimView();
    ~GpsSimView();

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;
    void focus() override;

    std::string title() const override { return "GPS Sim TX"; }

   private:
    static constexpr double sample_rate_ = 2600000.0;  /* upstream .C8 rate */

    void toggle_tx();
    void start_tx();
    void stop_tx();
    void confirm_and_start();
    bool is_transmitting() const { return transmitting_; }
    std::vector<gps::SatConfig> build_sat_list() const;
    void refresh_status();

    gps::SignalGenerator generator_{};
    GpsSampleRing ring_{};
    bool transmitting_{false};
    bool warned_{false};

    ui::Labels labels_{
        {{0 * 8, 1 * 16}, "L1:", ui::Color::light_grey()},
        {{0 * 8, 4 * 16}, "Lat:", ui::Color::light_grey()},
        {{0 * 8, 5 * 16}, "Lon:", ui::Color::light_grey()},
        {{0 * 8, 6 * 16}, "Alt:", ui::Color::light_grey()},
        {{0 * 8, 7 * 16}, "Sats:", ui::Color::light_grey()},
        {{0 * 8, 8 * 16}, "Week:", ui::Color::light_grey()},
        {{0 * 8, 9 * 16}, "TOW:", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{5 * 8, 1 * 16}};

    /* Static position. Latitude in 1/1000 deg (-90000..90000), longitude in
     * 1/1000 deg (-180000..180000), altitude in metres. Displayed only — see
     * the file header on why these do not yet drive a geometry solution. */
    ui::NumberField field_lat_{
        {6 * 8, 4 * 16}, 8, {-90000, 90000}, 100, ' '};
    ui::NumberField field_lon_{
        {6 * 8, 5 * 16}, 8, {-180000, 180000}, 100, ' '};
    ui::NumberField field_alt_{
        {6 * 8, 6 * 16}, 6, {-1000, 20000}, 10, ' '};

    /* Number of PRNs (1..N) to broadcast, and the highest PRN. */
    ui::NumberField field_num_sats_{
        {6 * 8, 7 * 16}, 2, {1, gps::MAX_PRN}, 1, ' '};

    ui::NumberField field_week_{
        {6 * 8, 8 * 16}, 4, {0, 1023}, 1, ' '};
    ui::NumberField field_tow_{
        {5 * 8, 9 * 16}, 6, {0, 100799}, 1, ' '};

    ui::Text text_status_{
        {0 * 8, 11 * 16, 30 * 8, 16}, ""};

    ui::Console console_{
        {0, 12 * 16, 30 * 8, 5 * 16}};

    ui::Button button_start_{
        {2 * 8, 17 * 16, 26 * 8, 3 * 16}, "Start TX"};
};

}  // namespace app

#endif /* MB200_GPS_ENCODER_ONLY */

#endif /*__MB200_UI_GPS_SIM_H__*/
