/*
 * mayhem-b200 — RIFF/WAVE PCM reading and writing.
 *
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "wav_file.hpp"

#include <algorithm>
#include <cstring>

namespace core {

namespace {

/* WAVE is little-endian regardless of host, so the fields are (de)serialised a
 * byte at a time rather than by casting a packed struct over the buffer. */

uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t rd_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

void wr_u16(uint8_t* p, uint16_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
}

void wr_u32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v & 0xff);
    p[1] = static_cast<uint8_t>((v >> 8) & 0xff);
    p[2] = static_cast<uint8_t>((v >> 16) & 0xff);
    p[3] = static_cast<uint8_t>((v >> 24) & 0xff);
}

void wr_tag(uint8_t* p, const char* tag) {
    std::memcpy(p, tag, 4);
}

bool tag_is(const uint8_t* p, const char* tag) {
    return std::memcmp(p, tag, 4) == 0;
}

/* Chunk IDs are four printable ASCII characters. Used to tell a real chunk
 * header from the byte a writer that ignored RIFF's even-alignment rule left
 * the cursor pointing at. */
bool is_chunk_id(const uint8_t* p) {
    for (int i = 0; i < 4; ++i)
        if (p[i] < 0x20 || p[i] > 0x7e) return false;
    return true;
}

constexpr uint16_t format_pcm = 0x0001;
constexpr uint16_t format_extensible = 0xfffe;

/* Cap on how much of an INAM value is kept, so a hostile file cannot make the
 * reader allocate without bound. Upstream's buffer is 32 bytes. */
constexpr uint32_t title_read_limit = 256;

}  // namespace

/* -------------------------------------------------------------------------
 * Reader
 * ---------------------------------------------------------------------- */

WavFileReader::~WavFileReader() {
    close();
}

void WavFileReader::close() {
    if (stream_.is_open()) stream_.close();
    stream_.clear();
    open_ = false;
    file_size_ = 0;
    data_start_ = 0;
    position_ = 0;
    format_tag_ = 0;
    channels_ = 0;
    sample_rate_ = 0;
    bits_per_sample_ = 0;
    bytes_per_sample_ = 0;
    data_size_ = 0;
    title_.clear();
}

bool WavFileReader::read_exact(void* buffer, size_t bytes) {
    stream_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(bytes));
    return static_cast<size_t>(stream_.gcount()) == bytes;
}

void WavFileReader::scan_info_list(uint64_t body_offset, uint32_t body_size) {
    /* body points at the LIST payload: a 4-byte form type then sub-chunks. */
    if (body_size < 4) return;

    uint8_t form[4];
    stream_.seekg(static_cast<std::streamoff>(body_offset), std::ios::beg);
    if (!read_exact(form, 4)) return;
    if (!tag_is(form, "INFO")) return;

    uint64_t pos = body_offset + 4;
    const uint64_t end = body_offset + body_size;

    while (pos + 8 <= end) {
        uint8_t hdr[8];
        stream_.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
        if (!read_exact(hdr, 8)) return;

        const uint32_t size = rd_u32(hdr + 4);
        const uint64_t payload = pos + 8;
        if (payload + size > end) return;

        if (tag_is(hdr, "INAM")) {
            const uint32_t take = std::min<uint32_t>(size, title_read_limit);
            std::string buf(take, '\0');
            if (take > 0 && !read_exact(buf.data(), take)) return;
            /* The field is NUL padded; keep only up to the first NUL. */
            const auto nul = buf.find('\0');
            if (nul != std::string::npos) buf.resize(nul);
            title_ = buf;
            return;
        }

        pos = payload + size + (size & 1u); /* RIFF chunks pad to even */
    }
}

