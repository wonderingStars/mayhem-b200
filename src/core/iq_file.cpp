/*
 * mayhem-b200 — IQ capture file formats and the Mayhem .TXT metadata sidecar.
 *
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc. (file formats)
 * Copyright (C) 2023 Kyle Reed (metadata sidecar)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "iq_file.hpp"

#include "string_format.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace core {

namespace {

/* Full-scale integer magnitudes. 32767 for C16 is what ReceiverModel's capture
 * tap already writes; 127 for C8 keeps the same ratio upstream's io_convert.cpp
 * uses between the two widths (it divides/multiplies by 256, and
 * 32767/256 = 127.996). */
constexpr float kFullScaleC16 = 32767.0f;
constexpr float kFullScaleC8 = 127.0f;

/* Samples converted per file-I/O round trip. 64k samples is 256 kB of C16,
 * enough to keep the disk busy without a large permanent allocation. */
constexpr size_t kMaxChunkSamples = 65536;

std::string to_lower(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

/* Extension including the dot, or "" when there is none. Only looks after the
 * last path separator so "C:/a.dir/capture" is not given the extension ".dir". */
std::string_view path_extension(std::string_view path) {
    const size_t slash = path.find_last_of("/\\");
    const size_t start = (slash == std::string_view::npos) ? 0 : slash + 1;
    const size_t dot = path.find_last_of('.');
    if (dot == std::string_view::npos || dot < start) return {};
    return path.substr(dot);
}

int16_t float_to_i16(float v) {
    const float c = std::clamp(v, -1.0f, 1.0f);
    return static_cast<int16_t>(std::lrint(c * kFullScaleC16));
}

int8_t float_to_i8(float v) {
    const float c = std::clamp(v, -1.0f, 1.0f);
    return static_cast<int8_t>(std::lrint(c * kFullScaleC8));
}

/* Strips a trailing CR so a file written with CRLF (which is what Mayhem's
 * File::write_line emits) parses the same as one written with LF. */
std::string_view strip_cr(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
        line.remove_suffix(1);
    return line;
}

bool parse_u64(std::string_view s, uint64_t& out) {
    const std::string t = trim(s);
    if (t.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(t.c_str(), &end, 10);
    if (errno == ERANGE || end == t.c_str()) return false;
    out = static_cast<uint64_t>(v);
    return true;
}

bool parse_float(std::string_view s, float& out) {
    const std::string t = trim(s);
    if (t.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (errno == ERANGE || end == t.c_str()) return false;
    out = static_cast<float>(v);
    return true;
}

void set_error(std::string* sink, std::string message) {
    if (sink != nullptr) *sink = std::move(message);
}

}  // namespace

/* --- Format ---------------------------------------------------------------- */

IqFormat iq_format_from_path(std::string_view path) {
    const std::string ext = to_lower(path_extension(path));
    if (ext == ".c16") return IqFormat::C16;
    if (ext == ".c8") return IqFormat::C8;
    return IqFormat::Unknown;
}

const char* iq_format_name(IqFormat format) {
    switch (format) {
        case IqFormat::C16: return "C16";
        case IqFormat::C8: return "C8";
        case IqFormat::Unknown: break;
    }
    return "?";
}

const char* iq_format_extension(IqFormat format) {
    switch (format) {
        case IqFormat::C16: return ".C16";
        case IqFormat::C8: return ".C8";
        case IqFormat::Unknown: break;
    }
    return "";
}

size_t iq_format_frame_bytes(IqFormat format) {
    switch (format) {
        case IqFormat::C16: return 2 * sizeof(int16_t);
        case IqFormat::C8: return 2 * sizeof(int8_t);
        case IqFormat::Unknown: break;
    }
    return 0;
}

float iq_format_full_scale(IqFormat format) {
    switch (format) {
        case IqFormat::C16: return kFullScaleC16;
        case IqFormat::C8: return kFullScaleC8;
        case IqFormat::Unknown: break;
    }
    return 0.0f;
}

/* --- Buffer conversion ------------------------------------------------------
 * The integer buffers are laid out exactly as they sit on disk, so these double
 * as the serialisation step. x86 is little-endian and so is the file format, so
 * no byte swapping is needed; a big-endian host would need it here. */

void iq_to_c16(const cfloat* in, size_t count, int16_t* out) {
    for (size_t i = 0; i < count; i++) {
        out[2 * i + 0] = float_to_i16(in[i].real());
        out[2 * i + 1] = float_to_i16(in[i].imag());
    }
}

void c16_to_iq(const int16_t* in, size_t count, cfloat* out) {
    for (size_t i = 0; i < count; i++)
        out[i] = cfloat{static_cast<float>(in[2 * i + 0]) / kFullScaleC16,
                        static_cast<float>(in[2 * i + 1]) / kFullScaleC16};
}

void iq_to_c8(const cfloat* in, size_t count, int8_t* out) {
    for (size_t i = 0; i < count; i++) {
        out[2 * i + 0] = float_to_i8(in[i].real());
        out[2 * i + 1] = float_to_i8(in[i].imag());
    }
}

void c8_to_iq(const int8_t* in, size_t count, cfloat* out) {
    for (size_t i = 0; i < count; i++)
        out[i] = cfloat{static_cast<float>(in[2 * i + 0]) / kFullScaleC8,
                        static_cast<float>(in[2 * i + 1]) / kFullScaleC8};
}

/* --- Metadata sidecar ------------------------------------------------------ */

bool CaptureMetadata::has_position() const {
    /* metadata_file.cpp's gate, reproduced including its asymmetry: the upper
     * bound is a plain `< 200`, not on the magnitude. */
    return latitude != 0.0f && longitude != 0.0f && latitude < 200.0f && longitude < 200.0f;
}

std::string metadata_path_for(std::string_view capture_path) {
    const std::string_view ext = path_extension(capture_path);
    std::string out{capture_path};
    out.resize(capture_path.size() - ext.size());
    out += ".TXT";
    return out;
}

bool write_metadata_file(const std::string& path,
                         const CaptureMetadata& metadata,
                         std::string* error) {
    /* Binary mode plus an explicit CRLF: that is what a PortaPack writes, and
     * it keeps the bytes the same whatever platform this runs on. */
    std::ofstream f{path, std::ios::binary | std::ios::trunc};
    if (!f.is_open()) {
        set_error(error, "cannot create " + path);
        return false;
    }

    f << "center_frequency=" << to_string_dec_uint(metadata.center_frequency) << "\r\n";
    f << "sample_rate=" << to_string_dec_uint(metadata.sample_rate) << "\r\n";

    if (metadata.has_position()) {
        /* to_string_decimal() is the firmware's own formatter, so these lines
         * come out identical to Mayhem's. It carries the sign on the integer
         * part, which loses it for values in (-1, 0); the leading '-' below
         * puts it back rather than filing a capture in the wrong hemisphere. */
        const char* lat_sign = (metadata.latitude < 0.0f && metadata.latitude > -1.0f) ? "-" : "";
        const char* lon_sign = (metadata.longitude < 0.0f && metadata.longitude > -1.0f) ? "-" : "";

        f << "latitude=" << lat_sign << to_string_decimal(metadata.latitude, 7) << "\r\n";
        f << "longitude=" << lon_sign << to_string_decimal(metadata.longitude, 7) << "\r\n";
        f << "satinuse=" << to_string_dec_uint(metadata.satinuse) << "\r\n";
    }

    f.flush();
    if (!f.good()) {
        set_error(error, "write failed: " + path);
        return false;
    }
    return true;
}

bool read_metadata_file(const std::string& path, CaptureMetadata& out, std::string* error) {
    std::ifstream f{path, std::ios::binary};
    if (!f.is_open()) {
        set_error(error, "cannot open " + path);
        return false;
    }

    CaptureMetadata parsed{};
    std::string line;
    while (std::getline(f, line)) {
        const std::string_view sv = strip_cr(line);

        /* Upstream splits on '=' and skips anything that does not yield exactly
         * two columns, so "a=b=c" is a bad line, not a value of "b=c". */
        const size_t eq = sv.find('=');
        if (eq == std::string_view::npos) continue;
        if (sv.find('=', eq + 1) != std::string_view::npos) continue;

        const std::string key = trim(sv.substr(0, eq));
        const std::string_view value = sv.substr(eq + 1);

        if (key == "center_frequency") {
            parse_u64(value, parsed.center_frequency);
        } else if (key == "sample_rate") {
            uint64_t v = 0;
            if (parse_u64(value, v) && v <= UINT32_MAX)
                parsed.sample_rate = static_cast<uint32_t>(v);
        } else if (key == "latitude") {
            parse_float(value, parsed.latitude);
        } else if (key == "longitude") {
            parse_float(value, parsed.longitude);
        } else if (key == "satinuse") {
            uint64_t v = 0;
            if (parse_u64(value, v) && v <= 255)
                parsed.satinuse = static_cast<uint8_t>(v);
        }
    }

    /* Same acceptance test as read_metadata_file(): without both a centre
     * frequency and a sample rate the sidecar tells us nothing usable. */
    if (parsed.center_frequency == 0 || parsed.sample_rate == 0) {
        set_error(error, "no usable metadata in " + path);
        return false;
    }

    out = parsed;
    return true;
}

/* --- Whole-file inspection ------------------------------------------------- */

uint32_t iq_duration_ms(uint64_t samples, uint32_t sample_rate) {
    if (sample_rate == 0 || samples == 0) return 0;
    const uint64_t ms = (samples * 1000ULL) / sample_rate;
    return (ms > UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(ms);
}

IqFileInfo inspect_iq_file(const std::string& path) {
    IqFileInfo info{};
    info.path = path;
    info.format = iq_format_from_path(path);

    if (info.format == IqFormat::Unknown) {
        info.error = "not a .C16 or .C8 capture: " + path;
        return info;
    }

    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        info.error = "cannot read " + path + ": " + ec.message();
        return info;
    }

    const size_t frame = iq_format_frame_bytes(info.format);
    info.file_bytes = static_cast<uint64_t>(size);
    info.samples = info.file_bytes / frame;
    info.truncated = (info.file_bytes % frame) != 0;

    CaptureMetadata md{};
    if (read_metadata_file(metadata_path_for(path), md)) {
        info.has_metadata = true;
        info.metadata = md;
        info.sample_rate = static_cast<double>(md.sample_rate);
    }

    if (info.sample_rate > 0.0) {
        info.duration_seconds = static_cast<double>(info.samples) / info.sample_rate;
        info.duration_ms = iq_duration_ms(info.samples, md.sample_rate);
    }

    if (info.truncated) {
        info.error = "truncated: " + to_string_dec_uint(info.file_bytes) +
                     " bytes is not a whole number of " +
                     iq_format_name(info.format) + " samples";
    } else {
        info.valid = true;
    }

    return info;
}

/* --- Writer ---------------------------------------------------------------- */

IqFileWriter::~IqFileWriter() {
    close();
}

bool IqFileWriter::open(const std::string& path, IqFormat format) {
    close();

    error_.clear();
    samples_written_ = 0;
    path_ = path;
    format_ = (format == IqFormat::Unknown) ? iq_format_from_path(path) : format;

    if (format_ == IqFormat::Unknown) {
        error_ = "unknown IQ format for " + path;
        return false;
    }

    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_.is_open()) {
        error_ = "cannot create " + path;
        return false;
    }

    return true;
}

bool IqFileWriter::write(const cfloat* samples, size_t count) {
    if (!file_.is_open()) {
        error_ = "writer is not open";
        return false;
    }
    if (count == 0) return true;
    if (samples == nullptr) {
        error_ = "null sample buffer";
        return false;
    }

    size_t done = 0;
    while (done < count) {
        const size_t n = std::min(count - done, kMaxChunkSamples);
        const char* bytes = nullptr;
        size_t byte_count = 0;

        if (format_ == IqFormat::C16) {
            scratch16_.resize(n * 2);
            iq_to_c16(samples + done, n, scratch16_.data());
            bytes = reinterpret_cast<const char*>(scratch16_.data());
            byte_count = scratch16_.size() * sizeof(int16_t);
        } else {
            scratch8_.resize(n * 2);
            iq_to_c8(samples + done, n, scratch8_.data());
            bytes = reinterpret_cast<const char*>(scratch8_.data());
            byte_count = scratch8_.size() * sizeof(int8_t);
        }

        file_.write(bytes, static_cast<std::streamsize>(byte_count));
        if (!file_.good()) {
            error_ = "write failed: " + path_;
            return false;
        }

        samples_written_ += n;
        done += n;
    }

    return true;
}

bool IqFileWriter::flush() {
    if (!file_.is_open()) return false;
    file_.flush();
    if (!file_.good()) {
        error_ = "flush failed: " + path_;
        return false;
    }
    return true;
}

void IqFileWriter::close() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
}

