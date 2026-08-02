/*
 * mayhem-b200 — the frequency manager database.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2023 gullradriel, Nilorea Studio Inc.
 * Copyright (C) 2023 Kyle Reed
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "freqman_db.hpp"

#include "file_path.hpp"
#include "string_format.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <utility>

namespace core {

const char* const freqman_extension = ".TXT";

namespace {

using option_t = std::pair<std::string_view, int32_t>;
using options_t = std::vector<option_t>;

/* --- Option tables, verbatim from firmware/application/freqman_db.cpp ---
 * The index of an entry in these tables is what gets stored in a
 * freqman_entry; the second member is the value the radio wants. */

const options_t freqman_modulations = {
    {"AM", 0},
    {"NFM", 1},
    {"WFM", 2},
    {"SPEC", 3},
    {"AMFM", 4},  /* HF Wefax: AM and FM demod inside the audio app.  */
    {"FMAM", 5},  /* NOAA 137 MHz satellites: FM and AM demod.        */
};

const options_t freqman_bandwidths[freqman_modulation_count] = {
    {
        /* AM */
        {"DSB 9k", 0},
        {"DSB 6k", 1},
        {"USB+3k", 2},
        {"LSB-3k", 3},
        {"CW", 4},
    },
    {
        /* NFM */
        {"8k5", 0},
        {"11k", 1},
        {"12k5", 2},
        {"16k", 3},
    },
    {
        /* WFM — NB: the table is not in value order, and upstream's file
         * format stores the table position, not the value. */
        {"80k", 2},
        {"180k", 1},
        {"200k", 0},
    },
    {
        /* SPEC — these are bandwidths in Hz rather than config indices. */
        {"12k5", 12500},
        {"16k", 16000},
        {"25k", 25000},
        {"32k", 32000},
        {"50k", 50000},
        {"75k", 75000},
        {"100k", 100000},
        {"150k", 150000},
        {"250k", 250000},
        {"500k", 500000},
        {"600k", 600000},
        {"750k", 750000},
        {"1000k", 1000000},
        {"1250k", 1250000},
        {"1500k", 1500000},
        {"1750k", 1750000},
        {"2000k", 2000000},
        {"2250k", 2250000},
        {"2500k", 2500000},
        {"3000k", 3000000},
        {"3500k", 3500000},
        {"4000k", 4000000},
        {"4500k", 4500000},
        {"5000k", 5500000},  /* upstream types 5500000 here; kept verbatim */
        {"5500k", 5500000},
    },
    {
        /* AMFM, for Wefax */
        {"USB+FM(Wefax Apt)", 5},
    },
    {
        /* WFMAM, for the NOAA 137 MHz band */
        {"80k-NOAA Apt LPF", 0},
        {"38k-NOAA Apt LPF", 1},
        {"38k-NOAA Apt BPF", 2},
    },
};

const options_t freqman_steps = {
    {"10Hz        ", 10},
    {"50Hz        ", 50},
    {"0.1kHz      ", 100},
    {"1kHz        ", 1000},
    {"5kHz (SA AM)", 5000},
    {"6.25kHz(NFM)", 6250},
    {"8.33kHz(AIR)", 8333},
    {"9kHz (EU AM)", 9000},
    {"10kHz(US AM)", 10000},
    {"12.5kHz(NFM)", 12500},
    {"15kHz  (HFM)", 15000},
    {"25kHz   (N1)", 25000},
    {"30kHz (OIRT)", 30000},
    {"50kHz  (FM1)", 50000},
    {"100kHz (FM2)", 100000},
    {"250kHz  (N2)", 250000},
    {"500kHz (WFM)", 500000},
    {"750kHz      ", 750000},
    {"1MHz        ", 1000000},
};

