/*
 * mayhem-b200 — Search: wideband energy-search scanner.
 *
 * A port of the firmware's application/apps/ui_search.*. Search sweeps a
 * frequency range in 2.5 MHz slices, and for each slice takes a 256-bin power
 * spectrum, blanks the DC spike and the band edges, finds the strongest bin,
 * and — once the same bin stays hot long enough — "locks" it, resolves the bin
 * to an absolute frequency and drops it into a recency-ordered results table.
 *
 * What changed for the host:
 *   - The firmware's wideband_spectrum baseband image feeding ChannelSpectrum
 *     messages (256 uint8 power bins, DC-first) is replaced by pulling raw IQ
 *     from radio::ReceiverModel and running dsp::Fft. The host FFT already
 *     emits the -Fs/2..+Fs/2 layout (DC at bin N/2 = 128), which is exactly the
 *     layout upstream produces *after* its manual reorder — so the bin ↔
 *     frequency mapping is identical, no reorder needed.
 *   - The host FFT is in dBFS; the detector works in the firmware's 0..255
 *     "power byte" domain so the threshold semantics (Trig 5..255, mean+trig)
 *     port unchanged. db_to_power() quantises one to the other.
 *   - The upstream slice-sequencer walks slice_counter one past the array and
 *     reads/writes slices[slices_nb] out of bounds; the port fills exactly
 *     [0, slices_nb) and detects, dropping that bug.
 *   - Select on a result tunes the receiver there (the firmware pushes its
 *     FrequencySaveView, which this host build does not have yet).
 *
 * The pure logic — bin→frequency mapping, DC/edge blanking, per-slice scan,
 * mean, cross-slice peak selection, snap, slice planning and the lock/release
 * state machine — lives in namespace app::search as header-only functions and a
 * Detector class so it can be unit-tested without a radio (see
 * tests/test_search.cpp). Expected values are derived from upstream, not from
 * this port's output.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SEARCH_H__
#define __MB200_UI_SEARCH_H__

#include "../core/string_format.hpp"
#include "../dsp/fft.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_recent_entries.hpp"
#include "ui_spectrum.hpp"
#include "ui_widget.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace app {

/* ======================================================================== *
 *  Pure Search logic — ported faithfully from ui_search.cpp, no UI/radio.  *
 * ======================================================================== */
