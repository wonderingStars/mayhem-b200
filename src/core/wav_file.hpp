/*
 * mayhem-b200 — RIFF/WAVE PCM reading and writing.
 *
 * Ported from firmware/application/io_wave.{hpp,cpp} (WAVFileReader /
 * WAVFileWriter). Files written here are byte-compatible with what Mayhem's
 * apps expect: a 44-byte canonical header followed by the PCM data, followed by
 * the PortaPack LIST/INFO metadata chunk (IART = "PortaPack", INAM = title).
 * That chunk is Mayhem's WAV metadata convention — there is no sidecar file for
 * WAV. (The `.TXT` sidecar written by `write_metadata_file` is only used for raw
 * C8/C16 captures, see firmware/application/ui_record_view.cpp.)
 *
 * Two deliberate corrections to upstream, both documented at the point of use in
 * wav_file.cpp:
 *
 *  - upstream's writer patches `data.cksize` with `bytes_written_ - 44` *after*
 *    appending the 104-byte tags chunk, so every file it produces overstates the
 *    data chunk by 104 bytes and its own reader then never finds the INAM title.
 *    This writer patches the real data length.
 *  - upstream's reader assumes a fixed chunk layout; this one walks the chunk
 *    list, so it also reads WAVs written by anything else.
 *
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_WAV_FILE_H__
#define __MB200_WAV_FILE_H__

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>

namespace core {

/* Canonical RIFF/WAVE header: 'RIFF' size 'WAVE' + 'fmt ' (16) + 'data'. */
inline constexpr uint32_t wav_header_size = 44;

/* Size of the LIST/INFO chunk the writer appends, matching upstream `tags_t`:
 * 'LIST' size 'INFO' 'IART' 12 <artist> 'INAM' 64 <title>. */
inline constexpr uint32_t wav_info_chunk_size = 104;

/* Field widths inside that chunk, as upstream declares them. */
inline constexpr size_t wav_artist_field_size = 12;
inline constexpr size_t wav_title_field_size = 64;

/* What upstream stamps into IART for every file it writes. */
inline constexpr const char* wav_artist = "PortaPack";

/* Streaming reader for RIFF/WAVE PCM: 8/16/24/32 bit, any channel count, any
 * sample rate. Reads a block at a time; nothing is buffered in memory. */
class WavFileReader {
   public:
    WavFileReader() = default;
    ~WavFileReader();

    WavFileReader(const WavFileReader&) = delete;
    WavFileReader& operator=(const WavFileReader&) = delete;
    WavFileReader(WavFileReader&&) = delete;
    WavFileReader& operator=(WavFileReader&&) = delete;

    /* Opens and parses the header. False on a missing file, a non-RIFF/WAVE
     * file, a truncated header, a missing 'fmt ' or 'data' chunk, or a format
     * this reader does not decode. Nothing is left open on failure. */
    bool open(const std::string& path);
    void close();
    bool is_open() const { return open_; }

    uint16_t channels() const { return channels_; }
    uint32_t sample_rate() const { return sample_rate_; }
    uint16_t bits_per_sample() const { return bits_per_sample_; }

    /* Bytes for one sample of one channel, i.e. bits_per_sample / 8. Named as
     * upstream names it; a whole interleaved frame is this times channels(). */
    uint32_t bytes_per_sample() const { return bytes_per_sample_; }

    /* Length of the data chunk in bytes, clamped to what the file actually
     * holds (upstream files overstate it by the size of their tags chunk). */
    uint32_t data_size() const { return data_size_; }

    /* Single-channel samples, as upstream's sample_count(): for stereo this
     * counts left and right separately. Divide by channels() for frames. */
    uint32_t sample_count() const;

    /* Interleaved frames: sample_count() / channels(). */
    uint32_t frame_count() const;

    /* Playing time in milliseconds. Identical to upstream's
     * `ms_duration(data_size, sample_rate, bytes_per_sample)` for mono — which
     * is all upstream's callers ever open — but also divides by the channel
     * count, so stereo does not report double. */
    uint32_t ms_duration() const;