/* The names written to and read from the file's `s=` field. */
const options_t freqman_steps_short = {
    {"10Hz", 10},
    {"50Hz", 50},
    {"0.1kHz", 100},
    {"1kHz", 1000},
    {"5kHz", 5000},
    {"6.25kHz", 6250},
    {"8.33kHz", 8333},
    {"9kHz", 9000},
    {"10kHz", 10000},
    {"12.5kHz", 12500},
    {"15kHz", 15000},
    {"25kHz", 25000},
    {"30kHz", 30000},
    {"50kHz", 50000},
    {"100kHz", 100000},
    {"250kHz", 250000},
    {"500kHz", 500000},
    {"750kHz", 750000},
    {"1MHz", 1000000},
};

/* --- CTCSS tones, from firmware/application/tone_key.cpp --- */

constexpr uint32_t f2i100(double hz) {
    /* +0.5 to round rather than truncate, as the firmware's F2Ix100 does. */
    return static_cast<uint32_t>(hz * 100.0 + 0.5);
}

constexpr uint32_t tone_freq_tolerance_centihz = 4 * 100;

using tone_t = std::pair<std::string_view, uint32_t>;

/* Ascending by frequency — the nearest-match search relies on that. */
const std::vector<tone_t> tone_keys = {
    {"None", f2i100(0.0)},
    {"1 XZ", f2i100(67.0)},
    {"39 WZ", f2i100(69.3)},
    {"2 XA", f2i100(71.9)},
    {"3 WA", f2i100(74.4)},
    {"4 XB", f2i100(77.0)},
    {"5 WB", f2i100(79.7)},
    {"6 YZ", f2i100(82.5)},
    {"7 YA", f2i100(85.4)},
    {"8 YB", f2i100(88.5)},
    {"9 ZZ", f2i100(91.5)},
    {"10 ZA", f2i100(94.8)},
    {"11 ZB", f2i100(97.4)},
    {"12 1Z", f2i100(100.0)},
    {"13 1A", f2i100(103.5)},
    {"14 1B", f2i100(107.2)},
    {"15 2Z", f2i100(110.9)},
    {"16 2A", f2i100(114.8)},
    {"17 2B", f2i100(118.8)},
    {"18 3Z", f2i100(123.0)},
    {"19 3A", f2i100(127.3)},
    {"20 3B", f2i100(131.8)},
    {"21 4Z", f2i100(136.5)},
    {"22 4A", f2i100(141.3)},
    {"23 4B", f2i100(146.2)},
    {"24 5Z", f2i100(151.4)},
    {"25 5A", f2i100(156.7)},
    {"40 --", f2i100(159.8)},
    {"26 5B", f2i100(162.2)},
    {"41 --", f2i100(165.5)},
    {"27 6Z", f2i100(167.9)},
    {"42 --", f2i100(171.3)},
    {"28 6A", f2i100(173.8)},
    {"43 --", f2i100(177.3)},
    {"29 6B", f2i100(179.9)},
    {"44 --", f2i100(183.5)},
    {"30 7Z", f2i100(186.2)},
    {"45 --", f2i100(189.9)},
    {"31 7A", f2i100(192.8)},
    {"46 --", f2i100(196.6)},
    {"47 --", f2i100(199.5)},
    {"32 M1", f2i100(203.5)},
    {"48 8Z", f2i100(206.5)},
    {"33 M2", f2i100(210.7)},
    {"34 M3", f2i100(218.1)},
    {"35 M4", f2i100(225.7)},
    {"49 9Z", f2i100(229.1)},
    {"36 M5", f2i100(233.6)},
    {"37 M6", f2i100(241.8)},
    {"38 M7", f2i100(250.3)},
    {"50 0Z", f2i100(254.1)},
    {"Shure 19kHz", f2i100(19000.0)},
    {"Axient 28kHz", f2i100(28000.0)},
    {"Senn. 32.000k", f2i100(32000.0)},
    {"Sony 32.382k", f2i100(32382.0)},
    {"Senn. 32.768k", f2i100(32768.0)},
};

/* Nearest tone within the tolerance, or -1. The firmware walks the ascending
 * table and stops as soon as the difference starts growing. */