namespace search {

/* Constants, matching the firmware's #defines exactly. */
constexpr uint32_t slice_width_hz = 2'500'000;              /* SEARCH_SLICE_WIDTH   */
constexpr int bin_nb = 256;                                 /* SEARCH_BIN_NB        */
constexpr int bin_nb_no_dc = bin_nb - 16;                   /* SEARCH_BIN_NB_NO_DC  */
constexpr int dc_bin = bin_nb / 2;                          /* DC lands at bin 128  */
constexpr int64_t bin_width_hz = slice_width_hz / bin_nb;   /* SEARCH_BIN_WIDTH=9765 */
constexpr uint8_t detect_delay = 5;                         /* DETECT_DELAY, 100ms  */
constexpr uint8_t release_delay = 6;                        /* RELEASE_DELAY        */
constexpr int max_slices = 32;                              /* slices[32]           */

/* --- Bin ↔ frequency ---------------------------------------------------- */

/* Width of one bin, in Hz, for an arbitrary span/bin-count. Integer division,
 * matching the firmware's SEARCH_BIN_WIDTH definition. */
inline int64_t bin_width(uint64_t span, int nbins) {
    return nbins > 0 ? static_cast<int64_t>(span) / nbins : 0;
}

/* Absolute frequency of a bin, for an arbitrary centre/span/bin-count. Bin
 * nbins/2 is DC (the centre frequency); bins above it are higher in frequency,
 * bins below it lower. */
inline uint64_t bin_to_frequency(uint64_t center, int bin, uint64_t span, int nbins) {
    const int64_t bw = bin_width(span, nbins);
    return static_cast<uint64_t>(static_cast<int64_t>(center) + bw * (bin - nbins / 2));
}

/* Search's fixed-parameter form (2.5 MHz over 256 bins). This is upstream's
 *   center_frequency + (SEARCH_BIN_WIDTH * (bin - 128))
 * verbatim. */
inline uint64_t bin_to_frequency(uint64_t center, int bin) {
    return static_cast<uint64_t>(static_cast<int64_t>(center) + bin_width_hz * (bin - dc_bin));
}

/* DC-spike and band-edge blanking, faithful to on_channel_spectrum: the outer
 * two bins each side and the twelve centre bins (122..133) are forced to zero.
 * Defined against the 256-bin, DC-at-128 layout. */
inline bool bin_blanked(int bin) {
    return (bin < 2) || (bin > 253) || (bin >= 122 && bin < 134);
}

/* Snap a frequency to the nearest step at-or-below it.
 *
 * The firmware writes `round(resolved_frequency / snap_value) * snap_value`,
 * but resolved_frequency is int64 and snap_value uint32, so the division is
 * integer and round() is a no-op — the effective behaviour is truncation to the
 * step below. Ported as such so a given signal snaps to the same channel it
 * would on a PortaPack. */
inline uint64_t snap_frequency(uint64_t freq, uint32_t step) {
    return step == 0 ? freq : (freq / step) * step;
}

/* Quantise a dBFS magnitude to the firmware's 0..255 power-byte scale. Host
 * addition: the firmware's baseband already delivered bytes, the host FFT
 * delivers dBFS, and the detector's thresholds are in the byte domain. Linear
 * between floor_db (→0) and ceil_db (→255). */
inline uint8_t db_to_power(float db, float floor_db, float ceil_db) {
    if (ceil_db <= floor_db) return 0;
    float t = (db - floor_db) / (ceil_db - floor_db);
    if (t <= 0.0f) return 0;
    if (t >= 1.0f) return 255;
    return static_cast<uint8_t>(t * 255.0f + 0.5f);
}

/* --- Per-slice scan ----------------------------------------------------- */

struct SliceScan {
    uint8_t max_power{0};
    int16_t max_index{0};
    uint32_t power_sum{0};  /* Σ power over the 256 bins, blanked bins as 0. */
};

/* Scan one slice's 256-bin power spectrum (DC at bin 128). Applies blanking,
 * finds the strongest surviving bin, and sums powers for the running mean —
 * exactly the loop in on_channel_spectrum. `power` must point at 256 bytes. */
inline SliceScan scan_slice(const uint8_t* power) {
    SliceScan s;
    for (int bin = 0; bin < bin_nb; bin++) {
        const uint8_t p = bin_blanked(bin) ? uint8_t{0} : power[bin];
        s.power_sum += p;
        if (p > s.max_power) {
            s.max_power = p;
            s.max_index = static_cast<int16_t>(bin);
        }
    }
    return s;
}

/* Mean power per non-DC bin across the whole sweep. Firmware:
 *   mean_power = mean_acc / (SEARCH_BIN_NB_NO_DC * slices_nb) */
inline uint32_t mean_power(uint32_t total_power_sum, int slices_nb) {
    if (slices_nb <= 0) return 0;
    return total_power_sum / static_cast<uint32_t>(bin_nb_no_dc * slices_nb);
}

/* --- Cross-slice peak selection ---------------------------------------- */

struct Slice {
    uint64_t center_frequency{0};
    uint8_t max_power{0};
    int16_t max_index{0};
};

struct Peak {
    uint8_t power_max{0};
    int16_t bin_max{-1};       /* -1 when nothing cleared the threshold. */
    uint32_t slice_max{0};
    uint8_t overall_power_max{0};
};

/* The strongest bin across all slices that clears (mean + threshold), plus the
 * overall max regardless of threshold — the first loop of do_detection. */
inline Peak find_peak(const Slice* slices, int slices_nb, uint32_t mean, uint32_t threshold) {
    Peak r;
    for (int slice = 0; slice < slices_nb; slice++) {
        const uint8_t power = slices[slice].max_power;
        if (power > r.overall_power_max) r.overall_power_max = power;
        if ((power >= mean + threshold) && (power > r.power_max)) {
            r.power_max = power;
            r.bin_max = slices[slice].max_index;
            r.slice_max = static_cast<uint32_t>(slice);
        }
    }
    return r;
}

/* --- Slice planning ----------------------------------------------------- */

struct Plan {
    int slices_nb{1};
    bool overflow{false};      /* range needed > 32 slices; clamped. */
    uint64_t center[max_slices]{};
};

/* Lay out the slice centres over [freq_min, freq_max], faithful to
 * on_range_changed. A span wider than one slice is covered by ceil(span/width)
 * slices, centred on the range. */
inline Plan plan_slices(uint64_t freq_min, uint64_t freq_max) {
    Plan plan;
    const uint64_t lo = std::min(freq_min, freq_max);
    const uint64_t hi = std::max(freq_min, freq_max);
    const uint64_t span = hi - lo;  /* abs(freq_max - freq_min) */

    if (span > slice_width_hz) {
        int slices_nb = static_cast<int>((span + slice_width_hz - 1) / slice_width_hz);
        if (slices_nb > max_slices) {
            plan.overflow = true;
            slices_nb = max_slices;
        }
        const uint64_t slices_span = static_cast<uint64_t>(slices_nb) * slice_width_hz;
        const int64_t offset =
            (static_cast<int64_t>(span) - static_cast<int64_t>(slices_span)) / 2 +
            static_cast<int64_t>(slice_width_hz / 2);
        uint64_t c = static_cast<uint64_t>(static_cast<int64_t>(lo) + offset);
        for (int s = 0; s < slices_nb; s++) {
            plan.center[s] = c;
            c += slice_width_hz;
        }
        plan.slices_nb = slices_nb;
    } else {
        plan.center[0] = (freq_max + freq_min) / 2;
        plan.slices_nb = 1;
    }
    return plan;
}

/* --- Lock / release state machine -------------------------------------- */

enum class Action { None, Lock, Release };

struct Outcome {
    Action action{Action::None};
    uint64_t frequency{0};  /* resolved_frequency: the value shown / logged. */
    uint32_t duration{0};   /* set on Release (100 ms units). */
    bool out_of_range{false};
    bool display_dirty{false};
};

/* The detector half of do_detection()/do_timers(), with the UI, RTC, audio and
 * logging side effects lifted out and reported back through Outcome. One
 * do_detection() call corresponds to one completed sweep; do_timers() is the
 * firmware's ~10 Hz timer tick. */
class Detector {
   public:
    /* `bin_max`/`slice_max` come from find_peak; `peak_center` is the centre
     * frequency of slice_max (ignored when bin_max < 0). */
    Outcome do_detection(int16_t bin_max, uint32_t slice_max, uint64_t peak_center,
                         bool snap, uint32_t snap_step,
                         uint64_t freq_min, uint64_t freq_max) {
        Outcome out;

        if ((bin_max >= last_bin_ - 2) && (bin_max <= last_bin_ + 2) &&
            (bin_max > -1) && (slice_max == last_slice_)) {
            /* Peak has stayed within ±2 bins of the same slice. */
            if (detect_timer_ >= detect_delay) {
                if ((bin_max != locked_bin_) || (!locked_)) {
                    if (!locked_) {
                        uint64_t rf = bin_to_frequency(peak_center, bin_max);
                        if (snap) rf = snap_frequency(rf, snap_step);
                        resolved_frequency_ = rf;  /* updated even if out of range */
                        if (rf >= freq_min && rf <= freq_max) {
                            duration_ = 0;
                            locked_ = true;
                            locked_bin_ = static_cast<uint16_t>(bin_max);
                            out.action = Action::Lock;
                        } else {
                            out.out_of_range = true;
                        }
                    }
                    out.display_dirty = true;
                }
            }
            release_timer_ = 0;
        } else {
            detect_timer_ = 0;
            if (locked_) {
                if (release_timer_ >= release_delay) {
                    locked_ = false;
                    out.action = Action::Release;
                    out.duration = duration_;
                }
            }
        }

        last_bin_ = bin_max;
        last_slice_ = slice_max;
        out.frequency = resolved_frequency_;
        return out;
    }