bool WavFileReader::open(const std::string& path) {
    close();

    stream_.open(path, std::ios::in | std::ios::binary);
    if (!stream_.is_open()) return false;

    stream_.seekg(0, std::ios::end);
    const std::streamoff end = stream_.tellg();
    if (end < 12) { /* not even room for 'RIFF' size 'WAVE' */
        close();
        return false;
    }
    file_size_ = static_cast<uint64_t>(end);
    stream_.seekg(0, std::ios::beg);

    uint8_t riff[12];
    if (!read_exact(riff, 12) || !tag_is(riff, "RIFF") || !tag_is(riff + 8, "WAVE")) {
        close();
        return false;
    }

    /* Upstream assumes 'fmt ' then 'data' at fixed offsets and skips at most one
     * unexpected chunk. Walking the list instead costs nothing and reads the
     * same files plus everything else. */
    bool have_fmt = false;
    bool have_data = false;
    uint64_t pos = 12;

    while (pos + 8 <= file_size_) {
        uint8_t hdr[8];
        stream_.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
        if (!read_exact(hdr, 8)) break;

        const uint32_t size = rd_u32(hdr + 4);
        const uint64_t body = pos + 8;

        if (tag_is(hdr, "fmt ") && size >= 16 && body + 16 <= file_size_) {
            uint8_t f[16];
            if (!read_exact(f, 16)) break;
            format_tag_ = rd_u16(f + 0);
            channels_ = rd_u16(f + 2);
            sample_rate_ = rd_u32(f + 4);
            bits_per_sample_ = rd_u16(f + 14);
            have_fmt = true;
        } else if (tag_is(hdr, "data")) {
            data_start_ = body;
            /* Clamp: upstream's own writer overstates this by 104 bytes. */
            const uint64_t available = file_size_ - body;
            data_size_ = static_cast<uint32_t>(std::min<uint64_t>(size, available));
            have_data = true;
        } else if (tag_is(hdr, "LIST")) {
            const uint64_t available = file_size_ - body;
            scan_info_list(body, static_cast<uint32_t>(std::min<uint64_t>(size, available)));
        }

        /* RIFF pads odd chunks to an even boundary. Some writers do not, so if
         * the padded position does not look like a chunk header, fall back to
         * the unpadded one. */
        const uint64_t next_padded = body + size + (size & 1u);
        const uint64_t next_bare = body + size;
        pos = next_padded;
        if (next_padded != next_bare && next_padded + 8 <= file_size_) {
            uint8_t probe[4];
            stream_.seekg(static_cast<std::streamoff>(next_padded), std::ios::beg);
            if (read_exact(probe, 4) && !is_chunk_id(probe)) pos = next_bare;
        }
    }

    const bool format_ok = (format_tag_ == format_pcm) || (format_tag_ == format_extensible);
    const bool depth_ok = (bits_per_sample_ == 8) || (bits_per_sample_ == 16) ||
                          (bits_per_sample_ == 24) || (bits_per_sample_ == 32);

    if (!have_fmt || !have_data || !format_ok || !depth_ok ||
        channels_ == 0 || sample_rate_ == 0) {
        close();
        return false;
    }

    bytes_per_sample_ = bits_per_sample_ / 8u;
    open_ = true;
    rewind();
    return true;
}

uint32_t WavFileReader::sample_count() const {
    if (bytes_per_sample_ == 0) return 0;
    return data_size_ / bytes_per_sample_;
}

uint32_t WavFileReader::frame_count() const {
    if (channels_ == 0) return 0;
    return sample_count() / channels_;
}

uint32_t WavFileReader::ms_duration() const {
    const uint64_t divisor =
        static_cast<uint64_t>(bytes_per_sample_) * channels_ * sample_rate_;
    if (divisor == 0) return 0;
    return static_cast<uint32_t>((static_cast<uint64_t>(data_size_) * 1000u) / divisor);
}

void WavFileReader::rewind() {
    position_ = data_start_;
}

bool WavFileReader::data_seek(uint64_t sample_offset) {
    if (!open_) return false;
    const uint64_t byte_offset = sample_offset * bytes_per_sample_;
    if (byte_offset > data_size_) {
        position_ = data_start_ + data_size_;
        return false;
    }
    position_ = data_start_ + byte_offset;
    return true;
}

uint64_t WavFileReader::tell_sample() const {
    if (bytes_per_sample_ == 0) return 0;
    return (position_ - data_start_) / bytes_per_sample_;
}

