/*
 * mayhem-b200 — IQ Trim.
 *
 * Scans a capture file (.C16 / .C8) for the active, above-threshold region and
 * writes a trimmed copy in its place. Ported from the PortaPack Mayhem app
 * (application/apps/ui_iq_trim.* and application/iq_trim.*).
 *
 * The power-scan and trim-range computation is a faithful port of upstream's
 * iq::profile_capture / iq::compute_trim_range: raw ADC-domain integer samples
 * are read straight from the file (never the float path), power is the integer
 * magnitude-squared I*I + Q*Q, and the file is bucketed into a fixed number of
 * average-power columns from which the first and last buckets above a percentage
 * cutoff give the trim range. Building on core/iq_file.hpp for format detection
 * keeps the on-disk layout identical to what a PortaPack writes.
 *
 * Deliberate deviations from upstream, each host-safety or honesty motivated and
 * commented at its definition in the .cpp:
 *   - sample_interval and PowerBuckets::add()'s count are guarded so a tiny or
 *     pathological capture cannot spin forever or divide by zero on x86.
 *   - compute_trim_range() reports has_signal=false with an empty [0,0) range
 *     when nothing clears the cutoff, instead of upstream's one-bucket sliver —
 *     an honest "no active region" the UI can refuse to trim.
 *   - amplify_iq_buffer() amplifies every sample in the block; upstream's
 *     mult_count = length/sample_size/2 processes only a quarter of it and
 *     silently corrupts the output.
 *   - There is no on-device file browser in this build, so the capture is picked
 *     from a list of the files found in the CAPTURES directory rather than the
 *     firmware's FileLoadView.
 *
 * Copyright (C) 2023 Kyle Reed
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host implementation)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_IQ_TRIM_H__
#define __MB200_UI_IQ_TRIM_H__

#include "iq_file.hpp"  // core::IqFormat, format detection
#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

/* --- Pure trim logic (no UI) — mirrors upstream's iq:: namespace ----------- */
namespace iqtrim {

/* Metadata gathered while profiling a capture. Powers are in the raw ADC
 * domain (I*I + Q*Q on the stored integers), exactly as upstream. */
struct CaptureInfo {
    uint64_t file_size{0};
    uint64_t sample_count{0};
    uint8_t sample_size{0};  /* bytes per I/Q pair: 4 (C16) or 2 (C8) */
    uint32_t max_power{0};
    uint32_t max_iq{0};
};

/* Average sample power grouped into fixed-width buckets across the file. */
struct PowerBuckets {
    struct Bucket {
        uint32_t power{0};
        uint8_t count{0};
    };

    Bucket* p{nullptr};
    size_t size{0};

    /* Rolling average of the power added at `index` (upstream's math). Guarded
     * against the uint8_t count wrapping to zero on a degenerate profile. */
    void add(size_t index, uint32_t power);
};

/* A sample range to keep. end_sample is the sample *after* the last kept, so
 * (end - start) is the count. has_signal is false when nothing cleared the
 * cutoff — a host addition so callers can tell "no active region" from a real
 * range without inspecting the sample bounds. */
struct TrimRange {
    uint64_t start_sample{0};
    uint64_t end_sample{0};
    uint8_t sample_size{0};
    bool has_signal{false};
};

/* Reads capture metadata and fills `buckets` with sparse-sampled power. Returns
 * nullopt if the file cannot be opened or the extension is not a known IQ
 * format. samples_per_bucket controls the profiling density (upstream's 10). */
std::optional<CaptureInfo> profile_capture(
    const std::string& path,
    PowerBuckets& buckets,
    uint8_t samples_per_bucket = 10);

/* First/last buckets whose average power exceeds cutoff_percent (1–100) of the
 * capture's peak power, converted to a sample range. */
TrimRange compute_trim_range(
    const CaptureInfo& info,
    const PowerBuckets& buckets,
    uint8_t cutoff_percent);

/* Multiplies every I and Q integer in `buffer` (length bytes) by amplification,
 * clamping to the format's rails. sample_size is 4 for C16, 2 for C8. */
void amplify_iq_buffer(
    uint8_t* buffer,
    uint32_t length,
    uint32_t amplification,
    uint8_t sample_size);

/* Copies the sample range out to a temp file (optionally amplifying) and
 * replaces the original with it, as upstream does. on_progress may be empty.
 * Returns false on an invalid range or any I/O error, leaving the original
 * untouched. */
bool trim_capture_with_range(
    const std::string& path,
    const TrimRange& range,
    const std::function<void(uint8_t)>& on_progress,
    uint32_t amplification);

}  // namespace iqtrim

/* --- The app view ---------------------------------------------------------- */
namespace app {

class IQTrimView : public ui::View {
   public:
    IQTrimView();
    explicit IQTrimView(std::string path);

    std::string title() const override { return "IQ Trim"; }

    void on_show() override;
    void focus() override;
    void paint(ui::Painter& painter) override;

   private:
    void rescan_files();
    void open_file(const std::string& path);
    void profile_current();
    void compute_range();
    void update_range_controls(const iqtrim::TrimRange& range);
    void refresh_ui();
    bool do_trim();
    void set_status(const std::string& message, bool error = false);

    std::string path_{};
    std::optional<iqtrim::CaptureInfo> info_{};
    std::vector<iqtrim::PowerBuckets::Bucket> power_buckets_{};
    std::vector<std::string> files_{};

    ui::Labels labels_{
        {{0, 0}, "Capture File:", ui::Color::light_grey()},
        {{0, 96}, "Start  :", ui::Color::light_grey()},
        {{0, 112}, "End    :", ui::Color::light_grey()},
        {{0, 128}, "Samples:", ui::Color::light_grey()},
        {{0, 144}, "Max Pwr:", ui::Color::light_grey()},
        {{0, 160}, "Cutoff :", ui::Color::light_grey()},
        {{96, 160}, "%", ui::Color::light_grey()},
        {{0, 192}, "Amplify:", ui::Color::light_grey()},
        {{80, 192}, "x", ui::Color::light_grey()},
    };

    ui::OptionsField field_file_{{0, 16}, 28, {}};

    ui::NumberField field_start_{{72, 96}, 10, {0, 0}, 1, ' '};
    ui::NumberField field_end_{{72, 112}, 10, {0, 0}, 1, ' '};

    ui::Text text_samples_{{72, 128, 160, 16}, "0"};
    ui::Text text_max_{{72, 144, 160, 16}, "0"};

    ui::NumberField field_cutoff_{{72, 160}, 3, {1, 100}, 1, ' '};
    ui::NumberField field_amplify_{{72, 192}, 1, {1, 9}, 1, ' '};

    ui::Text text_status_{{0, 224, 240, 16}, ""};

    ui::Button button_trim_{{88, 260, 64, 32}, "Trim"};
};

}  // namespace app

#endif /*__MB200_UI_IQ_TRIM_H__*/
