/*
 * mayhem-b200 — filesystem helpers.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "fs_utils.hpp"

#include "file_path.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <cwctype>
#endif

namespace fs = std::filesystem;

namespace core {

namespace {

FsResult fail(std::string message) {
    return FsResult{false, std::move(message)};
}

FsResult fail_ec(std::string_view what, const std::string& path,
                 const std::error_code& ec) {
    return fail(std::string{what} + " '" + path + "': " + ec.message());
}

/* std::filesystem::file_time_type has no portable conversion to time_t before
 * C++20's clock_cast, and MSVC's clock_cast route goes through utc_clock,
 * which wants the leap-second database. The clock offset is stable, so
 * measuring it once per call is both cheap and dependency-free; the two now()
 * reads make this accurate to well under a millisecond, which is far more than
 * a file listing needs. */
std::time_t to_time_t(fs::file_time_type t) {
    const auto file_now = fs::file_time_type::clock::now();
    const auto sys_now = std::chrono::system_clock::now();
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - file_now + sys_now);
    return std::chrono::system_clock::to_time_t(sys);
}

char ascii_upper(char c) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
}

/* Case-insensitive ASCII compare, <0 / 0 / >0 like strcmp. Upstream's
 * path_iequal() is equally ASCII-only ("NB: Not correct for Unicode/locales"). */
int compare_ci(std::string_view a, std::string_view b) {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const char ca = ascii_upper(a[i]);
        const char cb = ascii_upper(b[i]);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    if (a.size() == b.size()) return 0;
    return a.size() < b.size() ? -1 : 1;
}

bool equals_ci(std::string_view a, std::string_view b) {
    return a.size() == b.size() && compare_ci(a, b) == 0;
}

/* Absolute, with "." and ".." resolved and no trailing separator, so two paths
 * can be compared component by component. Falls back to a purely lexical
 * normalisation when the path cannot be resolved against the disk. */
fs::path normalize(const fs::path& in) {
    std::error_code ec;

    fs::path result = fs::absolute(in, ec);
    if (ec) result = in;

    ec.clear();
    if (fs::path resolved = fs::weakly_canonical(result, ec); !ec)
        result = resolved;

    result = result.lexically_normal();
    result.make_preferred();

    /* "C:/a/" iterates as {"C:", "/", "a", ""}; drop that empty tail. */
    if (result.has_relative_path() && result.filename().empty())
        result = result.parent_path();

    return result;
}

bool component_equal(const fs::path& a, const fs::path& b) {
#if defined(_WIN32)
    const std::wstring& x = a.native();
    const std::wstring& y = b.native();
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i)
        if (std::towupper(x[i]) != std::towupper(y[i])) return false;
    return true;
#else
    return a.native() == b.native();
#endif
}

bool path_within(const fs::path& path, const fs::path& root) {
    if (root.empty()) return false;

    auto pi = path.begin();
    const auto pe = path.end();

    for (auto ri = root.begin(), re = root.end(); ri != re; ++ri, ++pi) {
        if (pi == pe) return false;
        if (!component_equal(*pi, *ri)) return false;
    }
    return true;
}

/* An extension filter matches with or without its leading dot, and without
 * regard to case — the same latitude upstream's path_iequal() gives. */
bool extension_matches(std::string_view ext, const std::vector<std::string>& filters) {
    if (filters.empty()) return true;

    for (const auto& raw : filters) {
        if (raw.empty()) continue;

        std::string_view want{raw};
        std::string_view have{ext};

        if (want.front() == '.')
            want.remove_prefix(1);
        if (!have.empty() && have.front() == '.')
            have.remove_prefix(1);

        if (equals_ci(have, want)) return true;
    }
    return false;
}

bool entry_less(const DirEntry& a, const DirEntry& b) {
    if (a.is_directory != b.is_directory) return a.is_directory;

    const int c = compare_ci(a.name, b.name);
    if (c != 0) return c < 0;

    /* Names differing only in case still need a total order. */
    return a.name < b.name;
}