int32_t tone_key_index_by_value(int64_t value) {
    int64_t min_diff = INT64_MAX;
    int32_t min_idx = -1;

    for (size_t idx = 0; idx < tone_keys.size(); ++idx) {
        const int64_t diff =
            std::abs(value - static_cast<int64_t>(tone_keys[idx].second));
        if (diff < min_diff) {
            min_idx = static_cast<int32_t>(idx);
            min_diff = diff;
        } else {
            break;
        }
    }

    return (min_diff < tone_freq_tolerance_centihz) ? min_idx : -1;
}

/* "8850" -> "88.5", as the firmware's fx100_string. */
std::string fx100_string(uint32_t f) {
    return to_string_dec_uint((f + 5) / 100) + "." +
           to_string_dec_uint(((f + 5) / 10) % 10);
}

/* --- Small helpers, matching the firmware's --- */

/* Same semantics as firmware/application/file_reader.cpp's split_string,
 * including the empty trailing field of "a=" and "f=1,". */
std::vector<std::string_view> split_string(std::string_view str, char c) {
    std::vector<std::string_view> cols;
    size_t start = 0;

    while (start < str.length()) {
        const auto it = str.find(c, start);
        if (it == std::string_view::npos) break;

        cols.push_back(str.substr(start, it - start));
        start = it + 1;
    }

    if (start <= str.length() && !str.empty()) cols.push_back(str.substr(start));

    return cols;
}

/* The firmware's parse_int: always writes the output, ignores trailing
 * garbage, and yields 0 for text that is not a number at all. */
template <typename T>
void parse_int_lenient(std::string_view str, T& out) {
    out = T{};
    if (str.empty() || str.size() > 64) return;

    const std::string s{str};
    errno = 0;
    out = static_cast<T>(std::strtoll(s.c_str(), nullptr, 10));
}

freqman_index_t find_by_name(const options_t& options, std::string_view name) {
    for (size_t ix = 0; ix < options.size(); ++ix) {
        if (options[ix].first == name) return static_cast<freqman_index_t>(ix);
    }
    return freqman_invalid_index;
}

const option_t* find_by_index(const options_t& options, freqman_index_t index) {
    if (static_cast<size_t>(index) < options.size()) return &options[index];
    return nullptr;
}

bool read_whole_file(const std::string& path, std::string& out) {
    std::ifstream f{path, std::ios::binary};
    if (!f) return false;

    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

/* Splits file text into lines, dropping the CR of a CRLF pair and the empty
 * segment a trailing newline leaves behind. Matches how the firmware's
 * FileWrapper counts lines: a final line without a newline still counts. */
std::vector<std::string> split_file_lines(std::string_view text) {
    std::vector<std::string> lines;
    size_t start = 0;

    while (start < text.size()) {
        const auto nl = text.find('\n', start);
        auto line = (nl == std::string_view::npos) ? text.substr(start)
                                                   : text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        lines.emplace_back(line);

        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }

    return lines;
}

bool write_lines(const std::string& path, const std::vector<std::string>& lines) {
    const auto parent = std::filesystem::path{path}.parent_path();
    if (!parent.empty() && !ensure_directory(parent.string())) return false;

    std::ofstream f{path, std::ios::binary | std::ios::trunc};
    if (!f) return false;

    for (const auto& line : lines) {
        f << line << "\r\n";
    }

    return f.good();
}

}  // namespace

/* --------------------------------------------------------------------- */

size_t freqman_bandwidth_count(freqman_index_t modulation) {
    if (static_cast<size_t>(modulation) >= freqman_modulation_count) return 0;
    return freqman_bandwidths[modulation].size();
}

size_t freqman_step_count() {
    return freqman_steps_short.size();
}

bool operator==(const freqman_entry& lhs, const freqman_entry& rhs) {
    auto equal = lhs.type == rhs.type &&
                 lhs.frequency_a == rhs.frequency_a &&
                 lhs.description == rhs.description &&
                 lhs.modulation == rhs.modulation &&
                 lhs.bandwidth == rhs.bandwidth;

    if (!equal) return false;

    if (lhs.type == freqman_type::Range) {
        equal = lhs.frequency_b == rhs.frequency_b && lhs.step == rhs.step;
    } else if (lhs.type == freqman_type::HamRadio) {
        equal = lhs.frequency_b == rhs.frequency_b && lhs.tone == rhs.tone;
    } else if (lhs.type == freqman_type::Repeater) {
        equal = lhs.frequency_b == rhs.frequency_b;
    }

    return equal;
}