    /* INAM value from the LIST/INFO chunk, empty when the file has none. */
    const std::string& title() const { return title_; }

    /* Seeks back to the first sample. */
    void rewind();

    /* Seeks to a single-channel sample index, as upstream's data_seek(). False
     * if the offset is past the end of the data chunk. */
    bool data_seek(uint64_t sample_offset);

    /* Current read position as a single-channel sample index. */
    uint64_t tell_sample() const;

    /* Reads up to `bytes`, never crossing the end of the data chunk. Returns
     * the number of bytes actually read; 0 at end of data. */
    size_t read(void* buffer, size_t bytes);

    /* Reads up to `count` single-channel samples as signed 16-bit, promoting
     * 8-bit unsigned PCM the way the WAV viewer does: (v - 0x80) * 256.
     * Returns the number of samples produced. Only 8- and 16-bit input. */
    size_t read_samples(int16_t* out, size_t count);

   private:
    bool read_exact(void* buffer, size_t bytes);
    void scan_info_list(uint64_t body_offset, uint32_t body_size);

    std::ifstream stream_{};
    bool open_{false};

    uint64_t file_size_{0};
    uint64_t data_start_{0};
    uint64_t position_{0}; /* absolute file offset of the read cursor */

    uint16_t format_tag_{0};
    uint16_t channels_{0};
    uint32_t sample_rate_{0};
    uint16_t bits_per_sample_{0};
    uint32_t bytes_per_sample_{0};
    uint32_t data_size_{0};
    std::string title_{};
};

/* Streaming writer. create() lays down a placeholder header, write() appends
 * PCM, and close() appends the LIST/INFO chunk and patches the RIFF and data
 * chunk sizes. The destructor closes, so a writer that goes out of scope still
 * leaves a valid file. */
class WavFileWriter {
   public:
    WavFileWriter() = default;
    ~WavFileWriter();

    WavFileWriter(const WavFileWriter&) = delete;
    WavFileWriter& operator=(const WavFileWriter&) = delete;
    WavFileWriter(WavFileWriter&&) = delete;
    WavFileWriter& operator=(WavFileWriter&&) = delete;

    /* Creates (truncating) `path`. `title` goes into the INAM tag and is
     * truncated to wav_title_field_size - 1 characters, as upstream does.
     * Defaults are upstream's fixed format: mono, 16-bit. False if the file
     * cannot be created or the format is not 8- or 16-bit PCM. */
    bool create(const std::string& path,
                uint32_t sample_rate,
                const std::string& title,
                uint16_t channels = 1,
                uint16_t bits_per_sample = 16);

    /* Appends raw PCM bytes exactly as given (little-endian for 16-bit). */
    bool write(const void* data, size_t bytes);

    /* Appends `count` single-channel samples. For a 16-bit file they are
     * written little-endian unchanged; for an 8-bit file they are converted to
     * unsigned with the inverse of the reader's promotion, (s / 256) + 0x80,
     * saturating. */
    bool write_samples(const int16_t* samples, size_t count);

    /* Appends the tags chunk, patches the sizes and closes. Idempotent; returns
     * false if any of that I/O failed. */
    bool close();

    bool is_open() const { return open_; }
    uint64_t data_bytes_written() const { return data_bytes_; }
    uint32_t sample_count() const;
    uint32_t ms_duration() const;

   private:
    bool write_header(uint32_t data_size, uint32_t info_size, uint32_t pad);
    bool write_tags();

    std::ofstream stream_{};
    bool open_{false};
    bool failed_{false};

    uint32_t sample_rate_{0};
    uint16_t channels_{0};
    uint16_t bits_per_sample_{0};
    uint32_t bytes_per_sample_{0};
    uint64_t data_bytes_{0};
    std::string title_{};
};

}  // namespace core

#endif /*__MB200_WAV_FILE_H__*/