/* Deletes without consulting the guard; every caller has already run it. */
FsResult remove_unchecked(const fs::path& p, bool recursive) {
    std::error_code ec;

    if (recursive) {
        fs::remove_all(p, ec);
        if (ec) return fail_ec("delete failed", p.string(), ec);
        return FsResult{};
    }

    if (!fs::remove(p, ec) || ec) {
        if (!ec) return fail("delete failed '" + p.string() + "': nothing was removed");
        return fail_ec("delete failed", p.string(), ec);
    }
    return FsResult{};
}

}  // namespace

/* --- ListOptions --------------------------------------------------------- */

ListOptions::ListOptions(const char* extension) {
    if (extension != nullptr && *extension != '\0') extensions.emplace_back(extension);
}

ListOptions::ListOptions(std::string_view extension) {
    if (!extension.empty()) extensions.emplace_back(extension);
}

/* --- Queries ------------------------------------------------------------- */

bool exists(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    const auto st = fs::symlink_status(fs::path{path}, ec);
    if (ec) return false;
    return fs::exists(st);
}

bool is_directory(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::is_directory(fs::path{path}, ec);
}

bool is_regular_file(const std::string& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::is_regular_file(fs::path{path}, ec);
}

bool is_empty_directory(const std::string& path) {
    if (!is_directory(path)) return false;

    std::error_code ec;
    fs::directory_iterator it{fs::path{path}, ec};
    if (ec) return false;
    return it == fs::directory_iterator{};
}

std::uintmax_t file_size(const std::string& path) {
    std::error_code ec;
    const auto size = fs::file_size(fs::path{path}, ec);
    return ec ? 0u : size;
}

int file_count(const std::string& directory) {
    std::error_code ec;
    fs::directory_iterator it{fs::path{directory}, ec};
    if (ec) return -1;

    int count = 0;
    const fs::directory_iterator last{};
    for (; it != last; it.increment(ec)) {
        if (ec) return -1;
        ++count;
    }
    return count;
}

FsResult list_directory(const std::string& directory,
                        std::vector<DirEntry>& out,
                        const ListOptions& options) {
    out.clear();

    std::error_code ec;
    fs::directory_iterator it{fs::path{directory}, ec};
    if (ec) return fail_ec("cannot list", directory, ec);

    const fs::directory_iterator last{};
    for (; it != last; it.increment(ec)) {
        if (ec) return fail_ec("cannot list", directory, ec);

        const fs::directory_entry& e = *it;
        const std::string name = e.path().filename().string();
        if (name.empty()) continue;

        if (!options.include_hidden && name.front() == '.') continue;

        std::error_code kind_ec;
        const bool dir = e.is_directory(kind_ec);
        if (kind_ec) continue;  /* vanished or unreadable between calls */

        if (dir) {
            if (!options.include_directories) continue;
        } else {
            if (!extension_matches(e.path().extension().string(), options.extensions))
                continue;
        }

        DirEntry entry{};
        entry.name = name;
        entry.is_directory = dir;

        if (!dir) {
            std::error_code size_ec;
            const auto size = e.file_size(size_ec);
            entry.size = size_ec ? 0u : size;
        }

        std::error_code time_ec;
        const auto written = e.last_write_time(time_ec);
        entry.modified = time_ec ? std::time_t{0} : to_time_t(written);

        out.push_back(std::move(entry));
    }

    std::sort(out.begin(), out.end(), entry_less);
    return FsResult{};
}

/* --- Mutations ----------------------------------------------------------- */

FsResult create_file(const std::string& path) {
    if (path.empty()) return fail("cannot create a file with an empty name");

    if (exists(path))
        return fail("create failed '" + path + "': already exists");

    std::ofstream out{fs::path{path}, std::ios::binary | std::ios::out};
    if (!out)
        return fail("create failed '" + path + "': cannot open for writing");

    out.close();
    if (out.fail())
        return fail("create failed '" + path + "': write error");

    return FsResult{};
}

FsResult create_directory(const std::string& path) {
    if (path.empty()) return fail("cannot create a directory with an empty name");

    if (exists(path))
        return fail("create failed '" + path + "': already exists");

    std::error_code ec;
    if (!fs::create_directory(fs::path{path}, ec) || ec) {
        if (!ec) return fail("create failed '" + path + "': directory not created");
        return fail_ec("create failed", path, ec);
    }
    return FsResult{};
}

