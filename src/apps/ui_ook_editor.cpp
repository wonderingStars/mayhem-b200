/*
 * mayhem-b200 — OOK Editor: edit and replay a raw OOK waveform.
 *
 * Copyright (C) 2024 Samir Sánchez Garnica @sasaga92
 * Copyright (C) 2024 gullradriel (ook_file format)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_ook_editor.hpp"

#include "../core/file_path.hpp"
#include "../core/string_format.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_alphanum.hpp"
#include "ui_navigation.hpp"
#include "ui_playlist_editor.hpp"  /* app::FileBrowserView */

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>

namespace app {
namespace ook_editor {

std::string sample_rate_token(uint32_t hz) {
    switch (hz) {
        case 250000U: return "250k";
        case 1000000U: return "1M";
        case 2000000U: return "2M";
        case 5000000U: return "5M";
        case 10000000U: return "10M";
        case 20000000U: return "20M";
        default: return "";
    }
}

uint32_t sample_rate_from_token(const std::string& token, bool& ok) {
    ok = true;
    if (token == "250k") return 250000U;
    if (token == "1M") return 1000000U;
    if (token == "2M") return 2000000U;
    if (token == "5M") return 5000000U;
    if (token == "10M") return 10000000U;
    if (token == "20M") return 20000000U;
    ok = false;
    return 0;
}

bool read_ook_file(const std::string& path, OokFileData& out) {
    std::ifstream f{path, std::ios::binary};
    if (!f.is_open()) return false;

    std::string line;
    if (!std::getline(f, line)) return false;
    /* Tolerate a trailing CR from a file written on Windows. */
    if (!line.empty() && line.back() == '\r') line.pop_back();

    /* Split exactly the way read_ook_file() does: five spaces, then the rest is
     * the payload. */
    const size_t s1 = line.find(' ');
    const size_t s2 = (s1 == std::string::npos) ? s1 : line.find(' ', s1 + 1);
    const size_t s3 = (s2 == std::string::npos) ? s2 : line.find(' ', s2 + 1);
    const size_t s4 = (s3 == std::string::npos) ? s3 : line.find(' ', s3 + 1);
    const size_t s5 = (s4 == std::string::npos) ? s4 : line.find(' ', s4 + 1);
    if (s1 == std::string::npos || s2 == std::string::npos || s3 == std::string::npos ||
        s4 == std::string::npos || s5 == std::string::npos)
        return false;

    const std::string frequency_str = line.substr(0, s1);
    const std::string sample_rate_str = line.substr(s1 + 1, s2 - s1 - 1);
    const std::string symbol_rate_str = line.substr(s2 + 1, s3 - s2 - 1);
    const std::string repeat_str = line.substr(s3 + 1, s4 - s3 - 1);
    const std::string pause_str = line.substr(s4 + 1, s5 - s4 - 1);
    const std::string payload = line.substr(s5 + 1);

    bool ok = false;
    const uint32_t sample_rate = sample_rate_from_token(sample_rate_str, ok);
    if (!ok) return false;

    out.frequency = std::strtoull(frequency_str.c_str(), nullptr, 10);
    out.sample_rate = sample_rate;
    out.symbol_rate = static_cast<uint16_t>(std::atoi(symbol_rate_str.c_str()));
    out.repeat = static_cast<uint16_t>(std::atoi(repeat_str.c_str()));
    out.pause_symbol_duration = static_cast<uint16_t>(std::atoi(pause_str.c_str()));
    out.payload = payload;
    return true;
}

bool save_ook_file(const OokFileData& data, const std::string& path) {
    const std::string token = sample_rate_token(data.sample_rate);
    if (token.empty()) return false;

    std::ofstream f{path, std::ios::binary | std::ios::trunc};
    if (!f.is_open()) return false;

    f << to_string_dec_uint(data.frequency) << " "
      << token << " "
      << to_string_dec_uint(data.symbol_rate) << " "
      << to_string_dec_uint(data.repeat) << " "
      << to_string_dec_uint(data.pause_symbol_duration) << " "
      << data.payload << "\n";

    return static_cast<bool>(f);
}

size_t pack_payload(const std::string& payload, std::vector<uint8_t>& out) {
    const size_t n = payload.size();
    out.assign((n + 7) / 8, 0);

    uint8_t byte = 0;
    size_t len = 0;
    for (char c : payload) {
        byte <<= 1;
        if (c != '0') byte |= 1;
        if ((len & 7) == 7) out[len >> 3] = byte;
        len++;
    }

    const size_t padding = 8 - (len & 7);
    if (padding != 8) {
        byte <<= padding;
        out[(len + padding - 1) >> 3] = byte;
    }

    return len;
}

std::string ook_directory() {
    return core::data_directory() + "/OOK";
}

}  // namespace ook_editor

/* ======================================================================== *
 *  View                                                                     *
 * ======================================================================== */

OOKEditorView::OOKEditorView() {
    add_children({&labels_, &field_frequency_, &field_sample_rate_, &field_symbol_rate_,
                  &field_repeat_, &field_pause_, &text_payload_, &button_set_,
                  &button_open_, &button_save_, &waveform_, &text_status_,
                  &progressbar_, &text_warning_, &button_send_});

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_frequency_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                                   static_cast<uint64_t>(caps.tx_freq.max));
    }

    field_frequency_.set_step_index(9);  // 100 kHz
    field_frequency_.set_value(433'920'000, false);
    field_frequency_.on_change = [this](uint64_t hz) {
        if (auto* tx = globals().transmitter) tx->set_target_frequency(hz);
    };

    field_sample_rate_.set_by_value(2000000);
    field_symbol_rate_.set_value(100);
    field_repeat_.set_value(4);
    field_pause_.set_value(100);

    button_set_.on_select = [this](ui::Button&) {
        auto* nav = globals().nav;
        if (!nav) return;
        ui::text_prompt(*nav, payload_, 512, ENTER_KEYBOARD_MODE_DIGITS,
                        [this](std::string& s) { set_payload(s); });
    };

    button_open_.on_select = [this](ui::Button&) { open_file(); };
    button_save_.on_select = [this](ui::Button&) { save_file(); };
    button_send_.on_select = [this](ui::Button&) { toggle_tx(); };

    draw_waveform();
}