std::string freqman_entry_get_modulation_string(freqman_index_t modulation) {
    if (const auto* opt = find_by_index(freqman_modulations, modulation))
        return std::string{opt->first};
    return {};
}

std::string freqman_entry_get_bandwidth_string(freqman_index_t modulation,
                                               freqman_index_t bandwidth) {
    if (static_cast<size_t>(modulation) < freqman_modulations.size()) {
        if (const auto* opt = find_by_index(freqman_bandwidths[modulation], bandwidth))
            return std::string{opt->first};
    }
    return {};
}

std::string freqman_entry_get_step_string(freqman_index_t step) {
    if (const auto* opt = find_by_index(freqman_steps, step))
        return std::string{opt->first};
    return {};
}

std::string freqman_entry_get_step_string_short(freqman_index_t step) {
    if (const auto* opt = find_by_index(freqman_steps_short, step))
        return std::string{opt->first};
    return {};
}

freqman_index_t freqman_find_modulation(std::string_view name) {
    return find_by_name(freqman_modulations, name);
}

freqman_index_t freqman_find_bandwidth(freqman_index_t modulation,
                                       std::string_view name) {
    if (static_cast<size_t>(modulation) >= freqman_modulation_count)
        return freqman_invalid_index;
    return find_by_name(freqman_bandwidths[modulation], name);
}

freqman_index_t freqman_find_step(std::string_view short_name) {
    return find_by_name(freqman_steps_short, short_name);
}

int32_t freqman_entry_get_step_value(freqman_index_t step) {
    if (const auto* opt = find_by_index(freqman_steps, step)) return opt->second;
    return -1;
}

int32_t freqman_entry_get_bandwidth_value(freqman_index_t modulation,
                                          freqman_index_t bandwidth) {
    if (static_cast<size_t>(modulation) < freqman_modulation_count) {
        if (const auto* opt = find_by_index(freqman_bandwidths[modulation], bandwidth))
            return opt->second;
    }
    return -1;
}

/* Tones ---------------------------------------------------------------- */

freqman_index_t freqman_parse_tone_key(std::string_view value) {
    /* Tones are written as a frequency in Hz with at most one decimal, and
     * stored as hundredths of a Hz. "88.5" -> 8850. The firmware drops any
     * digit past the first decimal place; so do we. */
    const auto parts = split_string(value, '.');

    int32_t whole_part = 0;
    if (!parts.empty()) parse_int_lenient(parts[0], whole_part);

    int64_t tone_freq = static_cast<int64_t>(whole_part) * 100;

    if (parts.size() > 1 && !parts[1].empty()) {
        const auto c = parts[1].front();
        const int digit = std::isdigit(static_cast<unsigned char>(c)) ? (c - '0') : 0;
        tone_freq += digit * 10;
    }

    return static_cast<freqman_index_t>(tone_key_index_by_value(tone_freq));
}

std::string freqman_tone_key_value_string(freqman_index_t index) {
    if (static_cast<size_t>(index) >= tone_keys.size()) return {};
    return fx100_string(tone_keys[index].second);
}

std::string freqman_tone_key_name(freqman_index_t index) {
    if (static_cast<size_t>(index) >= tone_keys.size()) return {};
    return std::string{tone_keys[index].first};
}

size_t freqman_tone_key_count() {
    return tone_keys.size();
}

/* Paths ---------------------------------------------------------------- */

std::string get_freqman_path(const std::string& stem) {
    return (std::filesystem::path{freqman_directory()} / (stem + freqman_extension))
        .string();
}