FsResult rename_path(const std::string& from, const std::string& to) {
    return move_path(from, to, /*overwrite*/ false);
}

FsResult move_path(const std::string& from, const std::string& to, bool overwrite) {
    if (from.empty() || to.empty())
        return fail("move failed: source and destination must both be named");

    if (!exists(from))
        return fail("move failed '" + from + "': does not exist");

    if (exists(to)) {
        if (!overwrite)
            return fail("move failed '" + to + "': already exists");

        const auto removed = remove_unchecked(fs::path{to}, /*recursive*/ true);
        if (!removed) return removed;
    }

    std::error_code ec;
    fs::rename(fs::path{from}, fs::path{to}, ec);
    if (!ec) return FsResult{};

    /* std::filesystem::rename cannot cross a volume boundary; copy and delete
     * is the only way there. Keep the original error if the copy fails too. */
    const auto copied = copy_path(from, to, /*overwrite*/ true);
    if (!copied)
        return fail_ec("move failed", from, ec);

    const auto removed = remove_unchecked(fs::path{from}, /*recursive*/ true);
    if (!removed)
        return fail("move '" + from + "' -> '" + to +
                    "': copied, but the source could not be removed (" +
                    removed.message + ")");

    return FsResult{};
}

FsResult copy_path(const std::string& from, const std::string& to, bool overwrite) {
    if (from.empty() || to.empty())
        return fail("copy failed: source and destination must both be named");

    if (!exists(from))
        return fail("copy failed '" + from + "': does not exist");

    if (exists(to) && !overwrite)
        return fail("copy failed '" + to + "': already exists");

    const bool from_is_dir = is_directory(from);

    if (from_is_dir && is_within_directory(to, from))
        return fail("copy failed '" + from + "': cannot copy a directory into itself");

    std::error_code ec;

    if (from_is_dir) {
        auto opts = fs::copy_options::recursive;
        if (overwrite) opts |= fs::copy_options::overwrite_existing;

        fs::copy(fs::path{from}, fs::path{to}, opts, ec);
    } else {
        const auto opts = overwrite ? fs::copy_options::overwrite_existing
                                    : fs::copy_options::none;
        fs::copy_file(fs::path{from}, fs::path{to}, opts, ec);
    }

    if (ec) return fail_ec("copy failed", from, ec);
    return FsResult{};
}

/* The whole point of this file: an app-triggered delete must not be able to
 * wander out of the data directory. The filesystem root and the data directory
 * itself are refused in *both* scopes — nothing an app does should remove
 * either. Pure: it looks at paths, never at content, and deletes nothing. */
FsResult may_delete(const std::string& path, DeleteScope scope) {
    if (path.empty()) return fail("refusing to delete an empty path");

    const fs::path norm = normalize(fs::path{path});

    if (!norm.has_relative_path())
        return fail("refusing to delete a filesystem root: '" + norm.string() + "'");

    const fs::path root = normalize(fs::path{data_directory()});

    /* Both sides are normalised and use the preferred separator, so a whole-
     * path case-insensitive compare is enough to spot "this is the root". */
    if (component_equal(norm, root))
        return fail("refusing to delete the data directory itself: '" +
                    norm.string() + "'");

    if (scope == DeleteScope::Anywhere) return FsResult{};

    if (!path_within(norm, root))
        return fail("refusing to delete outside the data directory ('" +
                    root.string() + "'): '" + norm.string() + "'");

    return FsResult{};
}

FsResult delete_path(const std::string& path, DeleteScope scope) {
    const auto allowed = may_delete(path, scope);
    if (!allowed) return allowed;

    if (!exists(path))
        return fail("delete failed '" + path + "': does not exist");

    if (is_directory(path) && !is_empty_directory(path))
        return fail("delete failed '" + path +
                    "': directory is not empty (use delete_recursive)");

    return remove_unchecked(fs::path{path}, /*recursive*/ false);
}

FsResult delete_recursive(const std::string& path, DeleteScope scope) {
    const auto allowed = may_delete(path, scope);
    if (!allowed) return allowed;

    if (!exists(path))
        return fail("delete failed '" + path + "': does not exist");

    return remove_unchecked(fs::path{path}, /*recursive*/ true);
}

