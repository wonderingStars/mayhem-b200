/*
 * mayhem-b200 — FM Radio (broadcast listening app).
 *
 * Ported from firmware/application/external/fmradio/. Upstream is a listening
 * front end for the ordinary analogue demodulators: a frequency field stepping
 * in 25 kHz, a modulation selector (AM / NFM / WFM / USB / LSB), a per-mode
 * bandwidth selector fed from the freqman bandwidth tables, ten one-touch
 * favourite buttons with a Save arming button, volume/gain/RSSI, and an audio
 * spectrum visualiser (a waveform that toggles to a graphic-EQ view).
 *
 * WHAT IS AND IS NOT PORTED
 *   - Tuning, modulation, bandwidth, favourites (including their persistence
 *     under the same setting key names upstream uses), volume, gain and the
 *     RSSI/audio meters are ported.
 *   - The 128-bin audio spectrum visualiser is NOT driven. Upstream receives an
 *     AudioSpectrumMessage from the M4's WFM processor; the host's audio path
 *     (radio::ReceiverModel -> audio::AudioOut) exposes only a peak level
 *     (AudioOut::take_peak()), not the post-demodulation audio samples. The
 *     view therefore shows a real audio level meter and says plainly on screen
 *     that the spectrum needs an audio-domain tap that does not exist yet.
 *     IDEAL TAP: ReceiverModel::take_audio_samples(std::vector<float>&, size_t)
 *     delivering the 48 kHz mono block it just handed to AudioOut — an FFT of
 *     that is exactly upstream's AudioSpectrum.
 *
 * DOCUMENTED DEVIATION — upstream's SSB index bug
 *   Upstream's change_mode() calls
 *       receiver_model.set_am_configuration(field_modulation.selected_index() == 3 ? 1 : 2)
 *   for USB / LSB. ReceiverModel::set_am_configuration() takes the freqman AM
 *   bandwidth index, whose table is {DSB 9k=0, DSB 6k=1, USB+3k=2, LSB-3k=3,
 *   CW=4} — so upstream selects "DSB 6k" for USB and "USB+3k" for LSB, one slot
 *   low in both cases. The host maps USB->AmConfig::USB and LSB->AmConfig::LSB,
 *   which is what the control claims to do.
 *
 * Copyright (C) 2024 HTotoo
 * Copyright (C) 2025 RocketGod
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_FMRADIO_H__
#define __MB200_UI_FMRADIO_H__

#include "../core/freqman_db.hpp"
#include "../core/settings.hpp"
#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace app {

/* Upstream stores twelve favourite slots but only shows ten buttons; both
 * numbers are kept so the settings file stays compatible. */
constexpr size_t kFmRadioFavouriteSlots = 12;
constexpr size_t kFmRadioFavouriteButtons = 10;

/* Upstream's default for an empty slot, and for an unset frequency field. */
constexpr uint64_t kFmRadioDefaultFrequency = 87'000'000;

/* Upstream sets this once in the constructor and never changes it per mode. */
constexpr uint64_t kFmRadioFrequencyStep = 25'000;

/* Upstream tunes the front end the same way for every mode. */
constexpr double kFmRadioSamplingRate = 3'072'000.0;
constexpr double kFmRadioBasebandBandwidth = 1'750'000.0;

/* The five entries of upstream's field_modulation, in order. Three of them map
 * onto the AM demodulator; the index is what tells them apart. */
enum class FmRadioModulation : uint8_t {
    Am = 0,
    Nfm = 1,
    Wfm = 2,
    Usb = 3,
    Lsb = 4,
};

struct FmRadioModeConfig {
    radio::ReceiverModel::Mode mode;
    /* 48 kHz for WFM, 24 kHz for everything else — upstream's
     * audio_sampling_rate assignments. */
    uint32_t audio_rate_hz;
    /* Which freqman bandwidth table feeds the bandwidth selector. */
    core::freqman_index_t bandwidth_table;
    /* The value upstream passes to field_bw.set_by_value() for this mode. */
    int32_t default_bandwidth_value;
    /* Only meaningful when mode == AMAudio. */
    radio::ReceiverModel::AmConfig am_config;
};

/* Pure: the whole of upstream change_mode()'s configuration table.
 * Tested in tests/test_fmradio.cpp. Inline so the tests can exercise it
 * without linking the view (and so the whole UI layer). */