std::vector<std::string> get_freqman_files() {
    std::vector<std::string> stems;

    std::error_code ec;
    const std::filesystem::path dir{freqman_directory()};
    if (!std::filesystem::is_directory(dir, ec)) return stems;

    for (const auto& entry : std::filesystem::directory_iterator{dir, ec}) {
        if (ec) break;
        if (!entry.is_regular_file(ec)) continue;

        auto ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        if (ext != freqman_extension) continue;

        auto stem = entry.path().stem().string();
        /* Skip temp/hidden files, as the firmware's category scan does. */
        if (stem.empty() || stem.front() == '.') continue;

        stems.push_back(std::move(stem));
    }

    std::sort(stems.begin(), stems.end());
    return stems;
}

bool create_freqman_file(const std::string& file_stem) {
    return write_lines(get_freqman_path(file_stem), {});
}

bool load_freqman_file(const std::string& file_stem, freqman_db& db,
                       freqman_load_options options) {
    return parse_freqman_file(get_freqman_path(file_stem), db, options);
}

bool save_freqman_file(const std::string& file_stem, const freqman_db& db) {
    return save_freqman_db(get_freqman_path(file_stem), db);
}

bool delete_freqman_file(const std::string& file_stem) {
    std::error_code ec;
    return std::filesystem::remove(get_freqman_path(file_stem), ec) && !ec;
}

/* Formatting ----------------------------------------------------------- */

std::string pretty_string(const freqman_entry& entry, size_t max_length) {
    std::string str;

    switch (entry.type) {
        case freqman_type::Single:
            str = to_string_short_freq(static_cast<uint64_t>(entry.frequency_a)) +
                  "M: " + entry.description;
            break;
        case freqman_type::Range:
            str = to_string_rounded_freq(static_cast<uint64_t>(entry.frequency_a), 1) +
                  "M-" +
                  to_string_rounded_freq(static_cast<uint64_t>(entry.frequency_b), 1) +
                  "M: " + entry.description;
            break;
        case freqman_type::HamRadio:
            str = "R:" +
                  to_string_rounded_freq(static_cast<uint64_t>(entry.frequency_a), 1) +
                  "M,T:" +
                  to_string_rounded_freq(static_cast<uint64_t>(entry.frequency_b), 1) +
                  "M: " + entry.description;
            break;
        case freqman_type::Repeater:
            str = "L:" +
                  to_string_rounded_freq(static_cast<uint64_t>(entry.frequency_a), 1) +
                  "M,T:" +
                  to_string_rounded_freq(static_cast<uint64_t>(entry.frequency_b), 1) +
                  "M: " + entry.description;
            break;
        case freqman_type::Raw:
            str = entry.description;
            break;
        default:
            str = "UNK:" + entry.description;
            break;
    }

    /* '+' marks a truncated string. */
    if (max_length > 0 && str.size() > max_length)
        return str.substr(0, max_length - 1) + "+";

    return str;
}

std::string to_freqman_string(const freqman_entry& entry) {
    std::string serialized;
    serialized.reserve(0x80);

    auto append_field = [&serialized](std::string_view name, std::string_view value) {
        if (!serialized.empty()) serialized += ",";
        serialized += std::string{name} + "=" + std::string{value};
    };

    switch (entry.type) {
        case freqman_type::Single:
            append_field("f", to_string_dec_uint(static_cast<uint64_t>(entry.frequency_a)));
            break;

        case freqman_type::Range:
            append_field("a", to_string_dec_uint(static_cast<uint64_t>(entry.frequency_a)));
            append_field("b", to_string_dec_uint(static_cast<uint64_t>(entry.frequency_b)));
            if (is_valid(entry.step))
                append_field("s", freqman_entry_get_step_string_short(entry.step));
            break;

        case freqman_type::HamRadio:
            append_field("r", to_string_dec_uint(static_cast<uint64_t>(entry.frequency_a)));
            append_field("t", to_string_dec_uint(static_cast<uint64_t>(entry.frequency_b)));
            if (is_valid(entry.tone))
                append_field("c", freqman_tone_key_value_string(entry.tone));
            break;

        case freqman_type::Repeater:
            append_field("l", to_string_dec_uint(static_cast<uint64_t>(entry.frequency_a)));
            append_field("t", to_string_dec_uint(static_cast<uint64_t>(entry.frequency_b)));
            break;

        case freqman_type::Raw:
            return entry.description;

        default:
            return {};
    }

    if (is_valid(entry.modulation) &&
        static_cast<size_t>(entry.modulation) < freqman_modulations.size()) {
        append_field("m", freqman_entry_get_modulation_string(entry.modulation));

        if (is_valid(entry.bandwidth) &&
            static_cast<size_t>(entry.bandwidth) <
                freqman_bandwidths[entry.modulation].size()) {
            append_field("bw",
                         freqman_entry_get_bandwidth_string(entry.modulation,
                                                            entry.bandwidth));
        }
    }

    if (!entry.description.empty()) append_field("d", entry.description);

    serialized.shrink_to_fit();
    return serialized;
}