bool is_within_directory(const std::string& path, const std::string& root) {
    if (path.empty() || root.empty()) return false;
    return path_within(normalize(fs::path{path}), normalize(fs::path{root}));
}

bool is_within_data_directory(const std::string& path) {
    return is_within_directory(path, data_directory());
}

/* --- Path helpers -------------------------------------------------------- */

std::string path_join(std::string_view base, std::string_view leaf) {
    if (base.empty()) return fs::path{leaf}.generic_string();
    if (leaf.empty()) return fs::path{base}.generic_string();

    return (fs::path{base} / fs::path{leaf}).generic_string();
}

std::string parent_path(std::string_view path) {
    return fs::path{path}.parent_path().generic_string();
}

std::string filename(std::string_view path) {
    return fs::path{path}.filename().generic_string();
}

std::string stem(std::string_view path) {
    return fs::path{path}.stem().generic_string();
}

std::string extension(std::string_view path) {
    return fs::path{path}.extension().generic_string();
}

std::string replace_extension(std::string_view path,
                              std::string_view new_extension) {
    fs::path p{path};

    if (new_extension.empty()) {
        p.replace_extension();
        return p.generic_string();
    }

    std::string ext{new_extension};
    if (ext.front() != '.') ext.insert(ext.begin(), '.');

    p.replace_extension(fs::path{ext});
    return p.generic_string();
}

std::string unique_filename(std::string_view directory, std::string_view name) {
    if (name.empty()) return {};

    const fs::path leaf{name};
    const std::string base = leaf.stem().generic_string();
    const std::string ext = leaf.extension().generic_string();

    std::string candidate{name};

    /* Upstream's get_unique_filename() counts from 1 and never gives up; a
     * host UI thread cannot afford an unbounded loop, so this stops. */
    constexpr int max_attempts = 100000;
    for (int serial = 1; serial <= max_attempts; ++serial) {
        if (!exists(path_join(directory, candidate))) return candidate;
        candidate = base + "_" + std::to_string(serial) + ext;
    }

    return {};
}

/* --- Text I/O ------------------------------------------------------------ */

FsResult read_file(const std::string& path, std::string& out) {
    out.clear();

    if (is_directory(path))
        return fail("read failed '" + path + "': is a directory");

    std::ifstream in{fs::path{path}, std::ios::binary};
    if (!in) return fail("read failed '" + path + "': cannot open");

    std::ostringstream ss;
    ss << in.rdbuf();
    if (in.bad()) return fail("read failed '" + path + "': read error");

    out = ss.str();
    return FsResult{};
}

FsResult write_file(const std::string& path, std::string_view content) {
    if (path.empty()) return fail("cannot write to an empty path");

    std::ofstream out{fs::path{path}, std::ios::binary | std::ios::trunc};
    if (!out) return fail("write failed '" + path + "': cannot open");

    if (!content.empty())
        out.write(content.data(), static_cast<std::streamsize>(content.size()));

    out.close();
    if (out.fail()) return fail("write failed '" + path + "': write error");

    return FsResult{};
}

FsResult for_each_line(const std::string& path,
                       const std::function<bool(std::string_view line)>& callback) {
    if (is_directory(path))
        return fail("read failed '" + path + "': is a directory");

    std::ifstream in{fs::path{path}, std::ios::binary};
    if (!in) return fail("read failed '" + path + "': cannot open");

    std::string line;
    while (std::getline(in, line)) {
        /* Binary mode keeps the '\r' of a CRLF file; drop it so a text editor
         * does not show a stray control character at the end of every line. */
        if (!line.empty() && line.back() == '\r') line.pop_back();

        if (callback && !callback(line)) return FsResult{};
    }

    if (in.bad()) return fail("read failed '" + path + "': read error");
    return FsResult{};
}

FsResult read_lines(const std::string& path, std::vector<std::string>& out) {
    out.clear();
    return for_each_line(path, [&out](std::string_view line) {
        out.emplace_back(line);
        return true;
    });
}

}  // namespace core
