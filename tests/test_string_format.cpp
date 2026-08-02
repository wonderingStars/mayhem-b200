/*
 * mayhem-b200 — string formatting tests.
 *
 * These lock the host helpers to the PortaPack firmware's output. Expected
 * values were derived by hand from firmware/application/string_format.cpp; the
 * padded-int cases in particular exist because the firmware pads the magnitude
 * before prepending the sign, which is easy to get wrong.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "string_format.hpp"

TEST(dec_uint_basic) {
    CHECK_STR_EQ(to_string_dec_uint(0u), "0");
    CHECK_STR_EQ(to_string_dec_uint(7u), "7");
    CHECK_STR_EQ(to_string_dec_uint(1234567890ull), "1234567890");
}

TEST(dec_int_basic) {
    CHECK_STR_EQ(to_string_dec_int(0), "0");
    CHECK_STR_EQ(to_string_dec_int(-1), "-1");
    CHECK_STR_EQ(to_string_dec_int(-1234567), "-1234567");
}

TEST(dec_int_min_does_not_overflow) {
    /* Negating INT64_MIN in signed space is UB; the helper must go through
     * unsigned. */
    CHECK_STR_EQ(to_string_dec_int(INT64_MIN), "-9223372036854775808");
}

TEST(dec_uint_padded) {
    CHECK_STR_EQ(to_string_dec_uint(42u, 5, '0'), "00042");
    CHECK_STR_EQ(to_string_dec_uint(42u, 5, ' '), "   42");
    /* Values wider than the field are never truncated. */
    CHECK_STR_EQ(to_string_dec_uint(123456u, 3, '0'), "123456");
}

TEST(dec_int_padded_sign_placement) {
    /* Magnitude is padded first, then the sign goes in front. */
    CHECK_STR_EQ(to_string_dec_int(-42, 5, '0'), "-0042");
    CHECK_STR_EQ(to_string_dec_int(42, 5, '0'), "00042");
    /* fill == 0 means "space pad the whole field", sign included. */
    CHECK_STR_EQ(to_string_dec_int(-42, 5, 0), "  -42");
    CHECK_STR_EQ(to_string_dec_int(437, 4, 0), " 437");
}

TEST(freq_10_column_readout) {
    /* to_string_freq is the 10-column form: 4 columns of MHz, 6 of Hz. */
    CHECK_STR_EQ(to_string_freq(437500000ull), " 437500000");
    CHECK_EQ(to_string_freq(437500000ull).size(), size_t{10});
    CHECK_STR_EQ(to_string_freq(1000000ull), "   1000000");
    CHECK_STR_EQ(to_string_freq(2450000000ull), "2450000000");
    /* Below 1 MHz it falls back to a plain right-justified integer. */
    CHECK_STR_EQ(to_string_freq(999999ull), "    999999");
    CHECK_EQ(to_string_freq(999999ull).size(), size_t{10});
}

TEST(short_freq_rounds_to_100hz) {
    CHECK_STR_EQ(to_string_short_freq(437500000ull), " 437.5000");
    CHECK_STR_EQ(to_string_short_freq(88500000ull), "  88.5000");
    /* +50 Hz rounding: 100 999 950 rounds up to 101.0000 MHz. */
    CHECK_STR_EQ(to_string_short_freq(100999950ull), " 101.0000");
}

TEST(rounded_freq_precision) {
    CHECK_STR_EQ(to_string_rounded_freq(437500000ull, 3), "437.500");
    CHECK_STR_EQ(to_string_rounded_freq(437500000ull, 1), "437.5");
    CHECK_STR_EQ(to_string_rounded_freq(437500000ull, 0), "437");
}

TEST(hex_formatting) {
    CHECK_STR_EQ(to_string_hex(0xDEADBEEFull, 8), "DEADBEEF");
    CHECK_STR_EQ(to_string_hex(0x5ull, 2), "05");
    /* Truncates from the left when the value does not fit. */
    CHECK_STR_EQ(to_string_hex(0xABCDull, 2), "CD");
}

TEST(hex_array) {
    const uint8_t bytes[] = {0x01, 0xAB, 0xFF};
    CHECK_STR_EQ(to_string_hex_array(bytes, 3), "01ABFF");
}

TEST(decimal_formatting) {
    CHECK_STR_EQ(to_string_decimal(1.5f, 2), "1.50");
    CHECK_STR_EQ(to_string_decimal(-1.25f, 2), "-1.25");
}

TEST(time_ms) {
    CHECK_STR_EQ(to_string_time_ms(500), "500ms");
    CHECK_STR_EQ(to_string_time_ms(1500), "1s");
    CHECK_STR_EQ(to_string_time_ms(61000), "1m1s");
}

TEST(file_size) {
    CHECK_STR_EQ(to_string_file_size(512), "512B");
    CHECK_STR_EQ(to_string_file_size(2048), "2kB");
    CHECK_STR_EQ(to_string_file_size(5ull * 1024 * 1024), "5MB");
    /* Must not run off the end of the suffix table on huge inputs. */
    CHECK_STR_EQ(to_string_file_size(3ull * 1024 * 1024 * 1024 * 1024), "3TB");
}

TEST(unit_auto_scale_steps_by_thousands) {
    /* base_unit 4 == unity. 2 000 000 -> "2.00M". */
    CHECK_STR_EQ(unit_auto_scale(2000000.0, 4, 2), "2.00M");
    CHECK_STR_EQ(unit_auto_scale(1500.0, 4, 1), "1.5k");
}

TEST(trim_and_truncate) {
    CHECK_STR_EQ(trim("  hello  "), "hello");
    CHECK_STR_EQ(trim("   "), "");
    CHECK_STR_EQ(trimr("hello   "), "hello");
    CHECK_STR_EQ(truncate("abcdef", 3), "abc");
    CHECK_STR_EQ(truncate("ab", 5), "ab");
}

TEST(binary_formatting) {
    CHECK_STR_EQ(to_string_bin(0b1010, 4), "1010");
    CHECK_STR_EQ(to_string_bin(0b1, 8), "00000001");
}