/* Parsing -------------------------------------------------------------- */

bool parse_freqman_entry(std::string_view str, freqman_entry& entry) {
    if (str.empty() || str[0] == '#') return false;

    entry = freqman_entry{};
    const auto cols = split_string(str, ',');

    for (const auto col : cols) {
        if (col.empty()) continue;

        const auto pair = split_string(col, '=');
        if (pair.size() != 2) continue;

        const auto key = pair[0];
        const auto value = pair[1];

        if (key == "a") {
            entry.type = freqman_type::Range;
            parse_int_lenient(value, entry.frequency_a);
        } else if (key == "b") {
            parse_int_lenient(value, entry.frequency_b);
        } else if (key == "bw") {
            /* NB: requires the modulation to have been set first — an invalid
             * index is 255, which fails this bound and leaves bw unset. */
            if (static_cast<size_t>(entry.modulation) < freqman_modulation_count)
                entry.bandwidth = find_by_name(freqman_bandwidths[entry.modulation], value);
        } else if (key == "c") {
            entry.tone = freqman_parse_tone_key(value);
        } else if (key == "d") {
            entry.description = trim(value).substr(0, freqman_max_desc_size);
        } else if (key == "f") {
            entry.type = freqman_type::Single;
            parse_int_lenient(value, entry.frequency_a);
        } else if (key == "m") {
            entry.modulation = find_by_name(freqman_modulations, value);
        } else if (key == "r") {
            /* Ham relay receive frequency. */
            entry.type = freqman_type::HamRadio;
            parse_int_lenient(value, entry.frequency_a);
        } else if (key == "l") {
            /* Repeater listen frequency; a plain single frequency when the
             * app is not in repeater mode. */
            entry.type = freqman_type::Repeater;
            parse_int_lenient(value, entry.frequency_a);
        } else if (key == "s") {
            entry.step = find_by_name(freqman_steps_short, value);
        } else if (key == "t") {
            /* TX frequency: scanned as a single frequency in ham mode, used as
             * the TX frequency in repeater mode, ignored by the scanner. */
            parse_int_lenient(value, entry.frequency_b);
        }
    }

    return is_valid(entry);
}

bool is_valid(const freqman_entry& entry) {
    if (entry.type == freqman_type::Unknown) return false;

    /* Frequency A is required by every type. */
    if (entry.frequency_a == 0) return false;

    if (entry.type == freqman_type::Range || entry.type == freqman_type::HamRadio ||
        entry.type == freqman_type::Repeater) {
        if (entry.frequency_b == 0) return false;
    }

    if (entry.type == freqman_type::Range) {
        if (entry.frequency_a > entry.frequency_b) return false;
    }

    return true;
}

