/*
 * mayhem-b200 — string formatting helpers.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (original semantics)
 * Copyright (C) 2026 mayhem-b200 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version. See LICENSE.GPL-2.0-or-later.
 */

#include "string_format.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace {

const char unit_prefix[7] = {'p', 'n', 'u', 'm', 0, 'k', 'M'};

std::string pad_left(std::string s, int32_t l, char fill) {
    if (fill == 0) fill = ' ';
    while (static_cast<int32_t>(s.size()) < l) s.insert(s.begin(), fill);
    return s;
}

}  // namespace

std::string to_string_dec_uint(uint64_t n) {
    /* Built back to front, same as the firmware, so 0 yields "0". */
    char buf[24];
    char* p = buf + sizeof(buf);
    *--p = '\0';
    char* q = p;
    do {
        *--q = static_cast<char>('0' + (n % 10));
        n /= 10;
    } while (n != 0);
    return std::string(q, p - q);
}

std::string to_string_dec_int(int64_t n) {
    if (n < 0) {
        /* Negate in unsigned space so INT64_MIN does not overflow. */
        const uint64_t magnitude = ~static_cast<uint64_t>(n) + 1ULL;
        return "-" + to_string_dec_uint(magnitude);
    }
    return to_string_dec_uint(static_cast<uint64_t>(n));
}

std::string to_string_dec_uint(const uint64_t n, const int32_t l, const char fill) {
    return pad_left(to_string_dec_uint(n), l, fill);
}

std::string to_string_dec_int(const int64_t n, const int32_t l, const char fill) {
    /* Matches the firmware exactly, including the order of operations: the
     * magnitude is fill-padded to (l - sign) first, the '-' is prepended after,
     * and only then is the whole thing space-padded out to l. That is why
     * (-42, 5, '0') yields "-0042" and not "00-42". */
    const size_t negative = (n < 0) ? 1 : 0;
    const uint64_t n_abs = negative ? (~static_cast<uint64_t>(n) + 1ULL)
                                    : static_cast<uint64_t>(n);

    constexpr int32_t buffer_limit = 23;
    const int32_t safe_l = std::min<int32_t>(l, buffer_limit);

    std::string s = to_string_dec_uint(n_abs);
    if (fill) {
        while (static_cast<int32_t>(s.size()) < safe_l - static_cast<int32_t>(negative))
            s.insert(s.begin(), fill);
    }
    if (negative) s.insert(s.begin(), '-');
    while (static_cast<int32_t>(s.size()) < safe_l)
        s.insert(s.begin(), fill ? fill : ' ');

    return s;
}

std::string to_string_bin(const uint32_t n, uint8_t l) {
    if (l >= 33) l = 32;
    std::string s;
    s.reserve(l);
    for (uint8_t c = 0; c < l; c++)
        s.push_back((n & (1UL << (l - 1 - c))) ? '1' : '0');
    return s;
}

std::string to_string_hex(uint64_t n, int32_t length) {
    static const char hex[] = "0123456789ABCDEF";
    if (length < 1) length = 1;
    if (length > 16) length = 16;

    std::string s(static_cast<size_t>(length), '0');
    for (int32_t i = length - 1; i >= 0; i--) {
        s[static_cast<size_t>(i)] = hex[n & 0xF];
        n >>= 4;
    }
    return s;
}

std::string to_string_hex_array(const uint8_t* array, int32_t length) {
    std::string s;
    if (array == nullptr) return s;
    s.reserve(static_cast<size_t>(length) * 2);
    for (int32_t i = 0; i < length; i++)
        s += to_string_hex(array[i], 2);
    return s;
}

