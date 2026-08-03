/*
 * mayhem-b200 — Signal Hunter (implementation).
 *
 * Copyright (C) 2026 Matej Sochan (original app and baseband processor)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_signal_hunter.hpp"

#include "app_context.hpp"
#include "app_registry.hpp"
#include "bitmaps.hpp"
#include "file_path.hpp"
#include "freqman_db.hpp"
#include "settings.hpp"
#include "string_format.hpp"
#include "theme.hpp"
#include "ui_menu.hpp"
#include "ui_navigation.hpp"

#include "../radio/receiver_model.hpp"
#include "../radio/usrp_radio.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace app {

namespace {

constexpr std::string_view kSection = "ext_signal_hunter";

constexpr float kLevelFloorDb = -100.0f;
constexpr float kLevelCeilDb = -10.0f;

/* Samples pulled from the receiver tap per frame. */
constexpr size_t kSamplesPerFrame = 2048;

/* Freqman list picker. Each app owns its own — doc/PORTING.md forbids adding a
 * shared header while other agents are writing in the same tree. */
class HunterFilePicker : public ui::View {
   public:
    explicit HunterFilePicker(std::function<void(std::string)> on_pick)
        : on_pick_{std::move(on_pick)} {
        add_children({&menu_, &text_empty_});

        auto files = core::get_freqman_files();
        if (files.empty()) {
            text_empty_.set("No lists in FREQMAN");
            menu_.hidden(true);
            return;
        }

        text_empty_.hidden(true);
        for (auto& stem : files) {
            std::string name = stem;
            menu_.add_item({name, ui::Color::white(), [this, name]() {
                                if (on_pick_) on_pick_(name);
                                if (auto* nav = app::globals().nav) nav->pop();
                            }});
        }
    }

    std::string title() const override { return "Target list"; }

    void focus() override {
        if (!menu_.hidden())
            menu_.focus();
        else
            text_empty_.focus();
    }

   private:
    std::function<void(std::string)> on_pick_{};
    ui::MenuView menu_{{0, 0, 240, 288}};
    ui::Text text_empty_{{0, 8, 240, 16}, ""};
};

}  // namespace

/* ======================================================================== */
/* HunterMainView                                                            */
/* ======================================================================== */

HunterMainView::HunterMainView(ui::Rect parent_rect, SignalHunterView& parent)
    : ui::View{parent_rect}, parent_{parent} {
    add_children({&text_current_freq_, &text_status_, &text_energy_, &text_peak_,
                  &button_start_stop_, &text_hits_});

    update_frequency(parent_.single_frequency);

    button_start_stop_.on_select = [this](ui::Button&) {
        parent_.is_hunting = !parent_.is_hunting;
        if (parent_.is_hunting) {
            button_start_stop_.set_text("STOP");
            if (parent_.freq_hop_mode && !parent_.frequency_list.empty()) {
                parent_.current_freq_index = 0;
                const auto freq = parent_.frequency_list[0];
                parent_.set_target_frequency(freq);
                update_frequency(freq);
            } else {
                parent_.set_target_frequency(parent_.single_frequency);
                update_frequency(parent_.single_frequency);
            }
            update_status("HUNTING...", ui::Theme::getInstance()->fg_green);
            parent_.send_hunter_config(true);
        } else {
            button_start_stop_.set_text("START");
            update_status("IDLE", ui::Theme::getInstance()->fg_light);
            set_recording_state(false);
            parent_.send_hunter_config(false);
        }
    };
}

void HunterMainView::focus() {
    button_start_stop_.focus();
}

void HunterMainView::update_frequency(int64_t freq) {
    current_freq_ = freq;
    text_current_freq_.set(to_string_dec_uint(static_cast<uint64_t>(std::max<int64_t>(freq, 0))) +
                           " Hz");
}

void HunterMainView::update_status(const std::string& status, const ui::Style* style) {
    text_status_.set(status);
    text_status_.set_style(style);
}

void HunterMainView::update_hits(uint32_t hits) {
    text_hits_.set("Hits: " + to_string_dec_uint(hits));
}

void HunterMainView::update_energy(uint32_t avg, uint32_t threshold) {
    text_energy_.set("E:" + to_string_dec_uint(avg) + " / " + to_string_dec_uint(threshold));
}

void HunterMainView::update_peak(const std::string& text) {
    text_peak_.set(text);
}

