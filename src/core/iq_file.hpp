/*
 * mayhem-b200 — IQ capture file formats (.C16 / .C8) and the Mayhem .TXT
 * metadata sidecar.
 *
 * These are exactly the files a PortaPack writes to /CAPTURES, so a capture
 * taken here opens in Mayhem's own tooling and a capture taken on a PortaPack
 * replays here. The formats are:
 *
 *   .C16  interleaved signed 16-bit little-endian, I then Q, no header.
 *   .C8   interleaved signed  8-bit,               I then Q, no header.
 *   .TXT  "key=value" lines beside the capture, same stem:
 *
 *           center_frequency=433920000
 *           sample_rate=500000
 *           latitude=51.5074000       (optional, only when a fix was held)
 *           longitude=-0.1278000      (optional)
 *           satinuse=9                (optional)
 *
 * Scaling. Upstream stores raw ADC-domain integers and converts between the
 * two widths by a factor of 256 (io_convert.cpp: c16_to_c8 divides, c8_to_c16
 * multiplies). Here samples are std::complex<float> nominally in [-1, 1], so
 * each format has a full-scale integer: 32767 for C16 (which is what
 * radio::ReceiverModel already writes) and 127 for C8. Those two are the same
 * ratio upstream uses to within one LSB, so a C8 file written here has the
 * amplitude a PortaPack expects.
 *
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc. (file formats)
 * Copyright (C) 2023 Kyle Reed (metadata sidecar)
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_IQ_FILE_H__
#define __MB200_IQ_FILE_H__

#include <complex>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace core {

using cfloat = std::complex<float>;

/* --- Format ---------------------------------------------------------------- */

enum class IqFormat : uint8_t {
    Unknown = 0,
    C16 = 1,  /* interleaved int16 */
    C8 = 2,   /* interleaved int8  */
};

/* Deduced from the extension, case-insensitively, as upstream's
 * capture_file_sample_size() does. Unknown for anything else — .CU8 is a
 * PortaPack read-only format and is deliberately not claimed here. */
IqFormat iq_format_from_path(std::string_view path);

const char* iq_format_name(IqFormat format);       /* "C16", "C8", "?"     */
const char* iq_format_extension(IqFormat format);  /* ".C16", ".C8", ""    */

/* Bytes one I/Q pair occupies on disk: 4 for C16, 2 for C8, 0 for Unknown. */
size_t iq_format_frame_bytes(IqFormat format);

/* Integer magnitude that corresponds to a float sample of 1.0. */
float iq_format_full_scale(IqFormat format);

/* --- Buffer conversion -----------------------------------------------------
 * `count` is I/Q pairs; the integer buffers hold 2*count elements. Floats
 * outside [-1, 1] are clamped so an overdriven capture saturates rather than
 * wrapping to the opposite rail. */

void iq_to_c16(const cfloat* in, size_t count, int16_t* out);
void c16_to_iq(const int16_t* in, size_t count, cfloat* out);
void iq_to_c8(const cfloat* in, size_t count, int8_t* out);
void c8_to_iq(const int8_t* in, size_t count, cfloat* out);

/* --- Metadata sidecar ------------------------------------------------------ */

struct CaptureMetadata {
    uint64_t center_frequency{0};
    uint32_t sample_rate{0};
    float latitude{0.0f};
    float longitude{0.0f};
    uint8_t satinuse{0};

    /* Upstream's gate for writing the GPS lines at all
     * (metadata_file.cpp: both non-zero and both under 200). */
    bool has_position() const;
};

/* "<stem>.C16" -> "<stem>.TXT", matching get_metadata_path(). */
std::string metadata_path_for(std::string_view capture_path);

/* Writes the sidecar. The GPS lines are emitted only when has_position().
 * Returns false and fills `error` (when non-null) on an I/O failure. */
bool write_metadata_file(const std::string& path,
                         const CaptureMetadata& metadata,
                         std::string* error = nullptr);

/* Reads the sidecar. Unknown keys and malformed lines are skipped, as upstream
 * does. Returns false if the file is missing, or if it parsed but left either
 * center_frequency or sample_rate at zero — upstream treats that as a failed
 * parse rather than a valid capture. */
bool read_metadata_file(const std::string& path,
                        CaptureMetadata& out,
                        std::string* error = nullptr);

/* --- Whole-file inspection -------------------------------------------------
 * Everything a file browser needs about a capture without reading its samples:
 * format from the extension, sample count from the size, and duration from the
 * sidecar's sample rate. */

struct IqFileInfo {
    bool valid{false};        /* format known and size consistent */
    std::string path{};
    IqFormat format{IqFormat::Unknown};
    uint64_t file_bytes{0};
    uint64_t samples{0};      /* whole I/Q pairs; a partial tail is not counted */
    bool truncated{false};    /* file_bytes is not a whole number of samples */
    bool has_metadata{false};
    CaptureMetadata metadata{};
    double sample_rate{0.0};  /* from the sidecar; 0 when there is none */
    double duration_seconds{0.0};
    uint32_t duration_ms{0};  /* integer form, matching upstream ms_duration() */
    std::string error{};
};

