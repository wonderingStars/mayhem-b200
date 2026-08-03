/*
 * mayhem-b200 — FLEX pager transmitter (implementation).
 *
 * Copyright (C) 2023-2024 PortaPack Mayhem contributors (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_flex_tx.hpp"

#include "../core/string_format.hpp"
#include "../radio/transmitter_model.hpp"
#include "../radio/usrp_radio.hpp"
#include "app_context.hpp"
#include "ui_alphanum.hpp"
#include "ui_modal.hpp"
#include "ui_navigation.hpp"

#include <algorithm>
#include <complex>
#include <ctime>
#include <memory>

namespace app::flex_tx {

/* ===========================================================================
 * FlexParamsView
 * ===========================================================================*/

FlexParamsView::FlexParamsView(FlexBIWParams& params)
    : params_(params) {
    add_children({&labels_,
                  &check_date_, &field_year_, &field_month_, &field_day_,
                  &check_time_, &field_hour_, &field_minute_, &field_second_,
                  &check_tz_, &options_tz_, &check_dst_,
                  &check_ssid1_, &field_local_id_, &labels_cz_, &field_coverage_,
                  &check_ssid2_, &field_country_, &check_roaming_,
                  &labels_msg_, &field_msg_number_,
                  &check_ers_, &field_ers_count_,
                  &button_save_, &button_cancel_});

    check_date_.set_value(params_.send_date);
    field_year_.set_value(params_.year);
    field_month_.set_value(params_.month);
    field_day_.set_value(params_.day);

    check_time_.set_value(params_.send_time);
    field_hour_.set_value(params_.hour);
    field_minute_.set_value(params_.minute);
    field_second_.set_value(params_.second);

    check_tz_.set_value(params_.send_tz);
    options_tz_.set_by_value(params_.tz_code);
    check_dst_.set_value(params_.send_dst);

    check_ssid1_.set_value(params_.send_ssid1);
    field_local_id_.set_value(params_.local_id);
    field_coverage_.set_value(params_.coverage_zone);

    check_ssid2_.set_value(params_.send_ssid2);
    field_country_.set_value(params_.country_code);
    check_roaming_.set_value(params_.roaming);

    field_msg_number_.set_value(params_.msg_number);

    check_ers_.set_value(params_.send_ers);
    field_ers_count_.set_value(params_.ers_count);

    check_roaming_.on_select = [this](ui::Checkbox&, bool v) {
        on_roaming_changed(v);
    };

    button_save_.on_select = [this](ui::Button&) {
        auto* nav = globals().nav;
        int32_t yr = field_year_.value();
        if (yr > 2025) {
            int equiv = flex_equiv_year(yr);
            if (nav)
                ui::display_modal(*nav, "Year > 2025",
                                  "FLEX carries 1994-2025.\nEquivalent for " +
                                      to_string_dec_uint(static_cast<uint32_t>(yr)) + ":\n" +
                                      to_string_dec_uint(static_cast<uint32_t>(equiv)));
            field_year_.set_value(equiv);
            return;
        }
        params_.send_date = check_date_.value();
        params_.year = yr;
        params_.month = field_month_.value();
        params_.day = field_day_.value();
        params_.send_time = check_time_.value();
        params_.hour = field_hour_.value();
        params_.minute = field_minute_.value();
        params_.second = field_second_.value();
        params_.send_tz = check_tz_.value();
        params_.tz_code = options_tz_.selected_index_value();
        params_.send_dst = check_dst_.value();
        params_.send_ssid1 = check_ssid1_.value();
        params_.local_id = field_local_id_.value();
        params_.coverage_zone = field_coverage_.value();
        params_.send_ssid2 = check_ssid2_.value();
        params_.country_code = field_country_.value();
        params_.roaming = check_roaming_.value();
        params_.msg_number = field_msg_number_.value();
        params_.send_ers = check_ers_.value();
        params_.ers_count = field_ers_count_.value();
        if (nav) nav->pop();
    };

    button_cancel_.on_select = [](ui::Button&) {
        if (auto* nav = globals().nav) nav->pop();
    };
}

void FlexParamsView::on_roaming_changed(bool v) {
    if (v) {
        check_ssid1_.set_value(true);
        check_ssid2_.set_value(true);
    }
}

void FlexParamsView::focus() {
    button_save_.focus();
}

