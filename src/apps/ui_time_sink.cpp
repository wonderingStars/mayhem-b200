/*
 * mayhem-b200 — Time Sink (time-domain scope).
 *
 * Copyright (C) 2026 zxkmm (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_time_sink.hpp"

#include "../ui/display.hpp"
#include "../ui/theme.hpp"
#include "app_context.hpp"

namespace app {

/* --- TimeSinkWaveformWidget ---------------------------------------------- */

TimeSinkWaveformWidget::TimeSinkWaveformWidget(ui::Rect parent_rect,
                                               const int8_t* data,
                                               size_t length,
                                               ui::Color color)
    : ui::Widget{parent_rect},
      data_{data},
      length_{length},
      color_{color} {
    reset_cache();
}

void TimeSinkWaveformWidget::set_parent_rect(const ui::Rect new_parent_rect) {
    ui::Widget::set_parent_rect(new_parent_rect);
    reset_cache();
}

void TimeSinkWaveformWidget::on_show() {
    reset_cache();
    set_dirty();
}

void TimeSinkWaveformWidget::reset_cache() {
    history_count_ = 0;
    history_head_ = 0;
    needs_clear_ = true;
}

void TimeSinkWaveformWidget::set_persistence_frames(uint8_t frames) {
    const auto clamped = static_cast<uint8_t>(
        std::clamp<size_t>(frames, 1, max_persistence_frames));
    if (clamped != persistence_frames_) {
        persistence_frames_ = clamped;
        reset_cache();
        set_dirty();
    }
}

void TimeSinkWaveformWidget::paint(ui::Painter& painter) {
    const auto r = screen_rect();
    const auto background = ui::Theme::getInstance()->bg_darkest->background;

    if (r.width() <= 0 || r.height() <= 0 || data_ == nullptr || length_ == 0) {
        if (needs_clear_) {
            painter.fill_rectangle_unrolled8(r, background);
            needs_clear_ = false;
        }
        history_count_ = 0;
        history_head_ = 0;
        return;
    }

    const size_t columns = std::min<size_t>({
        length_,
        max_columns,
        static_cast<size_t>(r.width()),
    });
    if (columns == 0) {
        history_count_ = 0;
        history_head_ = 0;
        return;
    }

    if (needs_clear_) {
        painter.fill_rectangle_unrolled8(r, background);
        needs_clear_ = false;
        history_count_ = 0;
        history_head_ = 0;
    }

    for (size_t x = 0; x < columns; ++x) {
        const size_t src_index = (x * length_) / columns;
        current_y_[x] = static_cast<ui::Coord>(
            time_sink_sample_to_y(r.top(), r.height(), data_[src_index]));
    }

    for (size_t x = 0; x < columns; ++x) {
        host::display.draw_pixel(
            {static_cast<ui::Coord>(r.left() + static_cast<int>(x)), current_y_[x]}, color_);
    }

    /* Erase the oldest retained frame's pixel only where no newer retained
     * frame and no current sample lights the same row. */
    if (history_count_ >= persistence_frames_) {
        const size_t expired_slot = history_head_;

        for (size_t x = 0; x < columns; ++x) {
            const auto expired_y = static_cast<ui::Coord>(
                time_sink_sample_to_y(r.top(), r.height(), history_samples_[expired_slot][x]));
            bool keep = (expired_y == current_y_[x]);

            if (!keep) {
                for (size_t i = 1; i < history_count_; ++i) {
                    const size_t slot = (history_head_ + i) % max_persistence_frames;
                    const auto y = static_cast<ui::Coord>(
                        time_sink_sample_to_y(r.top(), r.height(), history_samples_[slot][x]));
                    if (y == expired_y) {
                        keep = true;
                        break;
                    }
                }
            }

            if (!keep) {
                host::display.draw_pixel(
                    {static_cast<ui::Coord>(r.left() + static_cast<int>(x)), expired_y},
                    background);
            }
        }

        history_head_ = (history_head_ + 1) % max_persistence_frames;
        --history_count_;
    }

    const size_t tail_slot = (history_head_ + history_count_) % max_persistence_frames;
    for (size_t x = 0; x < columns; ++x) {
        const size_t src_index = (x * length_) / columns;
        history_samples_[tail_slot][x] = data_[src_index];
    }
    ++history_count_;
}

/* --- TimeSinkView --------------------------------------------------------- */