void HunterMainView::set_recording_state(bool recording) {
    text_current_freq_.set_style(recording ? ui::Theme::getInstance()->fg_red
                                           : ui::Theme::getInstance()->fg_light);
    text_current_freq_.set_dirty();
    update_frequency(current_freq_);
}

/* ======================================================================== */
/* HunterFreqsView                                                           */
/* ======================================================================== */

HunterFreqsView::HunterFreqsView(ui::Rect parent_rect, SignalHunterView& parent)
    : ui::View{parent_rect}, parent_{parent} {
    add_children({&button_load_file_, &button_clear_, &labels_, &field_dwell_,
                  &text_loaded_info_});

    hidden(true);  /* TabView contract: tabbed content arrives hidden. */

    field_dwell_.set_value(static_cast<int32_t>(parent_.hop_dwell_ms), false);
    field_dwell_.on_change = [this](int32_t v) {
        parent_.hop_dwell_ms = static_cast<uint32_t>(v);
    };

    button_load_file_.on_select = [this](ui::Button&) {
        auto* nav = app::globals().nav;
        if (!nav) return;
        nav->push(std::make_unique<HunterFilePicker>([this](std::string stem) {
            parent_.load_frequency_list(stem);
            update_list_count();
            parent_.config_view()->update_mode_display();
        }));
    };

    button_clear_.on_select = [this](ui::Button&) {
        parent_.frequency_list.clear();
        parent_.current_freq_index = 0;
        parent_.freq_hop_mode = false;
        update_list_count();
        parent_.config_view()->update_mode_display();
    };
}

void HunterFreqsView::update_list_count() {
    text_loaded_info_.set("Loaded: " + to_string_dec_uint(parent_.frequency_list.size()) +
                          " freqs");
}

void HunterFreqsView::focus() {
    button_load_file_.focus();
}

/* ======================================================================== */
/* HunterConfigView                                                          */
/* ======================================================================== */

HunterConfigView::HunterConfigView(ui::Rect parent_rect, SignalHunterView& parent)
    : ui::View{parent_rect}, parent_{parent} {
    add_children({&button_mode_, &field_single_freq_, &labels_, &field_threshold_,
                  &field_hang_time_, &text_info_});

    hidden(true);

    button_mode_.on_select = [this](ui::Button&) {
        parent_.freq_hop_mode = !parent_.freq_hop_mode;
        update_mode_display();
    };

    if (auto* r = app::globals().radio) {
        const auto& caps = r->caps();
        field_single_freq_.set_range(static_cast<uint64_t>(caps.rx_freq.min),
                                     static_cast<uint64_t>(caps.rx_freq.max));
    }
    field_single_freq_.set_value(static_cast<uint64_t>(parent_.single_frequency), false);
    field_single_freq_.on_change = [this](uint64_t hz) {
        parent_.single_frequency = static_cast<int64_t>(hz);
        if (!parent_.freq_hop_mode) parent_.set_target_frequency(parent_.single_frequency);
    };

    field_threshold_.set_value(static_cast<int32_t>(parent_.energy_threshold), false);
    field_threshold_.on_change = [this](int32_t v) {
        parent_.energy_threshold = static_cast<uint32_t>(v);
    };

    field_hang_time_.set_value(static_cast<int32_t>(parent_.hangtime_ms), false);
    field_hang_time_.on_change = [this](int32_t v) {
        parent_.hangtime_ms = static_cast<uint32_t>(v);
    };

    text_info_.set_style(ui::Theme::getInstance()->fg_yellow);

    update_mode_display();
}

void HunterConfigView::update_mode_display() {
    if (parent_.freq_hop_mode)
        button_mode_.set_text("MODE: HOP (" +
                              to_string_dec_uint(parent_.frequency_list.size()) + " freqs)");
    else
        button_mode_.set_text("MODE: SINGLE");
}

void HunterConfigView::focus() {
    field_threshold_.focus();
}

/* ======================================================================== */
/* SignalHunterView                                                          */
/* ======================================================================== */

