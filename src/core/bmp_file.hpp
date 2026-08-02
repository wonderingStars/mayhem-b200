/*
 * mayhem-b200 — uncompressed BMP reading and writing.
 *
 * Ported from firmware/common/bmpfile.{hpp,cpp} and firmware/common/bmp.hpp.
 * The SSTV, WEFAX and NOAA-APT receivers stream one scan line at a time into a
 * growing BMP, the BMP/screenshot viewer reads rows back out, and the painter
 * writes whole images — so the same class has to do incremental writes,
 * random-access reads and in-place growth, exactly as upstream's does.
 *
 * Pixels cross the API as ui::Color, the RGB565 framebuffer representation.
 * That is lossy against a 24-bit BMP: red and blue keep 5 bits, green keeps 6,
 * so a value read back from a file it was written to can differ by up to 7
 * (red, blue) or 3 (green).
 *
 * Files created here are 24-bit, top-down (negative height), 4-byte row
 * aligned, with the 54-byte BITMAPFILEHEADER + BITMAPINFOHEADER upstream
 * writes — the same bytes Mayhem's own viewer parses.
 *
 * Deviations from upstream, each documented at the point of use in
 * bmp_file.cpp:
 *
 *  - read_next_px_cnt() stops at the end of a row instead of running through
 *    the padding bytes, and follows the file's row order. Identical results for
 *    the unpadded top-down images upstream's callers use; correct for the rest.
 *  - reads and writes past the last pixel fail instead of silently re-reading
 *    it, and expand_y() refuses to grow a bottom-up file rather than flipping
 *    it to top-down and reversing every existing row.
 *  - growth is zero-filled rather than left as whatever the medium held.
 *
 * Copyright (C) 2024 HTotoo
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_BMP_FILE_H__
#define __MB200_BMP_FILE_H__

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

#include "ui.hpp"

namespace core {

/* BITMAPFILEHEADER (14) + BITMAPINFOHEADER (40). */
inline constexpr uint32_t bmp_header_size = 54;

/* Bytes one row of `width` pixels occupies, rounded up to the 4-byte boundary
 * the format requires. */
constexpr uint32_t bmp_bytes_per_row(uint32_t width, uint32_t bytes_per_pixel) {
    return (width * bytes_per_pixel + 3u) & ~3u;
}

class BmpFile {
   public:
    BmpFile() = default;
    ~BmpFile();

    BmpFile(const BmpFile&) = delete;
    BmpFile& operator=(const BmpFile&) = delete;
    BmpFile(BmpFile&&) = delete;
    BmpFile& operator=(BmpFile&&) = delete;

    /* Opens an existing BMP and parses its header. 24-, 32- and 16-bit
     * (ARGB1555) uncompressed images are supported; 8-bit palette images are
     * rejected, as upstream rejects them. */
    bool open(const std::string& path, bool readonly = true);

    /* Creates a 24-bit top-down image of `width` x `height`, zero-filled (or
     * filled with the colour set by set_bg_color()). Leaves the cursor at 0,0. */
    bool create(const std::string& path, uint32_t width, uint32_t height);

    void close();
    bool is_loaded() const { return is_opened_; }
    bool is_read_only() const { return is_read_only_; }

    uint32_t get_width() const;
    uint32_t get_real_height() const; /* magnitude of the header's height */
    bool is_bottomup() const;         /* header height >= 0 */
    uint32_t bytes_per_row() const { return byte_per_row_; }
    uint32_t bytes_per_pixel() const { return byte_per_px_; }
    uint16_t bits_per_pixel() const { return bpp_; }

    /* Moves the cursor to a pixel. y == 0 is always the top row, whichever way
     * round the rows are stored. False if x or y is outside the image. */
    bool seek(uint32_t x, uint32_t y);

    uint32_t cursor_x() const { return currx_; }
    uint32_t cursor_y() const { return curry_; }
    bool at_end() const { return at_end_; }

    /* Grows the image to `new_y` rows and leaves the cursor at the start of the
     * first new row. A file already that tall or taller is left alone. */
    bool expand_y(uint32_t new_y);
    bool expand_y_delta(uint32_t delta_y);

    /* Reads the pixel under the cursor. With `advance` the cursor moves on by
     * one pixel; false once it has passed the last pixel. */
    bool read_next_px(ui::Color& px, bool advance = true);

    /* Reads `count` pixels in raster order starting at the cursor. With
     * `advance` false the cursor is left where it was, which is how the map and
     * BMP viewers use it: seek to the start of a row, pull the row out. */
    bool read_next_px_cnt(ui::Color* px, uint32_t count, bool advance = true);

    /* Writes the pixel under the cursor and moves on by one. False on a
     * read-only file, an 8-bit file, or once past the last pixel. */
    bool write_next_px(ui::Color px);

    /* Colour used to fill rows added by create() and expand_y(). */
    void set_bg_color(ui::Color background);
    void delete_bg_color();

   private:
    enum class Format {
        none,
        bgr24,     /* upstream type 1 */
        bgra32,    /* upstream type 2 */
        palette8,  /* upstream type 4 — rejected */
        argb1555,  /* upstream type 5 */
    };

    struct Header {
        uint32_t size{0};
        uint32_t image_data{bmp_header_size};
        uint32_t bih_size{40};
        uint32_t width{0};
        int32_t height{0};
        uint16_t planes{1};
        uint16_t bpp{0};
        uint32_t compression{0};
        uint32_t data_size{0};
        uint32_t h_res{100};
        uint32_t v_res{100};
        uint32_t colors_count{0};
        uint32_t icolors_count{0};
    };

    bool read_at(uint64_t pos, void* buffer, size_t bytes);
    bool write_at(uint64_t pos, const void* buffer, size_t bytes);
    bool write_header();
    bool advance_px(uint32_t num);
    ui::Color decode_px(const uint8_t* raw) const;
    bool encode_px(ui::Color px, uint8_t* raw) const;
    bool fill_rows(uint32_t first_row, uint32_t row_count);

    std::fstream stream_{};
    bool is_opened_{false};
    bool is_read_only_{true};

    Header header_{};
    Format format_{Format::none};
    uint16_t bpp_{0};
    uint32_t byte_per_px_{0};
    uint32_t byte_per_row_{0};

    uint64_t file_pos_{0};
    uint32_t currx_{0};
    uint32_t curry_{0};
    bool at_end_{false};

    ui::Color bg_{};
    bool use_bg_{false};

    std::vector<uint8_t> scratch_{};
};

/* Loads a whole BMP into a top-to-bottom RGB565 framebuffer. */
bool load_bmp_rgb565(const std::string& path,
                     std::vector<ui::Color>& out,
                     uint32_t& width,
                     uint32_t& height);

/* Writes a top-to-bottom RGB565 framebuffer as a 24-bit BMP. */
bool save_bmp_rgb565(const std::string& path,
                     const ui::Color* pixels,
                     uint32_t width,
                     uint32_t height);

}  // namespace core

#endif /*__MB200_BMP_FILE_H__*/
