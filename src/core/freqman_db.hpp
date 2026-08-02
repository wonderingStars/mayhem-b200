/*
 * mayhem-b200 — the frequency manager database.
 *
 * Ported from the firmware's application/freqman_db.* and freqman.*. The file
 * format is Mayhem's, byte for byte, so a FREQMAN .TXT written here loads on a
 * PortaPack and vice versa. A line is a comma-separated list of key=value
 * fields:
 *
 *   f=437500000,d=Description                       single frequency
 *   a=430000000,b=440000000,s=25kHz,d=Range         range, with a scan step
 *   r=145600000,t=145000000,c=88.5,d=Ham            ham relay rx/tx + CTCSS
 *   l=145600000,t=145000000,d=Repeater              repeater listen/transmit
 *   f=100000000,m=WFM,bw=200k,d=Broadcast           modulation and bandwidth
 *
 * Recognised keys, exactly as upstream's parser has them:
 *
 *   f   single frequency, Hz          a   range start, Hz
 *   b   range end / tx frequency      r   ham relay receive frequency
 *   l   repeater listen frequency     t   transmit frequency
 *   m   modulation name               bw  bandwidth name (needs m first)
 *   s   step name (short form)        c   CTCSS tone in Hz
 *   d   description, trimmed, 30 chars max
 *
 * Faithful quirks, kept so that files stay interchangeable:
 *   - `bw=` is only honoured when `m=` has already appeared on the line.
 *   - A field whose value fails to parse becomes 0, which then usually fails
 *     validation, so the line is dropped rather than half-read.
 *   - A description containing ',' or '=' cannot be represented; upstream does
 *     not escape it, so neither do we.
 *   - When a loaded entry has no modulation or bandwidth, it inherits the
 *     previous entry's — that is how upstream files avoid repeating `m=` on
 *     every line.
 *
 * Deliberate deviation: upstream's FreqmanDB edits the file in place through
 * FileWrapper, an SD-card-oriented design that silently drops an append to a
 * file with no trailing newline. Here the lines are held in memory and written
 * back whole, so appends always work; set_autosave(false) if you want to batch
 * edits and call save() once.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2023 gullradriel, Nilorea Studio Inc.
 * Copyright (C) 2023 Kyle Reed
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_FREQMAN_DB_H__
#define __MB200_FREQMAN_DB_H__

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace core {

/* ".TXT" — upper case, as the firmware writes it. */
extern const char* const freqman_extension;

using freqman_index_t = uint8_t;
constexpr freqman_index_t freqman_invalid_index = static_cast<freqman_index_t>(-1);

constexpr bool is_valid(freqman_index_t index) {
    return index != freqman_invalid_index;
}

constexpr bool is_invalid(freqman_index_t index) {
    return index == freqman_invalid_index;
}

enum class freqman_type : uint8_t {
    Single,    /* f=            */
    Range,     /* a=, b=        */
    HamRadio,  /* r=, t=        */
    Repeater,  /* l=, t=        */
    Raw,       /* unparsed line, kept in `description` */
    Unknown,
};

/* Modulation indices. These are the values written to and read from the file
 * as `m=`, and they line up with the firmware's freqman_entry_modulation. */
enum freqman_modulation_index : freqman_index_t {
    freqman_modulation_am = 0,
    freqman_modulation_nfm = 1,
    freqman_modulation_wfm = 2,
    freqman_modulation_spec = 3,
    freqman_modulation_amfm = 4,  /* HF Wefax: AM + FM demod  */
    freqman_modulation_wfmam = 5, /* NOAA 137 MHz APT         */
};

/* Number of modulations, and so of per-modulation bandwidth tables. */
constexpr size_t freqman_modulation_count = 6;

struct freqman_entry {
    int64_t frequency_a{0};     /* f= / a= / r= / l= */
    int64_t frequency_b{0};     /* b= / t=           */
    std::string description{};  /* d=                */
    freqman_type type{freqman_type::Unknown};
    freqman_index_t modulation{freqman_invalid_index};
    freqman_index_t bandwidth{freqman_invalid_index};
    freqman_index_t step{freqman_invalid_index};
    freqman_index_t tone{freqman_invalid_index};
};