SignalHunterView::SignalHunterView()
    : receiver_{*app::globals().receiver} {
    add_children({&labels_, &field_gain_, &text_level_, &level_meter_,
                  &tab_view_, &view_main_, &view_freqs_, &view_config_});

    tab_view_.set_parent_rect(kTabRect);

    {
        auto& s = core::settings();
        energy_threshold = static_cast<uint32_t>(
            s.get_uint(kSection, "threshold", signal_hunter::kDefaultThreshold));
        hangtime_ms = static_cast<uint32_t>(
            s.get_uint(kSection, "hangtime_ms", signal_hunter::kDefaultHangtimeMs));
        hop_dwell_ms = static_cast<uint32_t>(
            s.get_uint(kSection, "hop_dwell_ms", signal_hunter::kDefaultDwellMs));
        single_frequency = s.get_int(kSection, "single_freq", 433'920'000);
        freqman_file = s.get_string(kSection, "file", "TARGETS");
    }

    window_ = dsp::make_window(dsp::WindowType::BlackmanHarris, fft_.size());

    if (auto* r = app::globals().radio) {
        const auto& caps = r->caps();
        field_gain_.set_range(static_cast<int32_t>(caps.rx_gain.min),
                              static_cast<int32_t>(caps.rx_gain.max));
    }
    field_gain_.set_value(static_cast<int32_t>(receiver_.gain()), false);
    field_gain_.on_change = [this](int32_t v) { receiver_.set_gain(v); };

    /* Upstream runs the hunter baseband at 2 Msps with a 1.75 MHz analog
     * filter; the same settings apply here. */
    receiver_.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
    receiver_.set_sampling_rate(static_cast<double>(signal_hunter::kBasebandFs));
    receiver_.set_target_frequency(static_cast<uint64_t>(std::max<int64_t>(single_frequency, 0)));

    view_main_.update_frequency(single_frequency);
    view_freqs_.update_list_count();
    view_config_.update_mode_display();
}

SignalHunterView::~SignalHunterView() {
    stop_recording();

    auto& s = core::settings();
    if (!freq_hop_mode) single_frequency = target_frequency();
    s.set_uint(kSection, "threshold", energy_threshold);
    s.set_uint(kSection, "hangtime_ms", hangtime_ms);
    s.set_uint(kSection, "hop_dwell_ms", hop_dwell_ms);
    s.set_int(kSection, "single_freq", single_frequency);
    s.set_string(kSection, "file", freqman_file);
    s.save();
}

void SignalHunterView::set_target_frequency(int64_t hz) {
    receiver_.set_target_frequency(static_cast<uint64_t>(std::max<int64_t>(hz, 0)));
}

int64_t SignalHunterView::target_frequency() const {
    return static_cast<int64_t>(receiver_.target_frequency());
}

void SignalHunterView::load_frequency_list(const std::string& stem) {
    frequency_list.clear();
    current_freq_index = 0;

    core::FreqmanDB db;
    if (db.open(core::get_freqman_path(stem))) {
        for (auto entry : db) {
            if (entry.frequency_a > 0) frequency_list.push_back(entry.frequency_a);
        }
    }
    freqman_file = stem;

    /* Switch to HOP mode if a file was actually loaded, as upstream. */
    if (!frequency_list.empty()) freq_hop_mode = true;
}

/* Upstream's send_hunter_config(): the M4 was told the threshold and hangtime
 * and whether to hunt. Here the same configuration goes straight into the
 * detector that runs on this thread. */
void SignalHunterView::send_hunter_config(bool start) {
    const double rate = receiver_.sampling_rate();
    const uint32_t samples_per_ms =
        std::max<uint32_t>(1u, static_cast<uint32_t>(std::lround(rate / 1000.0)));

    detector_.configure(energy_threshold,
                        signal_hunter::hangtime_to_samples(hangtime_ms, samples_per_ms));
    detector_.set_hunting(start);

    if (!start) stop_recording();
}

void SignalHunterView::on_show() {
    View::on_show();
    if (!receiver_.running()) receiver_.start();
    tab_view_.focus();
}

void SignalHunterView::process_samples() {
    if (!receiver_.take_spectrum_samples(samples_, kSamplesPerFrame)) return;
    if (samples_.empty()) return;

    for (const auto& s : samples_) {
        const auto ev = detector_.push(s.real(), s.imag());
        if (ev.trigger) on_trigger(ev.energy);
        if (ev.stop) on_stop();
    }
}

void SignalHunterView::on_trigger(uint32_t energy) {
    (void)energy;
    if (receiver_.capturing() || !is_hunting) return;

    trigger_hits++;
    view_main_.update_hits(trigger_hits);
    view_main_.update_status("RECORDING!", ui::Theme::getInstance()->fg_red);
    view_main_.set_recording_state(true);

    start_recording();
}

