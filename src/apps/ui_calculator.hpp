/*
 * mayhem-b200 — RPN calculator (ported from Mayhem's calculator app).
 *
 * The engine is IVT (IVEE-TINY), a FORTH-like RPN scientific calculator by
 * zooxo/deetee (3-Clause BSD), which Mayhem embeds verbatim as ivt.hpp and
 * drives from a 16-key pad. This port carries the IVT virtual machine across
 * faithfully; only the AVR display/keypad/EEPROM plumbing (already #if'd out
 * upstream) is replaced with host glue. The EEPROM is a no-op here exactly as in
 * the Mayhem port, so the persistent features (user programs, user menu, STO/RCL
 * to EEPROM) are inert on both — the stack calculator and built-in functions are
 * what work.
 *
 * Upstream: application/external/calculator/{ivt.hpp,ui_calculator.*}
 *   IVT engine (C) 2021 zooxo/deetee, 3-Clause BSD
 *   Mayhem wrapper (C) 2023 Bernd Herzog
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_CALCULATOR_H__
#define __MB200_UI_CALCULATOR_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <cstdint>
#include <string>

namespace app {

/* Thin, testable interface to the IVT virtual machine. State is a single
 * file-scope instance in the .cpp (there is only ever one calculator view), so
 * call reset() before an independent sequence. Keys are the 4x4 pad indices:
 *
 *   0:F  1:7  2:8  3:9      In the un-shifted layout 1-3,5-7,9-11,13 are the
 *   4:E  5:4  6:5  7:6      digits, 8=N(negate) 4=E(EE exponent) 14='.' 15=D
 *   8:N  9:1 10:2 11:3      (DUP / "enter") 12=C (DROP / clear entry) 0=F
 *  12:C 13:0 14:. 15:D      (shift). Pressing F then an op key applies it:
 *                           F+9=/  F+6=*  F+3=-  F+D=+  F+0=PI  F+.=INT.
 */
namespace calc {

/* Reset the whole VM to power-on state (empty stack, no shift, fresh entry). */
void reset();

/* Feed one pad key (index 0..15) and run the VM to a quiescent state, mirroring
 * the upstream on_button_press() stepping. */
void press(uint8_t key);

/* Convenience: press a run of keys. */
void press_keys(const uint8_t* keys, size_t count);

double top();    /* top of the data stack, or 0.0 if empty */
int depth();     /* number of items on the data stack */
bool fmode();    /* true if the F (shift) key is armed */
std::string display();  /* the 10-char calculator readout after the last key */

}  // namespace calc

class CalculatorView : public ui::View {
   public:
    CalculatorView();

    std::string title() const override { return "Calculator"; }
    void on_show() override;

   private:
    void on_button_press(uint8_t button);
    void update_button_labels();

    ui::Console console{{0, 0, 240, 148}};
    ui::Text text_readout{{0, 150, 240, 16}, ""};

    /* 4x4 pad. Column = index%4 (x=col*60), row = index/4 (y=168+row*34). */
    ui::Button button_F{{0 * 60, 168 + 0 * 34, 58, 32}, "F"};
    ui::Button button_7{{1 * 60, 168 + 0 * 34, 58, 32}, "7"};
    ui::Button button_8{{2 * 60, 168 + 0 * 34, 58, 32}, "8"};
    ui::Button button_9{{3 * 60, 168 + 0 * 34, 58, 32}, "9"};
    ui::Button button_E{{0 * 60, 168 + 1 * 34, 58, 32}, "E"};
    ui::Button button_4{{1 * 60, 168 + 1 * 34, 58, 32}, "4"};
    ui::Button button_5{{2 * 60, 168 + 1 * 34, 58, 32}, "5"};
    ui::Button button_6{{3 * 60, 168 + 1 * 34, 58, 32}, "6"};
    ui::Button button_N{{0 * 60, 168 + 2 * 34, 58, 32}, "N"};
    ui::Button button_1{{1 * 60, 168 + 2 * 34, 58, 32}, "1"};
    ui::Button button_2{{2 * 60, 168 + 2 * 34, 58, 32}, "2"};
    ui::Button button_3{{3 * 60, 168 + 2 * 34, 58, 32}, "3"};
    ui::Button button_C{{0 * 60, 168 + 3 * 34, 58, 32}, "C"};
    ui::Button button_0{{1 * 60, 168 + 3 * 34, 58, 32}, "0"};
    ui::Button button_P{{2 * 60, 168 + 3 * 34, 58, 32}, "."};
    ui::Button button_D{{3 * 60, 168 + 3 * 34, 58, 32}, "D"};
};

}  // namespace app

#endif /*__MB200_UI_CALCULATOR_H__*/