    /* ~10 Hz tick: advance the two saturating timers and, while locked, the
     * hit duration. The firmware's do_timers() also drives progress/rate/mean
     * widgets; those are the view's job. */
    void do_timers() {
        if (detect_timer_ < detect_delay) detect_timer_++;
        if (release_timer_ < release_delay) release_timer_++;
        if (locked_) duration_++;
    }

    bool locked() const { return locked_; }
    uint64_t resolved_frequency() const { return resolved_frequency_; }
    uint32_t duration() const { return duration_; }
    uint8_t detect_timer() const { return detect_timer_; }
    uint8_t release_timer() const { return release_timer_; }
    int16_t last_bin() const { return last_bin_; }

   private:
    uint8_t detect_timer_{0};
    uint8_t release_timer_{0};
    int16_t last_bin_{0};
    uint32_t last_slice_{0};
    uint16_t locked_bin_{0};
    bool locked_{false};
    uint64_t resolved_frequency_{0};
    uint32_t duration_{0};
};

/* Duration string for the results row, faithful to the firmware's table draw:
 * tenths of a second below a minute, "MmSSs" above. */
inline std::string format_duration(uint32_t duration) {
    if (duration < 600)
        return to_string_dec_uint(duration / 10) + "." + to_string_dec_uint(duration % 10) + "s";
    return to_string_dec_uint(duration / 600) + "m" + to_string_dec_uint((duration / 10) % 60) + "s";
}

}  // namespace search

/* ======================================================================== *
 *  Results table entry                                                      *
 * ======================================================================== */

/* One row of the results list. Keyed by frequency, so re-hearing a frequency
 * updates the existing row rather than adding a duplicate. Fields match the
 * firmware's SearchRecentEntry (frequency / duration / time). */