size_t WavFileReader::read(void* buffer, size_t bytes) {
    if (!open_ || bytes == 0) return 0;

    const uint64_t consumed = position_ - data_start_;
    if (consumed >= data_size_) return 0;

    const size_t remaining = static_cast<size_t>(data_size_ - consumed);
    const size_t want = std::min(bytes, remaining);

    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(position_), std::ios::beg);
    stream_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(want));
    const size_t got = static_cast<size_t>(stream_.gcount());
    position_ += got;
    return got;
}

size_t WavFileReader::read_samples(int16_t* out, size_t count) {
    if (!open_ || out == nullptr || count == 0) return 0;

    if (bits_per_sample_ == 16) {
        size_t produced = 0;
        uint8_t block[512];
        while (produced < count) {
            const size_t want = std::min(count - produced, sizeof(block) / 2u);
            const size_t got = read(block, want * 2u);
            const size_t samples = got / 2u;
            for (size_t i = 0; i < samples; ++i)
                out[produced + i] = static_cast<int16_t>(rd_u16(block + i * 2));
            produced += samples;
            if (samples < want) break;
        }
        return produced;
    }

    if (bits_per_sample_ == 8) {
        size_t produced = 0;
        uint8_t block[256];
        while (produced < count) {
            const size_t want = std::min(count - produced, sizeof(block));
            const size_t got = read(block, want);
            for (size_t i = 0; i < got; ++i) {
                /* Same promotion the WAV viewer applies to 8-bit files. */
                out[produced + i] = static_cast<int16_t>((static_cast<int>(block[i]) - 0x80) * 256);
            }
            produced += got;
            if (got < want) break;
        }
        return produced;
    }

    return 0; /* 24- and 32-bit files can still be read raw with read() */
}

/* -------------------------------------------------------------------------
 * Writer
 * ---------------------------------------------------------------------- */

WavFileWriter::~WavFileWriter() {
    close();
}

bool WavFileWriter::write_header(uint32_t data_size, uint32_t info_size, uint32_t pad) {
    uint8_t header[wav_header_size]{};

    const uint16_t block_align =
        static_cast<uint16_t>(channels_ * (bits_per_sample_ / 8u));

    wr_tag(header + 0, "RIFF");
    /* Upstream: sizeof(header_t) + data + info - 8, plus the alignment byte
     * after an odd-length data chunk (upstream only ever writes even ones). */
    wr_u32(header + 4, wav_header_size + data_size + pad + info_size - 8u);
    wr_tag(header + 8, "WAVE");

    wr_tag(header + 12, "fmt ");
    wr_u32(header + 16, 16);
    wr_u16(header + 20, format_pcm);
    wr_u16(header + 22, channels_);
    wr_u32(header + 24, sample_rate_);
    wr_u32(header + 28, sample_rate_ * block_align);
    wr_u16(header + 32, block_align);
    wr_u16(header + 34, bits_per_sample_);

    wr_tag(header + 36, "data");
    wr_u32(header + 40, data_size);

    stream_.seekp(0, std::ios::beg);
    stream_.write(reinterpret_cast<const char*>(header), sizeof(header));
    return stream_.good();
}

bool WavFileWriter::write_tags() {
    /* Byte-for-byte upstream's tags_t. */
    uint8_t tags[wav_info_chunk_size]{};

    wr_tag(tags + 0, "LIST");
    wr_u32(tags + 4, wav_info_chunk_size - 8u);
    wr_tag(tags + 8, "INFO");

    wr_tag(tags + 12, "IART");
    wr_u32(tags + 16, static_cast<uint32_t>(wav_artist_field_size));
    const size_t artist_len = std::min(std::strlen(wav_artist), wav_artist_field_size - 1);
    std::memcpy(tags + 20, wav_artist, artist_len);

    wr_tag(tags + 32, "INAM");
    wr_u32(tags + 36, static_cast<uint32_t>(wav_title_field_size));
    const size_t title_len = std::min(title_.size(), wav_title_field_size - 1);
    if (title_len > 0) std::memcpy(tags + 40, title_.data(), title_len);

    stream_.seekp(0, std::ios::end);
    stream_.write(reinterpret_cast<const char*>(tags), sizeof(tags));
    return stream_.good();
}

