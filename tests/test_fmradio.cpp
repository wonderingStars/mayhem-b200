/*
 * mayhem-b200 — FM Radio logic tests.
 *
 * Covers the parts of upstream's fmradio that are pure arithmetic and so are
 * checkable without a USRP: the MHz.dd button caption
 * (FmRadioView::to_nice_freq), the per-mode configuration table that
 * change_mode() applies, the empty-favourite fill rule, and the 25 kHz tuning
 * raster the app installs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "ui_fmradio.hpp"
#include "ui_freq_field.hpp"

using namespace app;

/* --- to_nice_freq --------------------------------------------------------- */

TEST(fmradio_nice_freq_whole_megahertz) {
    /* 87.00 MHz: (87000000/10000)%100 == 0, and to_string_dec_uint(0) == "0". */
    CHECK_STR_EQ(fmradio_to_nice_freq(87'000'000), "87.0");
    CHECK_STR_EQ(fmradio_to_nice_freq(100'000'000), "100.0");
}

TEST(fmradio_nice_freq_two_digit_fraction) {
    CHECK_STR_EQ(fmradio_to_nice_freq(87'500'000), "87.50");
    CHECK_STR_EQ(fmradio_to_nice_freq(88'750'000), "88.75");
    CHECK_STR_EQ(fmradio_to_nice_freq(107'900'000), "107.90");
}

TEST(fmradio_nice_freq_reproduces_upstream_missing_zero_pad) {
    /* Upstream builds the fraction with to_string_dec_uint() and no width, so
     * a leading-zero hundredth loses its zero: 87.05 prints as "87.5" and
     * 90.025 as "90.2". Reproduced deliberately — see the .hpp. */
    CHECK_STR_EQ(fmradio_to_nice_freq(87'050'000), "87.5");
    CHECK_STR_EQ(fmradio_to_nice_freq(90'025'000), "90.2");
}

TEST(fmradio_nice_freq_truncates_below_10_khz) {
    /* The fraction is in units of 10 kHz, so 25 kHz steps show as .2/.5/.7. */
    CHECK_STR_EQ(fmradio_to_nice_freq(88'025'000), "88.2");
    CHECK_STR_EQ(fmradio_to_nice_freq(88'075'000), "88.7");
}

/* --- change_mode() configuration table ------------------------------------ */