struct SearchRecentEntry {
    using Key = uint64_t;
    static constexpr Key invalid_key = 0xffffffffffffffffull;

    uint64_t frequency{0};
    uint32_t duration{0};  /* 100 ms units */
    std::string time{};

    SearchRecentEntry() = default;
    explicit SearchRecentEntry(Key f)
        : frequency{f} {}

    Key key() const { return frequency; }
    void set_time(const std::string& t) { time = t; }
    void set_duration(uint32_t d) { duration = d; }
};

using SearchRecentEntries = ui::RecentEntries<SearchRecentEntry>;

/* ======================================================================== *
 *  View                                                                     *
 * ======================================================================== */

class SearchView : public ui::View {
   public:
    SearchView();
    ~SearchView() override;

    SearchView(const SearchView&) = delete;
    SearchView& operator=(const SearchView&) = delete;

    std::string title() const override { return "Search"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void on_range_changed();
    void process_spectrum();
    void do_detection();
    void update_timers_ui();
    void update_power_ui();
    void update_rate_ui();
    void set_logging(bool enabled);
    void log_entry(const SearchRecentEntry& entry);

    radio::ReceiverModel& receiver_;
    radio::ReceiverModel::Mode previous_mode_{radio::ReceiverModel::Mode::NarrowbandFMAudio};
    double previous_sample_rate_{2'400'000.0};

    /* Settings (in-memory; persistence is a future add — see report). */
    uint32_t power_threshold_{80};
    uint64_t freq_min_{100'000'000};
    uint64_t freq_max_{400'000'000};
    bool snap_search_{true};
    uint32_t snap_step_{12'500};

    /* Sweep state. */
    search::Slice slices_[search::max_slices]{};
    int slices_nb_{1};
    int slice_counter_{0};
    uint32_t mean_acc_{0};
    uint32_t mean_power_{0};
    uint8_t overall_power_max_{0};
    uint32_t search_counter_{0};
    search::Detector detector_{};
    uint32_t frame_counter_{0};

    /* Logging. */
    bool logging_{false};
    std::ofstream log_file_{};

    /* FFT machinery. */
    dsp::Fft fft_{search::bin_nb};
    std::vector<float> window_{};
    std::vector<dsp::cfloat> samples_{};
    std::vector<float> spectrum_db_{};
    std::array<uint8_t, search::bin_nb> power_{};

    /* --- Widgets --- */
    ui::Labels labels_{
        {{0, 0}, "Min", ui::Color::light_grey()},
        {{0, 16}, "Max", ui::Color::light_grey()},
        {{0, 32}, "Gain", ui::Color::light_grey()},
        {{96, 32}, "Thr", ui::Color::light_grey()},
        {{0, 48}, "Mean", ui::Color::light_grey()},
        {{96, 48}, "Sl", ui::Color::light_grey()},
        {{152, 48}, "Rate", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_min_{{32, 0}};
    ui::FrequencyField field_frequency_max_{{32, 16}};

    ui::NumberField field_gain_{{40, 32}, 3, {0, 76}, 1, ' '};
    ui::NumberField field_threshold_{{128, 32}, 3, {5, 255}, 5, ' '};

    ui::Text text_mean_{{40, 48, 48, 16}, "---"};
    ui::Text text_slices_{{112, 48, 24, 16}, " 1"};
    ui::Text text_rate_{{184, 48, 48, 16}, "---"};

    ui::spectrum::SpectrumWidget trace_{{0, 64, 240, 44}};

    ui::ProgressBar progress_timers_{{0, 110, 110, 12}};
    ui::VuMeter vu_max_{{116, 110, 64, 12}, 16, false};
    ui::Text text_infos_{{0, 124, 240, 16}, "Listening"};

    ui::BigFrequency big_display_{{0, 142, 240, 44}};

    ui::Checkbox check_snap_{{0, 190}, 4, "Snap", true};
    ui::OptionsField options_snap_{
        {80, 190},
        7,
        {{"25kHz  ", 25'000},
         {"12.5kHz", 12'500},
         {"8.33kHz", 8'333},
         {"2.5kHz ", 2'500},
         {"500Hz  ", 500}}};
    ui::Checkbox check_log_{{184, 190}, 3, "LOG", true};

    ui::RecentEntriesColumns columns_{{"Frequency", 0}, {"Time", 8}, {"Duration", 8}};
    SearchRecentEntries recent_{};
    ui::RecentEntriesView<SearchRecentEntries> recent_view_{columns_, recent_};
};

}  // namespace app

#endif /*__MB200_UI_SEARCH_H__*/