bool parse_freqman_file(const std::string& path, freqman_db& db,
                        freqman_load_options options) {
    FreqmanDB file_db;
    file_db.set_read_raw(false);  /* drop malformed lines rather than keep them */
    if (!file_db.open(path)) return false;

    db.clear();
    db.reserve(file_db.entry_count());

    for (auto entry : file_db) {
        if (entry.type == freqman_type::Unknown ||
            (entry.type == freqman_type::Single && !options.load_freqs) ||
            (entry.type == freqman_type::Range && !options.load_ranges) ||
            (entry.type == freqman_type::HamRadio && !options.load_hamradios) ||
            (entry.type == freqman_type::Repeater && !options.load_repeaters)) {
            continue;
        }

        /* Inherit the previous entry's modulation/bandwidth when unset — this
         * is what lets a file set `m=` once and have it apply downwards. */
        if (!db.empty()) {
            if (is_invalid(entry.modulation)) entry.modulation = db.back()->modulation;
            if (is_invalid(entry.bandwidth)) entry.bandwidth = db.back()->bandwidth;
        }

        db.push_back(std::make_unique<freqman_entry>(std::move(entry)));

        if (options.max_entries > 0 && db.size() >= options.max_entries) break;
    }

    db.shrink_to_fit();
    return true;
}

bool save_freqman_db(const std::string& path, const freqman_db& db) {
    std::vector<std::string> lines;
    lines.reserve(db.size());

    for (const auto& entry : db) {
        if (!entry) continue;

        auto text = to_freqman_string(*entry);
        /* An Unknown entry serialises to nothing; writing a blank line would
         * add an entry that reads back as junk. */
        if (text.empty()) continue;

        lines.push_back(std::move(text));
    }

    return write_lines(path, lines);
}

/* FreqmanDB ------------------------------------------------------------ */

bool FreqmanDB::open(const std::string& path, bool create) {
    std::string text;
    if (!read_whole_file(path, text)) {
        if (!create) return false;

        if (!write_lines(path, {})) return false;
        text.clear();
    }

    lines_ = split_file_lines(text);
    path_ = path;
    open_ = true;
    dirty_ = false;
    return true;
}

bool FreqmanDB::open_list(const std::string& stem, bool create) {
    return open(get_freqman_path(stem), create);
}

void FreqmanDB::close() {
    lines_.clear();
    path_.clear();
    open_ = false;
    dirty_ = false;
}

bool FreqmanDB::save() const {
    if (!open_) return false;
    if (!write_lines(path_, lines_)) return false;

    dirty_ = false;
    return true;
}

void FreqmanDB::after_mutation() {
    dirty_ = true;
    if (autosave_) save();
}

std::string FreqmanDB::line(Index index) const {
    if (index >= lines_.size()) return {};
    return lines_[index];
}

freqman_entry FreqmanDB::operator[](Index index) const {
    if (index >= lines_.size()) return {};

    const auto& text = lines_[index];

    freqman_entry entry;
    if (parse_freqman_entry(text, entry)) return entry;

    if (read_raw_) {
        entry = freqman_entry{};
        entry.type = freqman_type::Raw;
        entry.description = trim(text).substr(0, freqman_max_desc_size);
        return entry;
    }

    return {};
}

void FreqmanDB::insert_entry(Index index, const freqman_entry& entry) {
    if (!open_) return;

    const auto count = entry_count();
    if (index > count) index = count;

    lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(index),
                  to_freqman_string(entry));
    after_mutation();
}

void FreqmanDB::append_entry(const freqman_entry& entry) {
    insert_entry(entry_count(), entry);
}

void FreqmanDB::replace_entry(Index index, const freqman_entry& entry) {
    if (!open_ || index >= lines_.size()) return;

    lines_[index] = to_freqman_string(entry);
    after_mutation();
}

void FreqmanDB::delete_entry(Index index) {
    if (!open_ || index >= lines_.size()) return;

    lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(index));
    after_mutation();
}

bool FreqmanDB::delete_entry(const freqman_entry& entry) {
    auto it = find_entry(entry);
    if (it == end()) return false;

    delete_entry(it.index());
    return true;
}

FreqmanDB::iterator FreqmanDB::find_entry(const freqman_entry& entry) {
    return find_entry([&entry](const freqman_entry& other) { return entry == other; });
}

}  // namespace core