/* ===========================================================================
 * FlexTXView
 * ===========================================================================*/

FlexTXView::FlexTXView() {
    /* Fill date/time from the system clock — the host equivalent of upstream
     * reading the RTC. */
    {
        std::time_t t = std::time(nullptr);
        std::tm lt{};
#if defined(_WIN32)
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        biw_params_.year = flex_equiv_year(lt.tm_year + 1900);
        biw_params_.month = lt.tm_mon + 1;
        biw_params_.day = lt.tm_mday;
        biw_params_.hour = lt.tm_hour;
        biw_params_.minute = lt.tm_min;
        biw_params_.second = lt.tm_sec;
        int seed = lt.tm_sec + lt.tm_min * 7 + lt.tm_hour;
        biw_params_.msg_number = seed & 63;
        if (biw_params_.msg_number == 0)
            biw_params_.msg_number = 1;
    }

    add_children({&labels_,
                  &field_freq_,
                  &field_capcode_,
                  &options_speed_,
                  &options_type_,
                  &text_capinfo_,
                  &text_message_,
                  &text_message_l2_,
                  &button_message_,
                  &button_params_,
                  &button_tx_,
                  &progressbar_,
                  &text_warning_,
                  &text_status_});

    text_warning_.set(STR_COLOR_YELLOW "Illegal to radiate in most areas");

    options_speed_.set_selected_index(0);
    options_type_.set_selected_index(0);
    field_capcode_.set_value(static_cast<uint64_t>(1000));

    if (auto* r = globals().radio) {
        const auto& caps = r->caps();
        field_freq_.set_range(static_cast<uint64_t>(caps.tx_freq.min),
                              static_cast<uint64_t>(caps.tx_freq.max));
    }
    if (auto* tx = globals().transmitter)
        field_freq_.set_value(tx->target_frequency(), false);
    else
        field_freq_.set_value(931'740'000, false);

    field_capcode_.on_change = [this](ui::SymField&) { update_capcode_info(); };

    button_message_.on_select = [this](ui::Button&) { set_message(); };

    button_params_.on_select = [this](ui::Button&) {
        if (auto* nav = globals().nav)
            nav->push(std::make_unique<FlexParamsView>(biw_params_));
    };

    button_tx_.on_select = [this](ui::Button&) {
        if (transmitting_)
            stop_tx();
        else
            start_tx();
    };

    update_message_text();
    update_capcode_info();
    progressbar_.set_max(1000);
}

FlexTXView::~FlexTXView() {
    stop_tx();
}

void FlexTXView::focus() {
    field_capcode_.focus();
}

void FlexTXView::on_show() {
    ui::View::on_show();
    update_message_text();
    update_capcode_info();
}

void FlexTXView::on_hide() {
    stop_tx();
    ui::View::on_hide();
}

void FlexTXView::set_message() {
    auto* nav = globals().nav;
    if (!nav) return;
    ui::text_prompt(*nav, message_, 240, ENTER_KEYBOARD_MODE_ALPHA,
                    [this](std::string&) { update_message_text(); });
}

void FlexTXView::update_message_text() {
    if (message_.length() <= 30) {
        text_message_.set(message_);
        text_message_l2_.set("");
    } else if (message_.length() <= 60) {
        text_message_.set(message_.substr(0, 29));
        text_message_l2_.set(message_.substr(29));
    } else {
        text_message_.set(message_.substr(0, 29));
        text_message_l2_.set(message_.substr(29, 27) + "...");
    }
}

void FlexTXView::update_capcode_info() {
    const uint64_t cap = field_capcode_.to_integer();
    std::string info;
    if (cap == 0) {
        info = "Invalid";
    } else if (cap <= 1933312ULL) {
        info = "Short addr";
    } else if (cap >= 2062336ULL && cap <= 2062351ULL) {
        info = "Temp grp #" + to_string_dec_uint(static_cast<uint32_t>(cap - 2062336ULL));
    } else if (cap >= 2062352ULL && cap <= 2062367ULL) {
        info = "Operator msg";
    } else if (cap >= 2058240ULL && cap <= 2062335ULL) {
        info = "Network addr";
    } else if (cap >= 2041856ULL && cap <= 2058239ULL) {
        info = "Info service";
    } else if (cap > 1933312ULL && cap < 2101249ULL) {
        info = "Reserved";
    } else if (cap >= 2101249ULL && cap <= max_capcode) {
        info = "Long addr";
    } else {
        info = "Invalid";
    }
    if (cap >= 1 && cap <= max_capcode) {
        static const char ph[] = "ABCD";
        int frame = (int)((cap / 16) % 128);
        int phase = (int)((cap / 4) % 4);
        info += ", F" + to_string_dec_uint(static_cast<uint32_t>(frame)) +
                " " + std::string(1, ph[phase]);
    }
    text_capinfo_.set(info);
}

