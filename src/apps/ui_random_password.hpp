/*
 * mayhem-b200 — Random password generator (ported from Mayhem's
 * random_password / ui_random_password).
 *
 * The upstream app harvests entropy from off-air AFSK samples via the M4
 * baseband and mixes it through an LCG roll and an optional SHA-512 pass. The
 * B200 host has no AFSK baseband wired up, so the radio-seed path is not
 * available; the generation algorithm (character sets, LCG roll, SHA-512
 * whitening) is ported faithfully and fed from the host CSPRNG instead. The
 * on-screen text says so — nothing here pretends to be radio-sourced.
 *
 * Upstream: application/external/random_password/*
 *   Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 *   Copyright (C) 2017 Furrtek / (C) 2024 zxkmm / (C) 2024 HTotoo
 *   SHA-512 modified from https://github.com/ulwanski/sha512 (@ulwanski)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_RANDOM_PASSWORD_H__
#define __MB200_UI_RANDOM_PASSWORD_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* Pure generation logic, testable without a display or radio. */
namespace rndpw {

constexpr int kMaxDigits = 30;  // upstream MAX_DIGITS

enum class Method {
    RollLCG,      // RADIO_LCG_ROLL     — LCG roll only
    RollLCGHash,  // RADIO_LCG_ROLL_HASH — LCG roll, then SHA-512 whitening
};

struct CharsetOptions {
    bool digits{true};
    bool latin_lower{true};
    bool latin_upper{true};
    bool punctuation{true};
    bool allow_confusable{false};  // include 0 O o 1 l
};

/* Build the selection alphabet exactly as upstream new_password() does. */
std::string build_charset(const CharsetOptions& opts);

/* SHA-512 of `input`, returned as a 128-char lowercase hex string. */
std::string sha512_hex(const std::string& input);

/* Faithful port of the upstream password generator.
 *
 * `seeds` supplies the LCG seeds; two are consumed per output character, so it
 * must hold at least length*2 entries (upstream waits for a full seed buffer).
 * Returns an empty string if the alphabet is empty or there are too few seeds.
 * Deterministic for a given (seeds, opts, length, method) on a given platform's
 * rand() implementation. */
std::string generate_password(const std::vector<unsigned int>& seeds,
                              const CharsetOptions& opts,
                              int length,
                              Method method);

}  // namespace rndpw

class RandomPasswordView : public ui::View {
   public:
    RandomPasswordView();

    std::string title() const override { return "R.passwd"; }
    void on_show() override;

   private:
    void new_password();
    std::vector<unsigned int> collect_seeds(int count);
    rndpw::CharsetOptions current_options() const;

    std::string password_{};

    ui::Labels labels{
        {{0, 0}, "Random password", ui::Color::light_grey()},
        {{0, 2 * 16}, "Length:", ui::Color::light_grey()},
        {{15 * 8, 2 * 16}, "Method:", ui::Color::light_grey()}};

    ui::Text text_note{
        {0, 1 * 16, 240, 16},
        "entropy: host RNG (no AFSK)"};

    ui::NumberField field_digits{
        {8 * 8, 2 * 16}, 2, {1, rndpw::kMaxDigits}, 1, ' '};

    ui::OptionsField field_method{
        {22 * 8, 2 * 16},
        7,
        {{"R+L+R", static_cast<int32_t>(rndpw::Method::RollLCG)},
         {"R+L+R+H", static_cast<int32_t>(rndpw::Method::RollLCGHash)}}};

    ui::Checkbox check_digits{{0, 4 * 16}, 3, "123"};
    ui::Checkbox check_latin_lower{{0, 6 * 16}, 3, "abc"};
    ui::Checkbox check_latin_upper{{0, 8 * 16}, 3, "ABC"};
    ui::Checkbox check_punctuation{{15 * 8, 4 * 16}, 6, ".,-!?"};
    ui::Checkbox check_allow_confusable{{15 * 8, 6 * 16}, 9, "0 O o 1 l"};

    ui::Labels label_result{
        {{0, 11 * 16}, "----------password----------", ui::Color::light_grey()}};

    ui::Text text_password{
        {0, 12 * 16, 240, 16},
        ""};

    ui::Button button_generate{
        {0, 15 * 16, 112, 32},
        "Generate"};
    ui::Button button_exit{
        {120, 15 * 16, 112, 32},
        "Back"};
};

}  // namespace app

#endif /*__MB200_UI_RANDOM_PASSWORD_H__*/
