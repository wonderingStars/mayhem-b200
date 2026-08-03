/*
 * mayhem-b200 — tests for the RPN calculator (ui_calculator / IVT engine).
 *
 * The IVT engine is a stack (RPN) machine: there is no operator precedence or
 * parentheses syntax — grouping is expressed by the order operands and
 * operators are entered. These tests drive the real virtual machine through the
 * pad-key interface and check the resulting stack. Expected values were
 * confirmed by running the engine standalone; they follow from RPN semantics.
 *
 * Pad key indices: 0=F 1=7 2=8 3=9 4=E 5=4 6=5 7=6 8=N 9=1 10=2 11=3 12=C
 * 13=0 14=. 15=D. F+D=+  F+3=-  F+6=*  F+9=/  F+0=PI  F+.=INT.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"
#include "ui_calculator.hpp"

#include <cmath>
#include <string>

using namespace mb200test;

namespace {

/* digit char -> pad key index */
int digit_key(char d) {
    static const int m[10] = {13, 9, 10, 11, 5, 6, 7, 1, 2, 3};
    return m[d - '0'];
}

/* Type a number string (digits and one optional '.'). */
void num(const char* s) {
    for (const char* p = s; *p; ++p) {
        if (*p == '.')
            app::calc::press(14);
        else
            app::calc::press(static_cast<uint8_t>(digit_key(*p)));
    }
}
void enter() { app::calc::press(15); }              // D: separate operands mid-entry
void op_add() { app::calc::press(0); app::calc::press(15); }
void op_sub() { app::calc::press(0); app::calc::press(11); }
void op_mul() { app::calc::press(0); app::calc::press(7); }
void op_div() { app::calc::press(0); app::calc::press(3); }
void op_pi() { app::calc::press(0); app::calc::press(13); }
void op_int() { app::calc::press(0); app::calc::press(14); }
void neg() { app::calc::press(8); }

std::string trimmed() {
    std::string s = app::calc::display();
    size_t a = s.find_first_not_of(' ');
    size_t b = s.find_last_not_of(' ');
    if (a == std::string::npos) return "";
    return s.substr(a, b - a + 1);
}

}  // namespace

TEST(calc_reset_empty) {
    app::calc::reset();
    CHECK_EQ(app::calc::depth(), 0);
    CHECK_NEAR(app::calc::top(), 0.0, 1e-12);
    CHECK(!app::calc::fmode());
}

TEST(calc_add) {
    app::calc::reset();
    num("2"); enter(); num("3"); op_add();
    CHECK_NEAR(app::calc::top(), 5.0, 1e-9);
    CHECK_EQ(app::calc::depth(), 1);
}

TEST(calc_subtract) {
    app::calc::reset();
    num("7"); enter(); num("4"); op_sub();
    CHECK_NEAR(app::calc::top(), 3.0, 1e-9);
    CHECK_EQ(app::calc::depth(), 1);
}

TEST(calc_subtract_negative_result) {
    app::calc::reset();
    num("3"); enter(); num("8"); op_sub();
    CHECK_NEAR(app::calc::top(), -5.0, 1e-9);
    CHECK_EQ(app::calc::depth(), 1);
}

TEST(calc_multiply) {
    app::calc::reset();
    num("3"); enter(); num("4"); op_mul();
    CHECK_NEAR(app::calc::top(), 12.0, 1e-9);
    CHECK_EQ(app::calc::depth(), 1);
}

TEST(calc_divide) {
    app::calc::reset();
    num("6"); enter(); num("2"); op_div();
    CHECK_NEAR(app::calc::top(), 3.0, 1e-9);
    CHECK_EQ(app::calc::depth(), 1);
}

TEST(calc_divide_fraction) {
    app::calc::reset();
    num("1"); enter(); num("4"); op_div();
    CHECK_NEAR(app::calc::top(), 0.25, 1e-9);
}

TEST(calc_multidigit_entry) {
    app::calc::reset();
    num("123");
    CHECK_NEAR(app::calc::top(), 123.0, 1e-9);
    CHECK_EQ(app::calc::depth(), 1);
}