/* --- Streaming reader ------------------------------------------------------ */

IqFileReader::~IqFileReader() {
    close();
}

bool IqFileReader::open(const std::string& path, IqFormat format, bool allow_partial_tail) {
    close();

    error_.clear();
    path_ = path;
    position_ = 0;
    total_samples_ = 0;
    truncated_ = false;
    failed_ = false;
    has_metadata_ = false;
    metadata_ = CaptureMetadata{};
    format_ = (format == IqFormat::Unknown) ? iq_format_from_path(path) : format;

    if (format_ == IqFormat::Unknown) {
        error_ = "unknown IQ format for " + path;
        return false;
    }

    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        error_ = "cannot read " + path + ": " + ec.message();
        return false;
    }

    const size_t frame = iq_format_frame_bytes(format_);
    const uint64_t bytes = static_cast<uint64_t>(size);
    truncated_ = (bytes % frame) != 0;

    if (truncated_ && !allow_partial_tail) {
        error_ = "truncated: " + to_string_dec_uint(bytes) +
                 " bytes is not a whole number of " + iq_format_name(format_) + " samples";
        return false;
    }

    file_.open(path, std::ios::binary);
    if (!file_.is_open()) {
        error_ = "cannot open " + path;
        return false;
    }

    total_samples_ = bytes / frame;

    /* The sidecar is optional: a capture from another tool may not have one,
     * and the caller decides what to do about a missing sample rate. */
    CaptureMetadata md{};
    if (read_metadata_file(metadata_path_for(path), md)) {
        has_metadata_ = true;
        metadata_ = md;
    }

    return true;
}