TEST(fmradio_mode_config_wfm) {
    const auto cfg = fmradio_mode_config(FmRadioModulation::Wfm);
    CHECK(cfg.mode == radio::ReceiverModel::Mode::WidebandFMAudio);
    CHECK_EQ(cfg.audio_rate_hz, 48'000u);
    CHECK_EQ(static_cast<int>(cfg.bandwidth_table), static_cast<int>(core::freqman_modulation_wfm));
    /* Upstream field_bw.set_by_value(0): the WFM table's "200k". */
    CHECK_EQ(cfg.default_bandwidth_value, 0);
    CHECK_STR_EQ(core::freqman_entry_get_bandwidth_string(
                     cfg.bandwidth_table,
                     core::freqman_find_bandwidth(cfg.bandwidth_table, "200k")),
                 "200k");
    CHECK_EQ(core::freqman_entry_get_bandwidth_value(
                 cfg.bandwidth_table,
                 core::freqman_find_bandwidth(cfg.bandwidth_table, "200k")),
             cfg.default_bandwidth_value);
}

TEST(fmradio_mode_config_nfm) {
    const auto cfg = fmradio_mode_config(FmRadioModulation::Nfm);
    CHECK(cfg.mode == radio::ReceiverModel::Mode::NarrowbandFMAudio);
    CHECK_EQ(cfg.audio_rate_hz, 24'000u);
    CHECK_EQ(static_cast<int>(cfg.bandwidth_table), static_cast<int>(core::freqman_modulation_nfm));
    CHECK_EQ(cfg.default_bandwidth_value, 2);
    /* Value 2 in the host's NfmConfig is the widest filter. */
    CHECK(static_cast<radio::ReceiverModel::NfmConfig>(cfg.default_bandwidth_value) ==
          radio::ReceiverModel::NfmConfig::Wide16k);
}

TEST(fmradio_mode_config_am_and_ssb) {
    const auto am = fmradio_mode_config(FmRadioModulation::Am);
    CHECK(am.mode == radio::ReceiverModel::Mode::AMAudio);
    CHECK_EQ(am.audio_rate_hz, 24'000u);
    CHECK_EQ(static_cast<int>(am.bandwidth_table), static_cast<int>(core::freqman_modulation_am));
    CHECK(am.am_config == radio::ReceiverModel::AmConfig::DSB9k);

    /* The documented fix for upstream's off-by-one: USB selects USB, LSB LSB. */
    const auto usb = fmradio_mode_config(FmRadioModulation::Usb);
    CHECK(usb.mode == radio::ReceiverModel::Mode::AMAudio);
    CHECK(usb.am_config == radio::ReceiverModel::AmConfig::USB);

    const auto lsb = fmradio_mode_config(FmRadioModulation::Lsb);
    CHECK(lsb.mode == radio::ReceiverModel::Mode::AMAudio);
    CHECK(lsb.am_config == radio::ReceiverModel::AmConfig::LSB);
}

TEST(fmradio_mode_config_audio_rate_is_48k_only_for_wfm) {
    const FmRadioModulation all[] = {FmRadioModulation::Am, FmRadioModulation::Nfm,
                                     FmRadioModulation::Wfm, FmRadioModulation::Usb,
                                     FmRadioModulation::Lsb};
    for (auto modulation : all) {
        const auto cfg = fmradio_mode_config(modulation);
        const uint32_t expected = (modulation == FmRadioModulation::Wfm) ? 48'000u : 24'000u;
        CHECK_EQ(cfg.audio_rate_hz, expected);
    }
}

/* --- favourites ----------------------------------------------------------- */

TEST(fmradio_empty_favourites_default_to_87mhz_wfm) {
    FmRadioFavourite favs[kFmRadioFavouriteSlots]{};
    /* One slot already holds something; it must not be touched. */
    favs[3].frequency = 101'700'000;
    favs[3].modulation = static_cast<int32_t>(FmRadioModulation::Nfm);
    favs[3].bandwidth = 2;

    fmradio_fill_empty_favourites(favs, kFmRadioFavouriteSlots);

    for (size_t i = 0; i < kFmRadioFavouriteSlots; ++i) {
        if (i == 3) continue;
        CHECK_EQ(favs[i].frequency, kFmRadioDefaultFrequency);
        CHECK_EQ(favs[i].modulation, static_cast<int32_t>(FmRadioModulation::Wfm));
        /* Upstream leaves bandwidth alone, and a fresh slot is already 0. */
        CHECK_EQ(static_cast<int>(favs[i].bandwidth), 0);
    }

    CHECK_EQ(favs[3].frequency, 101'700'000u);
    CHECK_EQ(favs[3].modulation, static_cast<int32_t>(FmRadioModulation::Nfm));
    CHECK_EQ(static_cast<int>(favs[3].bandwidth), 2);
}

TEST(fmradio_fill_favourites_tolerates_null) {
    fmradio_fill_empty_favourites(nullptr, 4);  /* must not crash */
    CHECK(true);
}

TEST(fmradio_default_favourite_caption) {
    FmRadioFavourite favs[2]{};
    fmradio_fill_empty_favourites(favs, 2);
    CHECK_STR_EQ(fmradio_to_nice_freq(favs[0].frequency), "87.0");
}

/* --- tuning raster -------------------------------------------------------- */

TEST(fmradio_step_is_in_the_frequency_field_table) {
    /* The app installs a 25 kHz step by looking it up in FrequencyField's
     * table; if that entry ever disappeared the lookup would silently leave
     * the default step in place. */
    /* The step is looked up in the table at run time, which is also what keeps
     * these comparisons out of the compiler's constant folder. */
    size_t index = ui::FrequencyField::step_count;
    for (size_t i = 0; i < ui::FrequencyField::step_count; ++i) {
        if (static_cast<uint64_t>(ui::FrequencyField::steps[i]) == kFmRadioFrequencyStep) {
            index = i;
        }
    }
    CHECK(index < ui::FrequencyField::step_count);
    if (index >= ui::FrequencyField::step_count) return;

    const uint64_t step = static_cast<uint64_t>(ui::FrequencyField::steps[index]);
    CHECK_EQ(step, 25'000u);
}

TEST(fmradio_step_reaches_both_broadcast_rasters) {
    size_t index = 0;
    for (size_t i = 0; i < ui::FrequencyField::step_count; ++i) {
        if (static_cast<uint64_t>(ui::FrequencyField::steps[i]) == kFmRadioFrequencyStep) {
            index = i;
        }
    }
    const uint64_t step = static_cast<uint64_t>(ui::FrequencyField::steps[index]);

    /* 25 kHz divides both the 100 kHz (ITU region 1) and 200 kHz (region 2)
     * channel spacings, so every broadcast channel is reachable. */
    CHECK_EQ(100'000u % step, 0u);
    CHECK_EQ(200'000u % step, 0u);

    /* Twenty steps up from 87.500 lands exactly on 88.000. */
    uint64_t f = 87'500'000;
    for (int i = 0; i < 20; ++i) f += step;
    CHECK_EQ(f, 88'000'000u);
    CHECK_STR_EQ(fmradio_to_nice_freq(f), "88.0");
}

TEST(fmradio_band_edges_render) {
    /* The two ends of the broadcast band, as the favourite buttons show them. */
    CHECK_STR_EQ(fmradio_to_nice_freq(87'500'000), "87.50");
    CHECK_STR_EQ(fmradio_to_nice_freq(108'000'000), "108.0");
    /* Japan's band starts lower; the caption still works. */
    CHECK_STR_EQ(fmradio_to_nice_freq(76'100'000), "76.10");
}