OOKEditorView::~OOKEditorView() {
    stop_tx();
}

void OOKEditorView::focus() {
    button_set_.focus();
}

void OOKEditorView::on_hide() {
    stop_tx();
    View::on_hide();
}

void OOKEditorView::set_payload(const std::string& payload) {
    payload_ = payload;
    text_payload_.set(payload_);
    text_status_.set("");
    draw_waveform();
}

void OOKEditorView::draw_waveform() {
    constexpr size_t kPadLeft = 1;
    constexpr size_t kPadRight = 1;

    size_t length = payload_.length();
    if (length + (kPadLeft + kPadRight) >= kWaveformBufferSize)
        length = kWaveformBufferSize - (kPadLeft + kPadRight);

    for (size_t i = 0; i < kPadLeft; i++)
        waveform_buffer_[i] = 0;
    for (size_t n = 0; n < length; n++)
        waveform_buffer_[n + kPadLeft] = (payload_[n] == '0') ? 0 : 1;
    for (size_t i = length + kPadLeft; i < kWaveformBufferSize; i++)
        waveform_buffer_[i] = 0;

    waveform_.set_length(static_cast<uint32_t>(length + kPadLeft + kPadRight));
    waveform_.set_dirty();
}

void OOKEditorView::update_from_file(const ook_editor::OokFileData& data) {
    field_frequency_.set_value(data.frequency, false);
    field_sample_rate_.set_by_value(static_cast<int32_t>(data.sample_rate), false);
    field_symbol_rate_.set_value(data.symbol_rate, false);
    field_repeat_.set_value(data.repeat, false);
    field_pause_.set_value(data.pause_symbol_duration, false);
    set_payload(data.payload);
}

void OOKEditorView::open_file() {
    auto* nav = globals().nav;
    if (!nav) return;
    stop_tx();

    core::ensure_directory(ook_editor::ook_directory());
    auto browser = std::make_unique<FileBrowserView>(
        ook_editor::ook_directory(), std::vector<std::string>{".OOK"});
    browser->on_selected = [this](const std::string& path) {
        ook_editor::OokFileData data{};
        if (read_ook_file(path, data)) {
            update_from_file(data);
            text_status_.set("Loaded " + core::filename(path));
        } else {
            text_status_.set("Load failed");
        }
    };
    nav->push(std::move(browser));
}