bool WavFileWriter::create(const std::string& path,
                           uint32_t sample_rate,
                           const std::string& title,
                           uint16_t channels,
                           uint16_t bits_per_sample) {
    close();

    if (sample_rate == 0 || channels == 0) return false;
    if (bits_per_sample != 8 && bits_per_sample != 16) return false;

    stream_.open(path, std::ios::out | std::ios::binary | std::ios::trunc);
    if (!stream_.is_open()) return false;

    sample_rate_ = sample_rate;
    channels_ = channels;
    bits_per_sample_ = bits_per_sample;
    bytes_per_sample_ = bits_per_sample / 8u;
    data_bytes_ = 0;
    title_ = title;
    failed_ = false;
    open_ = true;

    /* Placeholder; close() rewrites it with the real lengths. */
    if (!write_header(0, wav_info_chunk_size, 0)) {
        failed_ = true;
        close();
        return false;
    }
    return true;
}

bool WavFileWriter::write(const void* data, size_t bytes) {
    if (!open_ || failed_) return false;
    if (bytes == 0) return true;

    stream_.seekp(0, std::ios::end);
    stream_.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    if (!stream_.good()) {
        failed_ = true;
        return false;
    }
    data_bytes_ += bytes;
    return true;
}

bool WavFileWriter::write_samples(const int16_t* samples, size_t count) {
    if (!open_ || failed_) return false;
    if (samples == nullptr) return false;
    if (count == 0) return true;

    if (bits_per_sample_ == 16) {
        uint8_t block[512];
        size_t done = 0;
        while (done < count) {
            const size_t chunk = std::min(count - done, sizeof(block) / 2u);
            for (size_t i = 0; i < chunk; ++i)
                wr_u16(block + i * 2, static_cast<uint16_t>(samples[done + i]));
            if (!write(block, chunk * 2u)) return false;
            done += chunk;
        }
        return true;
    }

    /* 8-bit: inverse of the reader's (v - 0x80) * 256 promotion. */
    uint8_t block[256];
    size_t done = 0;
    while (done < count) {
        const size_t chunk = std::min(count - done, sizeof(block));
        for (size_t i = 0; i < chunk; ++i) {
            const int v = (samples[done + i] / 256) + 0x80;
            block[i] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
        if (!write(block, chunk)) return false;
        done += chunk;
    }
    return true;
}

bool WavFileWriter::close() {
    if (!open_) return !failed_;

    bool ok = !failed_;

    const uint32_t data_size = static_cast<uint32_t>(data_bytes_);
    const uint32_t pad = data_size & 1u;

    /* RIFF chunks start on even offsets. An 8-bit file with an odd sample count
     * needs the alignment byte or the LIST chunk that follows is unreadable. */
    if (ok && pad != 0) {
        const char zero = 0;
        stream_.seekp(0, std::ios::end);
        stream_.write(&zero, 1);
        ok = stream_.good();
    }

    if (ok) ok = write_tags();

    /* Upstream computes the data length from bytes_written_ *after* the tags
     * chunk has been appended, so its files claim 104 bytes more data than they
     * hold and its own reader then hunts for INAM past the end of the file. The
     * real length goes in here. */
    if (ok) ok = write_header(data_size, wav_info_chunk_size, pad);

    stream_.flush();
    stream_.close();
    open_ = false;
    failed_ = !ok;
    return ok;
}

uint32_t WavFileWriter::sample_count() const {
    if (bytes_per_sample_ == 0) return 0;
    return static_cast<uint32_t>(data_bytes_ / bytes_per_sample_);
}

uint32_t WavFileWriter::ms_duration() const {
    const uint64_t divisor =
        static_cast<uint64_t>(bytes_per_sample_) * channels_ * sample_rate_;
    if (divisor == 0) return 0;
    return static_cast<uint32_t>((data_bytes_ * 1000u) / divisor);
}

}  // namespace core