IqFileInfo inspect_iq_file(const std::string& path);

/* Duration in whole milliseconds, computed the way upstream's ms_duration()
 * does (integer arithmetic, so it truncates). Zero if either argument is. */
uint32_t iq_duration_ms(uint64_t samples, uint32_t sample_rate);

/* --- Writer ---------------------------------------------------------------- */

class IqFileWriter {
   public:
    IqFileWriter() = default;
    ~IqFileWriter();

    IqFileWriter(const IqFileWriter&) = delete;
    IqFileWriter& operator=(const IqFileWriter&) = delete;

    /* Creates or truncates `path`. The format comes from the extension unless
     * `format` overrides it. Returns false and sets error() on failure. */
    bool open(const std::string& path, IqFormat format = IqFormat::Unknown);

    /* Appends `count` I/Q pairs. Returns false on a write error, after which
     * the writer stays open but every further write fails. */
    bool write(const cfloat* samples, size_t count);

    bool flush();
    void close();

    bool is_open() const { return file_.is_open(); }
    IqFormat format() const { return format_; }
    const std::string& path() const { return path_; }
    uint64_t samples_written() const { return samples_written_; }
    const std::string& error() const { return error_; }

   private:
    std::ofstream file_{};
    IqFormat format_{IqFormat::Unknown};
    std::string path_{};
    std::string error_{};
    uint64_t samples_written_{0};
    std::vector<int16_t> scratch16_{};
    std::vector<int8_t> scratch8_{};
};

/* --- Streaming reader -------------------------------------------------------
 * Yields blocks of std::complex<float> and knows where it is in the file, so a
 * replay UI can show a position bar and scrub. Not thread-safe: one owner at a
 * time (radio::ReplayModel gives it to its DSP thread). */

class IqFileReader {
   public:
    IqFileReader() = default;
    ~IqFileReader();

    IqFileReader(const IqFileReader&) = delete;
    IqFileReader& operator=(const IqFileReader&) = delete;

    /* Opens `path` for reading and, if a sidecar exists beside it, loads the
     * metadata too. The format comes from the extension unless `format`
     * overrides it.
     *
     * Fails, with error() set, when the file is missing, the extension is not a
     * known IQ format, or the size is not a whole number of samples. That last
     * case — a capture cut short by a yanked drive — can be opened anyway with
     * `allow_partial_tail`, which drops the incomplete sample; truncated()
     * still reports it. */
    bool open(const std::string& path,
              IqFormat format = IqFormat::Unknown,
              bool allow_partial_tail = false);

    void close();

    bool is_open() const { return file_.is_open(); }
    const std::string& path() const { return path_; }
    IqFormat format() const { return format_; }
    const std::string& error() const { return error_; }

    /* Whole I/Q pairs in the file. */
    uint64_t total_samples() const { return total_samples_; }
    uint64_t position() const { return position_; }
    uint64_t remaining() const { return total_samples_ - position_; }
    bool at_end() const { return position_ >= total_samples_; }
    bool truncated() const { return truncated_; }

    /* Reads up to `count` samples. Returns how many were read: fewer than
     * asked for means end of file, 0 means nothing left (or an error, which
     * sets error() and latches failed()). */
    size_t read(cfloat* out, size_t count);

    /* Same, resizing `out` to what was actually read. */
    size_t read(std::vector<cfloat>& out, size_t count);

    bool failed() const { return failed_; }

    /* Moves to `sample_index`. Seeking to total_samples() is legal and leaves
     * the reader at end of file; beyond that fails. */
    bool seek(uint64_t sample_index);
    bool rewind() { return seek(0); }

    bool has_metadata() const { return has_metadata_; }
    const CaptureMetadata& metadata() const { return metadata_; }

    /* Sample rate from the sidecar, or 0 when there is none. */
    double sample_rate() const;

    /* Length in seconds at the sidecar's rate, or at `rate_hz` when given.
     * 0 when the rate is unknown. */
    double duration_seconds() const;
    double duration_seconds(double rate_hz) const;

   private:
    bool fill_scratch(size_t frames);

    std::ifstream file_{};
    IqFormat format_{IqFormat::Unknown};
    std::string path_{};
    std::string error_{};
    uint64_t total_samples_{0};
    uint64_t position_{0};
    bool truncated_{false};
    bool failed_{false};
    bool has_metadata_{false};
    CaptureMetadata metadata_{};
    std::vector<char> scratch_{};
};

}  // namespace core

#endif /*__MB200_IQ_FILE_H__*/