void IqFileReader::close() {
    if (file_.is_open()) file_.close();
    file_.clear();
}

size_t IqFileReader::read(cfloat* out, size_t count) {
    if (!file_.is_open() || failed_ || out == nullptr || count == 0) return 0;

    const uint64_t left = remaining();
    if (left == 0) return 0;
    if (static_cast<uint64_t>(count) > left) count = static_cast<size_t>(left);

    const size_t frame = iq_format_frame_bytes(format_);
    const size_t want_bytes = count * frame;

    scratch_.resize(want_bytes);
    file_.read(scratch_.data(), static_cast<std::streamsize>(want_bytes));
    const std::streamsize got = file_.gcount();

    if (got <= 0) {
        /* The size said there was more here. Something removed or replaced the
         * file underneath us; say so rather than handing back a zeroed block. */
        file_.clear();
        failed_ = true;
        error_ = "read failed at sample " + to_string_dec_uint(position_) + " of " + path_;
        return 0;
    }

    const size_t frames = static_cast<size_t>(got) / frame;
    if (frames < count) {
        file_.clear();
        failed_ = true;
        error_ = "short read at sample " + to_string_dec_uint(position_) + " of " + path_;
        if (frames == 0) return 0;
    }

    if (format_ == IqFormat::C16)
        c16_to_iq(reinterpret_cast<const int16_t*>(scratch_.data()), frames, out);
    else
        c8_to_iq(reinterpret_cast<const int8_t*>(scratch_.data()), frames, out);

    position_ += frames;
    return frames;
}

size_t IqFileReader::read(std::vector<cfloat>& out, size_t count) {
    out.resize(count);
    const size_t n = read(out.data(), count);
    out.resize(n);
    return n;
}

bool IqFileReader::seek(uint64_t sample_index) {
    if (!file_.is_open()) {
        error_ = "reader is not open";
        return false;
    }
    if (sample_index > total_samples_) {
        error_ = "seek past end of " + path_;
        return false;
    }

    /* Clear first: at end of file the stream carries eofbit, and seekg on a
     * stream in a failed state is a no-op. */
    file_.clear();
    const uint64_t offset = sample_index * iq_format_frame_bytes(format_);
    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);

    if (!file_.good()) {
        failed_ = true;
        error_ = "seek failed on " + path_;
        return false;
    }

    position_ = sample_index;
    failed_ = false;
    return true;
}

double IqFileReader::sample_rate() const {
    return has_metadata_ ? static_cast<double>(metadata_.sample_rate) : 0.0;
}

double IqFileReader::duration_seconds() const {
    return duration_seconds(sample_rate());
}

double IqFileReader::duration_seconds(double rate_hz) const {
    if (rate_hz <= 0.0) return 0.0;
    return static_cast<double>(total_samples_) / rate_hz;
}

}  // namespace core