TimeSinkView::TimeSinkView()
    : receiver_{*globals().receiver} {
    add_children({&labels_,
                  &field_frequency_,
                  &field_frequency_step_,
                  &field_gain_,
                  &options_sample_rate_,
                  &field_frame_skip_,
                  &options_persistence_,
                  &options_trigger_mode_,
                  &field_trigger_level_,
                  &waveform_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
        field_frequency_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                   static_cast<uint64_t>(caps.rx_freq.max));
    }

    field_frequency_.set_value(receiver_.target_frequency(), false);
    field_frequency_.on_change = [this](uint64_t hz) { receiver_.set_target_frequency(hz); };

    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t db) { receiver_.set_gain(db); };

    options_sample_rate_.set_by_nearest_value(static_cast<int32_t>(sampling_rate_), false);
    sampling_rate_ = static_cast<uint32_t>(options_sample_rate_.selected_index_value());
    options_sample_rate_.on_change = [this](size_t, int32_t v) {
        sampling_rate_ = static_cast<uint32_t>(v);
        apply_sample_rate();
    };

    field_frame_skip_.set_value(frame_skip_, false);
    field_frame_skip_.on_change = [this](int32_t v) {
        frame_skip_ = static_cast<uint8_t>(v);
        frame_phase_ = 0;
    };

    options_persistence_.set_by_nearest_value(persistence_frames_, false);
    persistence_frames_ = static_cast<uint8_t>(options_persistence_.selected_index_value());
    waveform_.set_persistence_frames(persistence_frames_);
    options_persistence_.on_change = [this](size_t, int32_t v) {
        persistence_frames_ = static_cast<uint8_t>(v);
        waveform_.set_persistence_frames(persistence_frames_);
    };

    options_trigger_mode_.set_by_value(static_cast<int32_t>(trigger_mode_), false);
    options_trigger_mode_.on_change = [this](size_t, int32_t v) {
        trigger_mode_ = static_cast<TimeSinkTriggerMode>(v);
        trigger_state_.lock_valid = false;
    };

    field_trigger_level_.set_value(trigger_level_, false);
    field_trigger_level_.on_change = [this](int32_t v) {
        trigger_level_ = v;
        trigger_state_.lock_valid = false;
    };

    /* Upstream runs the radio in SpectrumAnalysis mode with squelch off: the
     * scope wants the raw band, not a demodulated channel. */
    receiver_.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    receiver_.set_squelch_level(0);
    apply_sample_rate();
}

TimeSinkView::~TimeSinkView() = default;

void TimeSinkView::apply_sample_rate() {
    receiver_.set_sampling_rate(static_cast<double>(sampling_rate_));
}

void TimeSinkView::on_show() {
    ui::View::on_show();
    field_frequency_.focus();
    if (!receiver_.running()) receiver_.start();
}

void TimeSinkView::on_hide() {
    ui::View::on_hide();
}

void TimeSinkView::set_parent_rect(const ui::Rect new_parent_rect) {
    ui::View::set_parent_rect(new_parent_rect);
    waveform_.set_parent_rect({0, header_height,
                               new_parent_rect.width(),
                               static_cast<ui::Dim>(new_parent_rect.height() - header_height)});
}

void TimeSinkView::update_trace() {
    /* This is the wideband tap, and for a scope it is the correct one — the
     * upstream processor also reads the pre-channel-filter buffer. A dedicated
     * "raw buffer with its own sample-rate tag" tap would save the extra copy
     * but would not change the trace. */
    if (!receiver_.take_spectrum_samples(samples_, kTimeSinkTracePoints * 4)) return;
    if (samples_.empty()) return;

    time_sink_fill_trace(samples_.data(), samples_.size(),
                         static_cast<uint32_t>(receiver_.sampling_rate()), trace_);

    const size_t trigger_index =
        time_sink_find_trigger(trace_, trigger_mode_, trigger_level_, trigger_state_);
    time_sink_map_waveform(trace_, trigger_index, waveform_buffer_, kTimeSinkWaveformPoints);

    waveform_.set_dirty();
}

void TimeSinkView::on_frame_sync() {
    ui::View::on_frame_sync();

    /* Upstream's `trigger` field skips baseband buffers; here it skips UI
     * frames, which is the same "slow the trace down" control. */
    if (frame_skip_ > 0) {
        if (frame_phase_ < frame_skip_) {
            frame_phase_++;
            return;
        }
        frame_phase_ = 0;
    }

    update_trace();
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Category::Receive — upstream external.cmake places time_sink in the RX list.
 * No bitmap depicts an oscilloscope, so a generic tile rather than a borrowed
 * icon (doc/PORTING.md). */
const app::Registrar reg_time_sink{{"time_sink", "Time Sink", app::Category::Receive,
                                    ui::Color::green(), nullptr,
                                    [] { return std::make_unique<app::TimeSinkView>(); }}};
}  // namespace