bool FlexTXView::start_tx() {
    const uint64_t capcode = field_capcode_.to_integer();
    if (capcode < min_capcode || capcode > max_capcode) {
        ui::display_modal("Bad capcode", "Capcode range:\n1 - 4297068542");
        return false;
    }

    const int msg_type = static_cast<int>(options_type_.selected_index());
    if (msg_type == 1) {
        if (message_.find_first_not_of("0123456789.U -][") != std::string::npos) {
            ui::display_modal("Bad message",
                              "Numeric may only contain\n0-9 . U - ] [ or space.");
            return false;
        }
    }

    tx_bytes_.clear();
    if (!flex_build_transmission(tx_bytes_, capcode, msg_type, message_, biw_params_)) {
        ui::display_modal("Encode error", "Could not encode the\nlong address.");
        return false;
    }

    fsk_.configure(static_cast<float>(sample_rate_hz),
                   static_cast<float>(symbol_rate), deviation_hz);
    fsk_.set_gaussian(0.0f);  // 2FSK, hard-keyed
    fsk_.set_repeat(1, 0);
    fsk_.set_data(tx_bytes_.data(), tx_bytes_.size() * 8);

    total_samples_ = static_cast<uint64_t>(
        static_cast<double>(tx_bytes_.size() * 8) * (sample_rate_hz / symbol_rate));
    produced_samples_.store(0);
    tx_done_.store(false);

    auto* tx = globals().transmitter;
    if (!tx) {
        text_status_.set(STR_COLOR_RED "No transmitter (needs USRP B200)");
        return false;
    }

    tx->set_mode(radio::TransmitterModel::Mode::Raw);
    tx->set_sampling_rate(sample_rate_hz);
    tx->set_target_frequency(field_freq_.value());
    tx->set_iq_source([this](std::complex<float>* out, size_t n) -> size_t {
        const size_t w = fsk_.process(out, n);
        produced_samples_.fetch_add(w);
        if (w < n) tx_done_.store(true);
        return w;
    });

    if (!tx->start()) {
        tx->set_iq_source(nullptr);
        std::string err = "TX start failed (needs USRP B200)";
        if (auto* r = globals().radio) {
            const auto& e = r->last_error();
            if (!e.empty()) err = e;
        }
        text_status_.set(STR_COLOR_RED + err);
        return false;
    }

    transmitting_ = true;
    button_tx_.set_text("Stop");
    text_status_.set(STR_COLOR_GREEN "Transmitting 1600 bps");
    return true;
}

void FlexTXView::stop_tx() {
    if (auto* tx = globals().transmitter) {
        tx->stop();
        tx->set_iq_source(nullptr);
    }
    if (transmitting_) {
        transmitting_ = false;
        button_tx_.set_text("Start TX");
        progressbar_.set_value(0);
        text_status_.set("");
        /* Advance the message number like upstream does after a send. */
        biw_params_.msg_number = (biw_params_.msg_number + 1) & 63;
    }
}

void FlexTXView::on_frame_sync() {
    ui::View::on_frame_sync();
    if (!transmitting_) return;

    if (total_samples_ > 0) {
        const uint64_t p = std::min(produced_samples_.load(), total_samples_);
        progressbar_.set_value(static_cast<uint32_t>(p * 1000 / total_samples_));
    }

    if (tx_done_.load()) {
        stop_tx();
        text_status_.set(STR_COLOR_GREEN "Sent");
    }
}

}  // namespace app::flex_tx

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
const app::Registrar reg_flex_tx{{
    "flex_tx",
    "FLEX TX",
    app::Category::Transmit,
    ui::Color::cyan(),
    &ui::bitmap_icon_pocsag,
    [] { return std::make_unique<app::flex_tx::FlexTXView>(); },
    false}};
}  // namespace