std::string to_string_decimal(float decimal, int8_t precision) {
    double integer_part = 0.0;
    if (precision > 9) precision = 9;
    if (precision < 0) precision = 0;

    double fractional_part = std::modf(static_cast<double>(decimal), &integer_part) *
                             std::pow(10.0, precision);
    if (fractional_part < 0) fractional_part = -fractional_part;

    return to_string_dec_int(static_cast<int64_t>(integer_part)) + "." +
           to_string_dec_uint(static_cast<uint64_t>(fractional_part), precision, '0');
}

std::string to_string_freq(const uint64_t f) {
    if (f < 1000000)
        return to_string_dec_int(static_cast<int64_t>(f), 10, ' ');
    return to_string_dec_int(static_cast<int64_t>(f / 1000000), 4) +
           to_string_dec_int(static_cast<int64_t>(f % 1000000), 6, '0');
}

std::string to_string_short_freq(const uint64_t f) {
    return to_string_dec_int(static_cast<int64_t>((f + 50) / 1000000), 4) + "." +
           to_string_dec_int(static_cast<int64_t>(((f + 50) / 100) % 10000), 4, '0');
}

std::string to_string_rounded_freq(const uint64_t f, int8_t precision) {
    static constexpr uint32_t pow10[7] = {1, 10, 100, 1000, 10000, 100000, 1000000};

    if (precision < 1) return to_string_dec_uint(f / 1000000);
    if (precision > 6) precision = 6;

    const uint32_t divisor = pow10[6 - precision];
    const uint64_t rounded = f + (divisor / 2);
    return to_string_dec_uint(rounded / 1000000) + "." +
           to_string_dec_int(static_cast<int64_t>((rounded / divisor) % pow10[precision]),
                             precision, '0');
}

std::string to_string_time_ms(const uint32_t ms) {
    if (ms < 1000) return to_string_dec_uint(ms) + "ms";

    const uint32_t seconds = ms / 1000;
    std::string s;
    if (seconds >= 60) s = to_string_dec_uint(seconds / 60) + "m";
    return s + to_string_dec_uint(seconds % 60) + "s";
}

std::string to_string_file_size(uint64_t file_size) {
    static const char* suffix[5] = {"B", "kB", "MB", "GB", "TB"};
    size_t suffix_index = 0;

    while (file_size >= 1024 && suffix_index < 4) {
        file_size /= 1024;
        suffix_index++;
    }

    return to_string_dec_uint(file_size) + suffix[suffix_index];
}

std::string unit_auto_scale(double n, const uint32_t base_unit, uint32_t precision) {
    static const uint32_t powers_of_ten[5] = {1, 10, 100, 1000, 10000};

    uint32_t prefix_index = base_unit;
    if (prefix_index > 6) prefix_index = 6;
    precision = std::min<uint32_t>(4, precision);

    while (n > 1000 && prefix_index < 6) {
        n /= 1000.0;
        prefix_index++;
    }

    double integer_part = 0.0;
    double fractional_part = std::modf(n, &integer_part) * powers_of_ten[precision];
    if (fractional_part < 0) fractional_part = -fractional_part;

    std::string s = to_string_dec_int(static_cast<int64_t>(integer_part));
    if (precision)
        s += '.' + to_string_dec_uint(static_cast<uint64_t>(fractional_part), precision, '0');

    if (unit_prefix[prefix_index] != 0) s += unit_prefix[prefix_index];

    return s;
}

namespace {

std::tm local_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm;
}

}  // namespace

std::string to_string_datetime_now() {
    const std::tm tm = local_now();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string to_string_timestamp_now() {
    const std::tm tm = local_now();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

std::string trim(std::string_view str) {
    const auto first = str.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = str.find_last_not_of(" \t\r\n");
    return std::string(str.substr(first, last - first + 1));
}

std::string trimr(std::string_view str) {
    const auto last = str.find_last_not_of(" \t\r\n");
    if (last == std::string_view::npos) return {};
    return std::string(str.substr(0, last + 1));
}

std::string truncate(std::string_view str, size_t length) {
    if (str.size() <= length) return std::string(str);
    return std::string(str.substr(0, length));
}
