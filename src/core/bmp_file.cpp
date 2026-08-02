/*
 * mayhem-b200 — uncompressed BMP reading and writing.
 *
 * Copyright (C) 2024 HTotoo
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "bmp_file.hpp"

#include <algorithm>
#include <cstring>

namespace core {

namespace {

/* BMP is little-endian on every platform, so fields are (de)serialised by hand
 * rather than by overlaying a packed struct. */

uint16_t rd_u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t rd_u32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

int32_t rd_i32(const uint8_t* p) {
    return static_cast<int32_t>(rd_u32(p));
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

void wr_i32(uint8_t* p, int32_t v) {
    wr_u32(p, static_cast<uint32_t>(v));
}

constexpr uint16_t bmp_signature = 0x4d42; /* 'B','M' */

/* Upstream's BI_RGB / BI_BITFIELDS acceptance. */
constexpr uint32_t bi_rgb = 0;
constexpr uint32_t bi_bitfields = 3;

}  // namespace

BmpFile::~BmpFile() {
    close();
}

void BmpFile::close() {
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
    stream_.clear();
    is_opened_ = false;
    is_read_only_ = true;
    header_ = Header{};
    format_ = Format::none;
    bpp_ = 0;
    byte_per_px_ = 0;
    byte_per_row_ = 0;
    file_pos_ = 0;
    currx_ = 0;
    curry_ = 0;
    at_end_ = false;
}

uint32_t BmpFile::get_width() const {
    if (!is_opened_) return 0;
    return header_.width;
}

uint32_t BmpFile::get_real_height() const {
    if (!is_opened_) return 0;
    return header_.height >= 0 ? static_cast<uint32_t>(header_.height)
                               : static_cast<uint32_t>(-static_cast<int64_t>(header_.height));
}

bool BmpFile::is_bottomup() const {
    return header_.height >= 0;
}

bool BmpFile::read_at(uint64_t pos, void* buffer, size_t bytes) {
    if (bytes == 0) return true;
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(pos), std::ios::beg);
    stream_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(bytes));
    const bool ok = static_cast<size_t>(stream_.gcount()) == bytes;
    stream_.clear();
    if (ok) file_pos_ = pos + bytes;
    return ok;
}

bool BmpFile::write_at(uint64_t pos, const void* buffer, size_t bytes) {
    if (bytes == 0) return true;
    stream_.clear();
    stream_.seekp(static_cast<std::streamoff>(pos), std::ios::beg);
    stream_.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(bytes));
    if (!stream_.good()) return false;
    file_pos_ = pos + bytes;
    return true;
}

bool BmpFile::write_header() {
    uint8_t raw[bmp_header_size]{};

    wr_u16(raw + 0, bmp_signature);
    wr_u32(raw + 2, header_.size);
    wr_u16(raw + 6, 0); /* reserved_1 */
    wr_u16(raw + 8, 0); /* reserved_2 */
    wr_u32(raw + 10, header_.image_data);
    wr_u32(raw + 14, header_.bih_size);
    wr_u32(raw + 18, header_.width);
    wr_i32(raw + 22, header_.height);
    wr_u16(raw + 26, header_.planes);
    wr_u16(raw + 28, header_.bpp);
    wr_u32(raw + 30, header_.compression);
    wr_u32(raw + 34, header_.data_size);
    wr_u32(raw + 38, header_.h_res);
    wr_u32(raw + 42, header_.v_res);
    wr_u32(raw + 46, header_.colors_count);
    wr_u32(raw + 50, header_.icolors_count);

    return write_at(0, raw, sizeof(raw));
}