/* Upstream's comparison: type, frequency_a, description, modulation and
 * bandwidth always; frequency_b and the type-specific extra only for the types
 * that use them. */
bool operator==(const freqman_entry& lhs, const freqman_entry& rhs);
inline bool operator!=(const freqman_entry& lhs, const freqman_entry& rhs) {
    return !(lhs == rhs);
}

/* Name lookups over the option tables. An unknown name yields
 * freqman_invalid_index; an out-of-range index yields an empty string. */
std::string freqman_entry_get_modulation_string(freqman_index_t modulation);
std::string freqman_entry_get_bandwidth_string(freqman_index_t modulation,
                                               freqman_index_t bandwidth);
std::string freqman_entry_get_step_string(freqman_index_t step);
std::string freqman_entry_get_step_string_short(freqman_index_t step);

freqman_index_t freqman_find_modulation(std::string_view name);
freqman_index_t freqman_find_bandwidth(freqman_index_t modulation, std::string_view name);
freqman_index_t freqman_find_step(std::string_view short_name);

/* Value behind an index, or -1 when out of range. Bandwidth values are the
 * firmware's per-modulation configuration indices except for SPEC, where they
 * are bandwidths in Hz — an upstream inconsistency, preserved. */
int32_t freqman_entry_get_step_value(freqman_index_t step);
int32_t freqman_entry_get_bandwidth_value(freqman_index_t modulation,
                                          freqman_index_t bandwidth);

size_t freqman_bandwidth_count(freqman_index_t modulation);
size_t freqman_step_count();

/* CTCSS tones, as used by the `c=` field. Kept private to freqman rather than
 * exported as a general tone_key module so a fuller port of the firmware's
 * tone_key.* can land later without a symbol clash. */
freqman_index_t freqman_parse_tone_key(std::string_view value);
/* "88.5" — the frequency form written back to the file. */
std::string freqman_tone_key_value_string(freqman_index_t index);
/* "8 YB" — the CTCSS code name, for display. */
std::string freqman_tone_key_name(freqman_index_t index);
size_t freqman_tone_key_count();

/* Upstream's cap on entries loaded into RAM. A host has no such constraint,
 * but the default is kept so behaviour matches unless a caller opts out with
 * max_entries = 0. */
constexpr size_t freqman_default_max_entries = 150;

/* The format specifies 30 characters of description. */
constexpr size_t freqman_max_desc_size = 30;

struct freqman_load_options {
    /* 0 loads every entry. */
    size_t max_entries{freqman_default_max_entries};
    bool load_freqs{true};
    bool load_ranges{true};
    bool load_hamradios{true};
    bool load_repeaters{true};
};

using freqman_entry_ptr = std::unique_ptr<freqman_entry>;
using freqman_db = std::vector<freqman_entry_ptr>;

/* <freqman dir>/<stem>.TXT */
std::string get_freqman_path(const std::string& stem);

/* Stems of every *.TXT in core::freqman_directory(), sorted, skipping names
 * that begin with '.' — the same filter the firmware's category list uses. */
std::vector<std::string> get_freqman_files();

/* Creates an empty list. Truncates an existing one, as the firmware's
 * make_new_file does — check get_freqman_files() first if that matters. */
bool create_freqman_file(const std::string& file_stem);
bool load_freqman_file(const std::string& file_stem, freqman_db& db,
                       freqman_load_options options = {});
bool save_freqman_file(const std::string& file_stem, const freqman_db& db);
bool delete_freqman_file(const std::string& file_stem);

/* Display form: "437.5000M: Description", "R:145.6M,T:145.0M: Ham", etc.
 * Truncated to max_length with a trailing '+' when it does not fit. */
std::string pretty_string(const freqman_entry& item, size_t max_length = 30);