void OOKEditorView::save_file() {
    auto* nav = globals().nav;
    if (!nav) return;
    if (payload_.empty()) {
        text_status_.set("Nothing to save");
        return;
    }

    name_buffer_.clear();
    ui::text_prompt(*nav, name_buffer_, 30, ENTER_KEYBOARD_MODE_ALPHA,
                    [this](std::string& name) {
                        if (name.empty()) return;
                        core::ensure_directory(ook_editor::ook_directory());
                        ook_editor::OokFileData data{};
                        data.frequency = field_frequency_.value();
                        data.sample_rate =
                            static_cast<uint32_t>(field_sample_rate_.selected_index_value());
                        data.symbol_rate = static_cast<uint16_t>(field_symbol_rate_.value());
                        data.repeat = static_cast<uint16_t>(field_repeat_.value());
                        data.pause_symbol_duration =
                            static_cast<uint16_t>(field_pause_.value());
                        data.payload = payload_;
                        const std::string path =
                            core::path_join(ook_editor::ook_directory(), name + ".OOK");
                        if (save_ook_file(data, path))
                            text_status_.set("Saved " + name + ".OOK");
                        else
                            text_status_.set("Save failed");
                    });
}

void OOKEditorView::toggle_tx() {
    if (transmitting_)
        stop_tx();
    else
        start_tx();
}

void OOKEditorView::start_tx() {
    if (payload_.empty()) {
        text_status_.set("No payload");
        return;
    }
    auto* tx = globals().transmitter;
    if (!tx) {
        text_status_.set("No TX (needs B200)");
        return;
    }

    bit_count_ = ook_editor::pack_payload(payload_, bits_);
    if (bit_count_ == 0) {
        text_status_.set("No payload");
        return;
    }

    const uint32_t sample_rate =
        static_cast<uint32_t>(field_sample_rate_.selected_index_value());
    const double symbol_rate = std::max<int32_t>(1, field_symbol_rate_.value());

    auto keyer = std::make_shared<dsp::OokKeyer>();
    keyer->configure(static_cast<float>(sample_rate), static_cast<float>(symbol_rate));
    keyer->set_data(bits_.data(), bit_count_);

    /* Pause between repeats is given in microseconds in the file; convert to
     * whole symbol periods for the keyer. */
    const double us_per_symbol = 1'000'000.0 / symbol_rate;
    uint32_t pause_symbols = 0;
    if (us_per_symbol > 0.0)
        pause_symbols = static_cast<uint32_t>(field_pause_.value() / us_per_symbol);
    keyer->set_repeat(static_cast<uint32_t>(field_repeat_.value()), pause_symbols);

    keyer_ = keyer;

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(static_cast<double>(sample_rate));
    tx->set_target_frequency(field_frequency_.value());
    tx->set_iq_source([keyer](dsp::cfloat* out, size_t count) {
        return keyer->process(out, count);
    });

    if (!tx->start()) {
        text_status_.set("TX start failed (B200?)");
        stop_tx();
        return;
    }

    transmitting_ = true;
    button_send_.set_text("Stop");
    progressbar_.set_max(field_repeat_.value());
    progressbar_.set_value(0);
    text_status_.set("Sending...");
}

void OOKEditorView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    keyer_.reset();
    transmitting_ = false;
    button_send_.set_text("Send");
    progressbar_.set_value(0);
}

void OOKEditorView::on_frame_sync() {
    View::on_frame_sync();
    if (!transmitting_) return;

    auto k = keyer_;
    if (!k) return;

    progressbar_.set_value(k->repeats_sent());
    if (k->done()) {
        stop_tx();
        text_status_.set("Done");
    }
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_ook_editor{{
    "ook_editor", "OOK Editor", app::Category::Transmit,
    ui::Color::orange(), &ui::bitmap_icon_remote,
    [] { return std::make_unique<app::OOKEditorView>(); }}};
}  // namespace