void SignalHunterView::on_stop() {
    if (!receiver_.capturing()) {
        detector_.end_recording();
        return;
    }

    stop_recording();
    view_main_.set_recording_state(false);

    if (is_hunting) {
        if (freq_hop_mode && !frequency_list.empty()) {
            current_freq_index = (current_freq_index + 1) % frequency_list.size();
            const auto next = frequency_list[current_freq_index];
            set_target_frequency(next);
            view_main_.update_frequency(next);
            hop_timer_ms_ = 0;
        }
        view_main_.update_status("HUNTING...", ui::Theme::getInstance()->fg_green);
    }
}

void SignalHunterView::start_recording() {
    /* Upstream's HNT_yyyymmddThhmmss naming; to_string_timestamp_now() gives
     * "YYYYMMDD_HHMMSS", so the separator is normalised to 'T'. */
    std::string ts = to_string_timestamp_now();
    const auto underscore = ts.find('_');
    if (underscore != std::string::npos) ts[underscore] = 'T';

    capture_name_ = "HNT_" + ts;
    core::ensure_directory(core::captures_directory());

    if (!receiver_.start_capture(core::captures_directory() + "/" + capture_name_)) {
        view_main_.update_status("FILE ERROR", ui::Theme::getInstance()->fg_red);
        send_hunter_config(false);
        is_hunting = false;
        view_main_.set_start_button_text("START");
        capture_name_.clear();
        detector_.end_recording();
        return;
    }

    detector_.begin_recording();
    view_main_.update_status("RECORDING!", ui::Theme::getInstance()->fg_red);
}

void SignalHunterView::stop_recording() {
    if (receiver_.capturing()) receiver_.stop_capture();
    capture_name_.clear();
    detector_.end_recording();
}

void SignalHunterView::update_peak_readout() {
    /* HOST ADDITION — see the header. Which bin of the captured span is hottest. */
    if (samples_.size() < static_cast<size_t>(kFftBins)) return;

    fft_.spectrum_db(samples_.data(), window_, spectrum_db_);
    const auto peak = signal_hunter::find_peak_bin(spectrum_db_);
    if (peak.bin < 0) return;

    const int64_t hz = signal_hunter::bin_to_frequency(
        target_frequency(), peak.bin, receiver_.sampling_rate(), kFftBins);

    view_main_.update_peak("Peak " +
                           to_string_short_freq(static_cast<uint64_t>(std::max<int64_t>(hz, 0))) +
                           " " + to_string_dec_int(static_cast<int32_t>(std::lround(peak.db))) +
                           "dB");
}

void SignalHunterView::on_frame_sync() {
    View::on_frame_sync();
    frame_counter_++;

    process_samples();

    /* Upstream's hop timer: one 60 Hz frame is ~17 ms. Held off while a capture
     * is running, as upstream holds it off while capture_thread exists. */
    if (is_hunting && freq_hop_mode && !receiver_.capturing() && !frequency_list.empty()) {
        hop_timer_ms_ += 17;
        if (hop_timer_ms_ >= hop_dwell_ms) {
            hop_timer_ms_ = 0;
            current_freq_index = (current_freq_index + 1) % frequency_list.size();
            const auto next = frequency_list[current_freq_index];
            set_target_frequency(next);
            view_main_.update_frequency(next);
        }
    } else {
        hop_timer_ms_ = 0;
    }

    /* ~10 Hz readouts. */
    if ((frame_counter_ % 6) == 0) {
        const float level = receiver_.rf_level_db();
        float frac = (level - kLevelFloorDb) / (kLevelCeilDb - kLevelFloorDb);
        frac = std::clamp(frac, 0.0f, 1.0f);
        level_meter_.set_value(static_cast<uint8_t>(frac * 255.0f));
        text_level_.set(to_string_dec_int(static_cast<int32_t>(std::lround(level))) + " dBFS");

        view_main_.update_energy(detector_.average(), detector_.threshold());
        update_peak_readout();
    }
}

}  // namespace app

/* --- Registration --------------------------------------------------------- */

namespace {
const app::Registrar reg_signal_hunter{
    {"signal_hunter", "Signal Hunter", app::Category::Receive, ui::Color::green(),
     &ui::bitmap_icon_search,
     [] { return std::make_unique<app::SignalHunterView>(); }}};
}  // namespace