inline FmRadioModeConfig fmradio_mode_config(FmRadioModulation modulation) {
    using Mode = radio::ReceiverModel::Mode;
    using AmConfig = radio::ReceiverModel::AmConfig;

    switch (modulation) {
        case FmRadioModulation::Nfm:
            /* Upstream: audio 24 kHz, NFM_MODULATION table, set_by_value(2).
             * The freqman NFM table is {8k5=0, 11k=1, 12k5=2, 16k=3}; on the
             * host ReceiverModel::NfmConfig has three entries and value 2 is
             * Wide16k, which is what upstream's comment says it wanted. */
            return {Mode::NarrowbandFMAudio, 24'000, core::freqman_modulation_nfm, 2,
                    AmConfig::DSB9k};

        case FmRadioModulation::Wfm:
            /* Upstream: audio 48 kHz, WFM_MODULATION table, set_by_value(0)
             * = "200k". */
            return {Mode::WidebandFMAudio, 48'000, core::freqman_modulation_wfm, 0,
                    AmConfig::DSB9k};

        case FmRadioModulation::Usb:
            return {Mode::AMAudio, 24'000, core::freqman_modulation_am, 0, AmConfig::USB};

        case FmRadioModulation::Lsb:
            return {Mode::AMAudio, 24'000, core::freqman_modulation_am, 0, AmConfig::LSB};

        case FmRadioModulation::Am:
        default:
            /* Upstream: audio 24 kHz, AM_MODULATION table, set_by_value(0)
             * = "DSB 9k", am_configuration 0. */
            return {Mode::AMAudio, 24'000, core::freqman_modulation_am, 0, AmConfig::DSB9k};
    }
}

/* Port of FmRadioView::to_nice_freq():
 *     MHz "." ((freq / 10000) % 100)
 * Upstream does not zero-pad the fraction, so 87.05 MHz prints as "87.5".
 * That is reproduced deliberately — it is what the favourite buttons show on a
 * PortaPack. Tested both ways. */
inline std::string fmradio_to_nice_freq(uint64_t frequency_hz) {
    std::string nice = to_string_dec_uint(frequency_hz / 1'000'000);
    nice += ".";
    nice += to_string_dec_uint((frequency_hz / 10'000) % 100);
    return nice;
}

struct FmRadioFavourite {
    uint64_t frequency{0};
    int32_t modulation{static_cast<int32_t>(FmRadioModulation::Wfm)};
    uint8_t bandwidth{0};
};

/* Port of the constructor's fill loop: any slot left at 0 Hz becomes 87.00 MHz
 * WFM. Upstream leaves `bandwidth` alone, which is already 0. */
inline void fmradio_fill_empty_favourites(FmRadioFavourite* favourites, size_t count) {
    if (favourites == nullptr) return;
    for (size_t i = 0; i < count; ++i) {
        if (favourites[i].frequency == 0) {
            favourites[i].frequency = kFmRadioDefaultFrequency;
            favourites[i].modulation = static_cast<int32_t>(FmRadioModulation::Wfm);
        }
    }
}

class FmRadioView : public ui::View {
   public:
    FmRadioView();
    ~FmRadioView() override;

    FmRadioView(const FmRadioView&) = delete;
    FmRadioView& operator=(const FmRadioView&) = delete;

    std::string title() const override { return "Radio"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void change_mode(FmRadioModulation modulation);
    void populate_bandwidth_options(core::freqman_index_t table, int32_t default_value);
    void on_favourite_clicked(size_t index);
    void update_favourite_texts();
    void update_meters();

    radio::ReceiverModel& receiver_;

    uint32_t frame_counter_{0};
    bool save_armed_{false};
    FmRadioModulation modulation_{FmRadioModulation::Wfm};

    /* Persisted state. Declared before settings_ so the store's constructor
     * writes into live members. Key names match upstream's "rx_fmradio". */
    uint64_t fav_freq_[kFmRadioFavouriteSlots]{};
    int32_t fav_mod_[kFmRadioFavouriteSlots]{};
    uint8_t fav_bw_[kFmRadioFavouriteSlots]{};
    uint8_t radio_bw_{0};
    uint32_t current_theme_{0};

    core::SettingsStore settings_{
        "rx_fmradio",
        {{"favlist0_freq", &fav_freq_[0]},   {"favlist1_freq", &fav_freq_[1]},
         {"favlist2_freq", &fav_freq_[2]},   {"favlist3_freq", &fav_freq_[3]},
         {"favlist4_freq", &fav_freq_[4]},   {"favlist5_freq", &fav_freq_[5]},
         {"favlist6_freq", &fav_freq_[6]},   {"favlist7_freq", &fav_freq_[7]},
         {"favlist8_freq", &fav_freq_[8]},   {"favlist9_freq", &fav_freq_[9]},
         {"favlist10_freq", &fav_freq_[10]}, {"favlist11_freq", &fav_freq_[11]},
         {"favlist0_mod", &fav_mod_[0]},     {"favlist1_mod", &fav_mod_[1]},
         {"favlist2_mod", &fav_mod_[2]},     {"favlist3_mod", &fav_mod_[3]},
         {"favlist4_mod", &fav_mod_[4]},     {"favlist5_mod", &fav_mod_[5]},
         {"favlist6_mod", &fav_mod_[6]},     {"favlist7_mod", &fav_mod_[7]},
         {"favlist8_mod", &fav_mod_[8]},     {"favlist9_mod", &fav_mod_[9]},
         {"favlist10_mod", &fav_mod_[10]},   {"favlist11_mod", &fav_mod_[11]},
         {"favlist0_bw", &fav_bw_[0]},       {"favlist1_bw", &fav_bw_[1]},
         {"favlist2_bw", &fav_bw_[2]},       {"favlist3_bw", &fav_bw_[3]},
         {"favlist4_bw", &fav_bw_[4]},       {"favlist5_bw", &fav_bw_[5]},
         {"favlist6_bw", &fav_bw_[6]},       {"favlist7_bw", &fav_bw_[7]},
         {"favlist8_bw", &fav_bw_[8]},       {"favlist9_bw", &fav_bw_[9]},
         {"favlist10_bw", &fav_bw_[10]},     {"favlist11_bw", &fav_bw_[11]},
         {"radio_bw", &radio_bw_},
         {"theme", &current_theme_}}};

    /* --- Widgets --- */

    ui::FrequencyField field_frequency_{{0, 0}};
    ui::FrequencyStepView step_view_{{84, 0}, field_frequency_};

    ui::Labels labels_{
        {{132, 0}, "G", ui::Color::light_grey()},
        {{188, 0}, "V", ui::Color::light_grey()},
        {{0, 18}, "RF", ui::Color::light_grey()},
        {{0, 32}, "AF", ui::Color::light_grey()},
        {{0, 50}, "MOD", ui::Color::light_grey()},
        {{88, 50}, "BW", ui::Color::light_grey()},
    };

    ui::NumberField field_gain_{{144, 0}, 3, {0, 76}, 1, ' '};
    ui::NumberField field_volume_{{200, 0}, 2, {0, 99}, 1, ' '};

    ui::VuMeter rf_meter_{{24, 18, 212, 12}, 24, true};
    ui::VuMeter af_meter_{{24, 32, 212, 12}, 24, false};

    ui::OptionsField options_modulation_{
        {32, 50},
        4,
        {{"AM  ", static_cast<int32_t>(FmRadioModulation::Am)},
         {"NFM ", static_cast<int32_t>(FmRadioModulation::Nfm)},
         {"WFM ", static_cast<int32_t>(FmRadioModulation::Wfm)},
         {"USB ", static_cast<int32_t>(FmRadioModulation::Usb)},
         {"LSB ", static_cast<int32_t>(FmRadioModulation::Lsb)}}};

    ui::OptionsField options_bw_{{112, 50}, 6, {}};

    ui::Button button_fav_save_{{0, 70, 64, 22}, "Save"};
    ui::Text text_save_help_{{72, 72, 168, 16}, ""};

    std::array<ui::Button, kFmRadioFavouriteButtons> buttons_fav_{
        ui::Button{{6, 98, 108, 26}, "---"},
        ui::Button{{126, 98, 108, 26}, "---"},
        ui::Button{{6, 128, 108, 26}, "---"},
        ui::Button{{126, 128, 108, 26}, "---"},
        ui::Button{{6, 158, 108, 26}, "---"},
        ui::Button{{126, 158, 108, 26}, "---"},
        ui::Button{{6, 188, 108, 26}, "---"},
        ui::Button{{126, 188, 108, 26}, "---"},
        ui::Button{{6, 218, 108, 26}, "---"},
        ui::Button{{126, 218, 108, 26}, "---"}};

    ui::Labels na_notes_{
        {{0, 254}, "Not available on B200:", ui::Color::yellow()},
        {{0, 270}, " audio spectrum / GraphEq", ui::Color::grey()},
        {{0, 286}, " needs an audio-domain tap", ui::Color::grey()},
    };
};

}  // namespace app

#endif /*__MB200_UI_FMRADIO_H__*/