TEST(calc_decimal_entry) {
    app::calc::reset();
    num("3.2");
    CHECK_NEAR(app::calc::top(), 3.2, 1e-6);
}

TEST(calc_decimal_add) {
    app::calc::reset();
    num("0.1"); enter(); num("0.2"); op_add();
    CHECK_NEAR(app::calc::top(), 0.3, 1e-6);
}

/* Grouping is by entry order (RPN). (2+3)*4 = 20. */
TEST(calc_grouping_sum_then_mul) {
    app::calc::reset();
    num("2"); enter(); num("3"); op_add();  // -> 5
    num("4"); op_mul();                     // 5 * 4
    CHECK_NEAR(app::calc::top(), 20.0, 1e-9);
    CHECK_EQ(app::calc::depth(), 1);
}

/* 2 + 3*4 = 14, entered as 3 4 * 2 + — the multiply binds first by order. */
TEST(calc_precedence_via_order) {
    app::calc::reset();
    num("3"); enter(); num("4"); op_mul();  // -> 12
    num("2"); op_add();                     // 12 + 2
    CHECK_NEAR(app::calc::top(), 14.0, 1e-9);
    CHECK_EQ(app::calc::depth(), 1);
}

/* Nested grouping: (1+2)*(3+4) = 21. */
TEST(calc_nested_grouping) {
    app::calc::reset();
    num("1"); enter(); num("2"); op_add();  // -> 3
    num("3"); enter(); num("4"); op_add();  // -> 7  (3 already on stack)
    op_mul();                               // 3 * 7
    CHECK_NEAR(app::calc::top(), 21.0, 1e-9);
    CHECK_EQ(app::calc::depth(), 1);
}

TEST(calc_negate) {
    app::calc::reset();
    num("5"); neg();
    CHECK_NEAR(app::calc::top(), -5.0, 1e-9);
}

TEST(calc_negative_operand_add) {
    /* After negate, isnewnumber is set, so the next number starts fresh with no
     * separator needed: -5 + 3 = -2. */
    app::calc::reset();
    num("5"); neg(); num("3"); op_add();
    CHECK_NEAR(app::calc::top(), -2.0, 1e-9);
    CHECK_EQ(app::calc::depth(), 1);
}

TEST(calc_pi) {
    app::calc::reset();
    op_pi();
    CHECK_NEAR(app::calc::top(), 3.14159265358979, 1e-6);
}

TEST(calc_int_truncation) {
    app::calc::reset();
    num("3.7"); op_int();
    CHECK_NEAR(app::calc::top(), 3.0, 1e-9);
    app::calc::reset();
    num("9.99"); op_int();
    CHECK_NEAR(app::calc::top(), 9.0, 1e-9);
}

TEST(calc_divide_by_zero_is_inf) {
    app::calc::reset();
    num("1"); enter(); num("0"); op_div();
    CHECK(std::isinf(app::calc::top()));
}

TEST(calc_empty_stack_add_is_graceful) {
    /* Popping an empty stack yields 0; an operator on an empty stack does not
     * crash and leaves 0. */
    app::calc::reset();
    op_add();
    CHECK_NEAR(app::calc::top(), 0.0, 1e-12);
    CHECK_EQ(app::calc::depth(), 1);
}

TEST(calc_single_operand_operator_graceful) {
    /* 5 then + with only one operand: 5 + 0 = 5, no crash. */
    app::calc::reset();
    num("5"); op_add();
    CHECK_NEAR(app::calc::top(), 5.0, 1e-9);
    CHECK_EQ(app::calc::depth(), 1);
}

TEST(calc_fmode_shift) {
    app::calc::reset();
    CHECK(!app::calc::fmode());
    app::calc::press(0);  // F
    CHECK(app::calc::fmode());
}

TEST(calc_display_formatting) {
    app::calc::reset();
    num("2"); enter(); num("3"); op_add();
    CHECK_STR_EQ(trimmed(), "5.");

    app::calc::reset();
    op_pi();
    CHECK_STR_EQ(trimmed(), "3.141593");

    app::calc::reset();
    num("0.1"); enter(); num("0.2"); op_add();
    CHECK_STR_EQ(trimmed(), "0.3");
}
