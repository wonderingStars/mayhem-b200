/*
 * mayhem-b200 — string formatting helpers.
 *
 * Same names and, importantly, the same output as the firmware's
 * application/string_format.cpp, so ported UI code lines up character for
 * character. The firmware's hand-rolled buffer arithmetic (written to avoid
 * heap churn on a 200 MHz M0) is replaced with straightforward std::string
 * code; the observable results are unchanged and covered by tests.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (original semantics)
 * Copyright (C) 2026 mayhem-b200 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version. See LICENSE.GPL-2.0-or-later.
 */

#ifndef __MB200_STRING_FORMAT_H__
#define __MB200_STRING_FORMAT_H__

#include <cstdint>
#include <string>
#include <string_view>

std::string to_string_dec_int(int64_t n);
std::string to_string_dec_uint(uint64_t n);

/* Right-justified in `l` columns. `fill` of 0 means pad with spaces. Values
 * wider than `l` are never truncated. */
std::string to_string_dec_uint(const uint64_t n, const int32_t l, const char fill = ' ');
std::string to_string_dec_int(const int64_t n, const int32_t l, const char fill = 0);

std::string to_string_bin(const uint32_t n, uint8_t l = 0);
std::string to_string_hex(uint64_t n, int32_t length);
std::string to_string_hex_array(const uint8_t* array, int32_t length);

std::string to_string_decimal(float decimal, int8_t precision);

/* "  437.500000" — the 10-column readout used by the frequency fields. */
std::string to_string_freq(const uint64_t f);
/* " 437.5000" — 4 integer digits, 4 decimals, rounded. */
std::string to_string_short_freq(const uint64_t f);
std::string to_string_rounded_freq(const uint64_t f, int8_t precision);

std::string to_string_time_ms(const uint32_t ms);
std::string to_string_file_size(uint64_t file_size);

/* Scales `n` by thousands and appends an SI prefix. base_unit indexes the
 * prefix table: 0=pico .. 4=unity .. 6=mega. */
std::string unit_auto_scale(double n, const uint32_t base_unit, uint32_t precision);

/* Local wall-clock timestamp, "YYYY-MM-DD HH:MM:SS". */
std::string to_string_datetime_now();
/* Compact form suitable for filenames, "YYYYMMDD_HHMMSS". */
std::string to_string_timestamp_now();

std::string trim(std::string_view str);
std::string trimr(std::string_view str);
std::string truncate(std::string_view str, size_t length);

#endif /*__MB200_STRING_FORMAT_H__*/