/* File form. Empty for a freqman_type::Unknown entry. */
std::string to_freqman_string(const freqman_entry& entry);

/* Parses one line. Returns false — leaving `entry` holding whatever it managed
 * to read — for a comment, a blank line, or anything that fails is_valid(). */
bool parse_freqman_entry(std::string_view str, freqman_entry& entry);

bool parse_freqman_file(const std::string& path, freqman_db& db,
                        freqman_load_options options = {});
bool save_freqman_db(const std::string& path, const freqman_db& db);

/* Well-formedness: a known type, a non-zero frequency_a, a non-zero
 * frequency_b for the two-frequency types, and a <= b for ranges. */
bool is_valid(const freqman_entry& entry);

/* CRUD over a freqman file. Lines are held in memory; every mutation rewrites
 * the file unless autosave is off. */
class FreqmanDB {
   public:
    using Index = uint32_t;
    static constexpr Index end_index = static_cast<Index>(-1);

    /* Forward, read-only, exactly as much iterator as upstream's. */
    class iterator {
       public:
        iterator(FreqmanDB& db, Index index)
            : db_{&db}, index_{index} {}

        iterator& operator++() {
            index_++;
            if (index_ >= db_->entry_count()) index_ = end_index;
            return *this;
        }

        freqman_entry operator*() const { return (*db_)[index_]; }

        bool operator==(const iterator& other) const {
            return db_ == other.db_ && index_ == other.index_;
        }
        bool operator!=(const iterator& other) const { return !(*this == other); }

        Index index() const { return index_; }

       private:
        FreqmanDB* db_;
        Index index_;
    };

    /* Opens a file by full path. With create = true a missing file is created
     * empty; without it, a missing file is a failure. */
    bool open(const std::string& path, bool create = false);
    /* Same, for a list name in core::freqman_directory(). */
    bool open_list(const std::string& stem, bool create = false);
    void close();

    bool is_open() const { return open_; }
    const std::string& path() const { return path_; }

    /* Writes the file. Returns false if it could not be written; the in-memory
     * lines are unaffected either way. */
    bool save() const;

    /* True when a mutation has not yet reached disk (only possible with
     * autosave off, or after a failed write). */
    bool dirty() const { return dirty_; }

    void set_autosave(bool v) { autosave_ = v; }
    bool autosave() const { return autosave_; }

    /* An out-of-range index yields a default (Unknown) entry. */
    freqman_entry operator[](Index index) const;

    void insert_entry(Index index, const freqman_entry& entry);
    void append_entry(const freqman_entry& entry);
    void replace_entry(Index index, const freqman_entry& entry);
    void delete_entry(Index index);
    bool delete_entry(const freqman_entry& entry);

    template <typename Fn>
    iterator find_entry(const Fn& predicate) {
        auto it = begin();
        const auto it_end = end();

        while (it != it_end) {
            if (predicate(*it)) return it;
            ++it;
        }

        return it_end;
    }
    iterator find_entry(const freqman_entry& entry);

    uint32_t entry_count() const { return static_cast<uint32_t>(lines_.size()); }
    bool empty() const { return lines_.empty(); }

    /* When true (the default), a line that does not parse comes back as a Raw
     * entry holding the line text, so an editor can show and keep it. Loading
     * into a freqman_db turns it off, so malformed lines are dropped. */
    void set_read_raw(bool v) { read_raw_ = v; }
    bool read_raw() const { return read_raw_; }

    /* Verbatim line text. Out of range gives an empty string. */
    std::string line(Index index) const;

    iterator begin() { return {*this, empty() ? end_index : 0u}; }
    iterator end() { return {*this, end_index}; }

   private:
    std::vector<std::string> lines_{};
    std::string path_{};
    bool open_{false};
    bool read_raw_{true};
    bool autosave_{true};
    mutable bool dirty_{false};

    void after_mutation();
};

}  // namespace core

#endif /*__MB200_FREQMAN_DB_H__*/