bool BmpFile::create(const std::string& path, uint32_t width, uint32_t height) {
    close();

    if (width == 0 || height == 0) return false;

    /* Guard the row-stride and file-size arithmetic before it can wrap. */
    if (width > 0x0fffffffu / 3u) return false;

    stream_.open(path, std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
    if (!stream_.is_open()) return false;

    format_ = Format::bgr24;
    bpp_ = 24;
    byte_per_px_ = 3;
    byte_per_row_ = bmp_bytes_per_row(width, byte_per_px_);

    if (height > (0xffffffffu - bmp_header_size) / byte_per_row_) {
        close();
        return false;
    }

    header_ = Header{};
    header_.bpp = 24;
    header_.planes = 1;
    header_.compression = bi_rgb;
    header_.width = width;
    header_.height = 0; /* grown by expand_y() below, as upstream does */
    header_.image_data = bmp_header_size;
    header_.bih_size = 40;
    header_.h_res = 100;
    header_.v_res = 100;
    header_.size = bmp_header_size;
    header_.data_size = 0;

    if (!write_header()) {
        close();
        return false;
    }

    is_opened_ = true;
    is_read_only_ = false;
    currx_ = 0;
    curry_ = 0;
    at_end_ = false;

    if (!expand_y(height)) {
        close();
        return false;
    }

    return seek(0, 0);
}

bool BmpFile::open(const std::string& path, bool readonly) {
    close();

    const auto mode = readonly ? (std::ios::in | std::ios::binary)
                               : (std::ios::in | std::ios::out | std::ios::binary);
    stream_.open(path, mode);
    if (!stream_.is_open()) return false;

    uint8_t raw[bmp_header_size]{};
    if (!read_at(0, raw, sizeof(raw))) {
        close();
        return false;
    }

    const uint16_t signature = rd_u16(raw + 0);
    header_.size = rd_u32(raw + 2);
    header_.image_data = rd_u32(raw + 10);
    header_.bih_size = rd_u32(raw + 14);
    header_.width = rd_u32(raw + 18);
    header_.height = rd_i32(raw + 22);
    header_.planes = rd_u16(raw + 26);
    header_.bpp = rd_u16(raw + 28);
    header_.compression = rd_u32(raw + 30);
    header_.data_size = rd_u32(raw + 34);
    header_.h_res = rd_u32(raw + 38);
    header_.v_res = rd_u32(raw + 42);
    header_.colors_count = rd_u32(raw + 46);
    header_.icolors_count = rd_u32(raw + 50);

    if (signature != bmp_signature || header_.planes != 1 ||
        (header_.compression != bi_rgb && header_.compression != bi_bitfields)) {
        close();
        return false;
    }

    switch (header_.bpp) {
        case 8:
            /* Upstream rejects palette images: decoding one needs the 1 KiB
             * palette resident, which the firmware cannot spare. Rejected here
             * too rather than returning wrong colours. */
            close();
            return false;

        case 16:
            if (header_.compression == bi_bitfields) { /* upstream: not implemented */
                close();
                return false;
            }
            format_ = Format::argb1555;
            byte_per_px_ = 2;
            break;

        case 24:
            format_ = Format::bgr24;
            byte_per_px_ = 3;
            break;

        case 32:
            format_ = Format::bgra32;
            byte_per_px_ = 4;
            break;

        default:
            close();
            return false;
    }

    if (header_.width == 0 || header_.height == 0 || header_.image_data < bmp_header_size) {
        close();
        return false;
    }
    if (header_.width > 0x0fffffffu / byte_per_px_) {
        close();
        return false;
    }

    bpp_ = header_.bpp;
    byte_per_row_ = bmp_bytes_per_row(header_.width, byte_per_px_);
    file_pos_ = header_.image_data;
    is_opened_ = true;
    is_read_only_ = readonly;
    currx_ = 0;
    curry_ = 0;
    at_end_ = false;
    return true;
}

bool BmpFile::seek(uint32_t x, uint32_t y) {
    if (!is_opened_) return false;
    if (x >= header_.width) return false;

    const uint32_t height = get_real_height();
    if (y >= height) return false;

    /* y == 0 is the top row either way: a bottom-up file stores the top row
     * last. Same arithmetic as upstream. */
    const uint32_t stored_row = is_bottomup() ? (height - y - 1) : y;

    file_pos_ = static_cast<uint64_t>(header_.image_data) +
                static_cast<uint64_t>(stored_row) * byte_per_row_ +
                static_cast<uint64_t>(x) * byte_per_px_;
    currx_ = x;
    curry_ = y;
    at_end_ = false;
    return true;
}

bool BmpFile::advance_px(uint32_t num) {
    if (!is_opened_) return false;
    if (at_end_) return false;

    const uint32_t width = header_.width;
    const uint32_t height = get_real_height();

    const uint64_t index = static_cast<uint64_t>(curry_) * width + currx_ + num;
    if (index >= static_cast<uint64_t>(width) * height) {
        /* Upstream leaves the cursor where it was and lets the next read return
         * the same pixel again; latching the end makes that a clean failure. */
        at_end_ = true;
        return false;
    }

    return seek(static_cast<uint32_t>(index % width), static_cast<uint32_t>(index / width));
}

ui::Color BmpFile::decode_px(const uint8_t* raw) const {
    switch (format_) {
        case Format::argb1555: {
            const uint16_t val = static_cast<uint16_t>(raw[0] | (raw[1] << 8));
            uint8_t r = static_cast<uint8_t>((val >> 10) & 0x1f);
            uint8_t g = static_cast<uint8_t>((val >> 5) & 0x1f);
            uint8_t b = static_cast<uint8_t>(val & 0x1f);
            /* Upstream's 5 -> 8 bit expansion. */
            r = static_cast<uint8_t>((r << 3) | (r >> 2));
            g = static_cast<uint8_t>((g << 3) | (g >> 2));
            b = static_cast<uint8_t>((b << 3) | (b >> 2));
            return ui::Color(r, g, b);
        }

        case Format::bgra32:
        case Format::bgr24:
        default:
            return ui::Color(raw[2], raw[1], raw[0]);
    }
}

bool BmpFile::encode_px(ui::Color px, uint8_t* raw) const {
    switch (format_) {
        case Format::argb1555: {
            /* px.r()/g()/b() hand back 8-bit values whose low bits are zero, so
             * the 5-bit fields come out of the top. Upstream's encoder forgets
             * to shift them down and truncates red into the alpha bit; this
             * writes the field the decoder above expects. */
            const uint8_t r5 = static_cast<uint8_t>(px.r() >> 3);
            const uint8_t g5 = static_cast<uint8_t>(px.g() >> 3);
            const uint8_t b5 = static_cast<uint8_t>(px.b() >> 3);
            raw[0] = static_cast<uint8_t>((g5 << 5) | b5);
            raw[1] = static_cast<uint8_t>(0x80 | (r5 << 2) | (g5 >> 3));
            return true;
        }

        case Format::bgra32:
            raw[2] = px.r();
            raw[1] = px.g();
            raw[0] = px.b();
            raw[3] = 255;
            return true;

        case Format::bgr24:
            raw[2] = px.r();
            raw[1] = px.g();
            raw[0] = px.b();
            return true;

        case Format::palette8:
        case Format::none:
        default:
            return false;
    }
}

bool BmpFile::read_next_px(ui::Color& px, bool advance) {
    if (!is_opened_ || at_end_) return false;

    uint8_t raw[4]{};
    if (!read_at(file_pos_, raw, byte_per_px_)) return false;

    px = decode_px(raw);

    if (advance) advance_px(1);
    return true;
}

bool BmpFile::read_next_px_cnt(ui::Color* px, uint32_t count, bool advance) {
    if (!is_opened_ || px == nullptr) return false;
    if (count == 0) return true;
    if (at_end_) return false;

    const uint32_t width = header_.width;
    const uint32_t height = get_real_height();

    const uint32_t start_x = currx_;
    const uint32_t start_y = curry_;

    uint32_t x = currx_;
    uint32_t y = curry_;
    uint32_t remaining = count;
    ui::Color* out = px;

    /* Upstream reads count * bytes_per_pixel contiguously, which walks into the
     * row padding and, on a bottom-up file, into the wrong row. Clamping each
     * read to the end of the current row gives the same answer for the unpadded
     * top-down images its callers use, and the right one otherwise. */
    while (remaining > 0) {
        if (y >= height) {
            if (!advance) seek(start_x, start_y);
            return false;
        }

        const uint32_t run = std::min(remaining, width - x);
        if (!seek(x, y)) {
            if (!advance) seek(start_x, start_y);
            return false;
        }

        const size_t bytes = static_cast<size_t>(run) * byte_per_px_;
        if (scratch_.size() < bytes) scratch_.resize(bytes);
        if (!read_at(file_pos_, scratch_.data(), bytes)) {
            if (!advance) seek(start_x, start_y);
            return false;
        }

        for (uint32_t i = 0; i < run; ++i)
            out[i] = decode_px(scratch_.data() + static_cast<size_t>(i) * byte_per_px_);

        out += run;
        remaining -= run;
        x += run;
        if (x >= width) {
            x = 0;
            ++y;
        }
    }

    if (advance) {
        seek(start_x, start_y);
        advance_px(count);
    } else {
        seek(start_x, start_y);
    }
    return true;
}

bool BmpFile::write_next_px(ui::Color px) {
    if (!is_opened_ || is_read_only_ || at_end_) return false;

    uint8_t raw[4]{};
    if (!encode_px(px, raw)) return false;
    if (!write_at(file_pos_, raw, byte_per_px_)) return false;

    advance_px(1);
    return true;
}

void BmpFile::set_bg_color(ui::Color background) {
    bg_ = background;
    use_bg_ = true;
}

void BmpFile::delete_bg_color() {
    use_bg_ = false;
}

bool BmpFile::fill_rows(uint32_t first_row, uint32_t row_count) {
    if (row_count == 0) return true;

    /* Whole rows including their padding, so the file has no undefined bytes.
     * Upstream leaves the growth as whatever was on the medium unless a
     * background colour was set. */
    std::vector<uint8_t> row(byte_per_row_, 0);

    if (use_bg_) {
        uint8_t raw[4]{};
        if (encode_px(bg_, raw)) {
            for (uint32_t x = 0; x < header_.width; ++x)
                std::memcpy(row.data() + static_cast<size_t>(x) * byte_per_px_, raw, byte_per_px_);
        }
    }

    uint64_t pos = static_cast<uint64_t>(header_.image_data) +
                   static_cast<uint64_t>(first_row) * byte_per_row_;
    for (uint32_t i = 0; i < row_count; ++i) {
        if (!write_at(pos, row.data(), row.size())) return false;
        pos += byte_per_row_;
    }
    return true;
}

bool BmpFile::expand_y(uint32_t new_y) {
    if (!is_opened_) return false;
    if (is_read_only_) return false;

    const uint32_t old_height = get_real_height();
    if (new_y <= old_height) return true;
    if (new_y > 0x7fffffffu) return false; /* height is a signed 32-bit field */

    /* Upstream stamps a negative height on whatever it grows, which silently
     * turns a bottom-up file top-down and reverses every row already in it.
     * Refuse instead — nothing upstream grows a bottom-up file on purpose. */
    if (old_height > 0 && is_bottomup()) return false;

    const uint32_t delta_rows = new_y - old_height;
    if (delta_rows > (0xffffffffu - header_.size) / byte_per_row_) return false;

    const uint32_t delta = delta_rows * byte_per_row_;
    header_.size += delta;
    header_.data_size += delta;
    /* Negative: stored top-down, so growth appends rows at the bottom of the
     * image and nothing already written moves. */
    header_.height = -static_cast<int32_t>(new_y);

    if (!write_header()) return false;
    if (!fill_rows(old_height, delta_rows)) return false;

    at_end_ = false;
    return seek(0, old_height);
}

bool BmpFile::expand_y_delta(uint32_t delta_y) {
    if (!is_opened_) return false;
    return expand_y(get_real_height() + delta_y);
}

/* -------------------------------------------------------------------------
 * Whole-image helpers
 * ---------------------------------------------------------------------- */

bool load_bmp_rgb565(const std::string& path,
                     std::vector<ui::Color>& out,
                     uint32_t& width,
                     uint32_t& height) {
    BmpFile bmp;
    if (!bmp.open(path, true)) return false;

    const uint32_t w = bmp.get_width();
    const uint32_t h = bmp.get_real_height();
    if (w == 0 || h == 0) return false;
    if (h > (0xffffffffu / w)) return false;

    out.assign(static_cast<size_t>(w) * h, ui::Color{});

    for (uint32_t y = 0; y < h; ++y) {
        if (!bmp.seek(0, y)) return false;
        if (!bmp.read_next_px_cnt(out.data() + static_cast<size_t>(y) * w, w, false))
            return false;
    }

    width = w;
    height = h;
    return true;
}

bool save_bmp_rgb565(const std::string& path,
                     const ui::Color* pixels,
                     uint32_t width,
                     uint32_t height) {
    if (pixels == nullptr || width == 0 || height == 0) return false;

    BmpFile bmp;
    if (!bmp.create(path, width, height)) return false;

    for (uint32_t y = 0; y < height; ++y) {
        if (!bmp.seek(0, y)) return false;
        for (uint32_t x = 0; x < width; ++x) {
            if (!bmp.write_next_px(pixels[static_cast<size_t>(y) * width + x]))
                return false;
        }
    }

    bmp.close();
    return true;
}

}  // namespace core
