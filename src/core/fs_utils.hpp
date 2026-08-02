/*
 * mayhem-b200 — filesystem helpers for the file manager, text editor and any
 * app that browses files.
 *
 * This is the host replacement for the firmware's FatFs wrapper
 * (application/file.cpp, application/file_reader.hpp) and for the directory
 * handling in application/apps/ui_fileman.cpp. The firmware talks to an SD
 * card through FatFs and reports FRESULT codes; here everything goes through
 * std::filesystem and every call reports an FsResult instead of throwing, so
 * a view can put the message on screen.
 *
 * Deliberate deviations from upstream, all for host safety:
 *
 *  - delete_path()/delete_recursive() refuse any target outside
 *    core::data_directory() unless the caller passes DeleteScope::Anywhere.
 *    The firmware runs against a removable SD card; this program runs against
 *    a real user profile, where an app-triggered recursive delete is a serious
 *    hazard. The guard also refuses a filesystem root and the data directory
 *    itself, in both scopes.
 *  - create_file() fails when the target exists. Upstream's make_new_file()
 *    opens with FA_CREATE_ALWAYS, so the file manager's "New file" button
 *    silently truncates a file of the same name. Use write_file() when
 *    replacing content is what you actually mean.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc. (original semantics)
 * Copyright (C) 2023 Kyle Reed (line reader semantics)
 * Copyright (C) 2026 mayhem-b200 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version. See LICENSE.GPL-2.0-or-later.
 */

#ifndef __MB200_FS_UTILS_H__
#define __MB200_FS_UTILS_H__

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace core {

/* Outcome of a filesystem operation. Never an exception: these run from UI
 * callbacks, and a view wants a string it can display. */
struct FsResult {
    bool ok{true};
    /* Human-readable reason, empty when ok. Includes the offending path. */
    std::string message{};

    explicit operator bool() const noexcept { return ok; }
};

/* One line of a directory listing. `name` is the leaf name only, not a path —
 * the same thing upstream's fileman_entry::path holds. */
struct DirEntry {
    std::string name{};
    bool is_directory{false};
    /* Bytes. Always 0 for directories, as upstream reports them. */
    std::uintmax_t size{0};
    /* Last modification, seconds since the Unix epoch; 0 if unavailable. */
    std::time_t modified{0};
};

/* Filtering for list_directory(). Implicitly constructible from a single
 * extension so the common case reads list_directory(dir, out, ".TXT"). */
struct ListOptions {
    ListOptions() = default;
    ListOptions(const char* extension);       /* implicit */
    ListOptions(std::string_view extension);  /* implicit */

    /* Extensions to keep, e.g. {".TXT"} or {".C8", ".CU8", ".C16"}. Empty
     * keeps every file. Matching is case-insensitive as upstream's
     * path_iequal() is, and a leading '.' is optional. Directories are never
     * filtered by extension — the file manager still has to navigate. */
    std::vector<std::string> extensions{};

    /* Upstream hides names starting with '.'; so does this, by default.
     * NB: the Windows "hidden" file attribute is not consulted — that needs
     * Win32 and std::filesystem does not expose it. */
    bool include_hidden{false};

    bool include_directories{true};
};

/* Whether a delete may touch paths outside core::data_directory(). */
enum class DeleteScope : uint8_t {
    /* Default everywhere in the app. */
    DataDirectoryOnly,
    /* The caller has taken responsibility for the path. */
    Anywhere
};

/* --- Queries. None of these throw; they answer false/0 on error. --------- */

bool exists(const std::string& path);
bool is_directory(const std::string& path);
bool is_regular_file(const std::string& path);
/* True only for a directory that exists and holds nothing. */
bool is_empty_directory(const std::string& path);
/* Size in bytes, or 0 when the path is not a readable regular file. */
std::uintmax_t file_size(const std::string& path);
/* Entries in a directory, files and subdirectories alike (upstream's
 * file_count). Returns -1 when the directory cannot be read, so callers can
 * tell "empty" from "unreadable". */
int file_count(const std::string& directory);

/* Lists `directory` into `out` (cleared first), sorted directories-first and
 * then by name. Name ordering is case-insensitive ASCII with a case-sensitive
 * tiebreak, which keeps the order total and matches what the firmware shows
 * for the uppercase 8.3-style names FAT hands back. */
FsResult list_directory(const std::string& directory,
                        std::vector<DirEntry>& out,
                        const ListOptions& options = {});

/* --- Mutations ----------------------------------------------------------- */

/* Creates an empty file. Fails if anything already exists at `path`. */
FsResult create_file(const std::string& path);

/* Creates one directory level; the parent must already exist. For the
 * recursive form use core::ensure_directory() from file_path.hpp. */
FsResult create_directory(const std::string& path);

/* Rename or relocate. Fails if `to` exists. Same operation as move_path()
 * without overwrite — both names exist because the file manager speaks both. */
FsResult rename_path(const std::string& from, const std::string& to);

/* Moves a file or a directory tree. Falls back to copy-then-delete when the
 * two ends are on different volumes, which std::filesystem::rename cannot do.
 * NB: unlike delete_path(), this is not scope-guarded — the caller named both
 * ends explicitly. */
FsResult move_path(const std::string& from, const std::string& to,
                   bool overwrite = false);

/* Copies a file, or a directory recursively. Refuses to copy a directory into
 * itself. */
FsResult copy_path(const std::string& from, const std::string& to,
                   bool overwrite = false);

/* Answers "would a delete of this path be allowed?" without touching the disk.
 * delete_path() and delete_recursive() both run exactly this check first. Ask
 * it before offering a Delete button, and — this matters — use it instead of a
 * real delete when testing the guard, so a broken guard cannot turn a test
 * into a recursive delete of something real. */
FsResult may_delete(const std::string& path,
                    DeleteScope scope = DeleteScope::DataDirectoryOnly);

/* Deletes a file or an *empty* directory. Reports an error if `path` does not
 * exist. Guarded — see DeleteScope and the header comment. */
FsResult delete_path(const std::string& path,
                     DeleteScope scope = DeleteScope::DataDirectoryOnly);

/* Deletes a directory and everything under it (or a single file). Same guard. */
FsResult delete_recursive(const std::string& path,
                          DeleteScope scope = DeleteScope::DataDirectoryOnly);

/* True if `path` is `root` or lies under it, after normalising "." and ".."
 * and comparing case-insensitively on Windows. Non-existent paths are fine:
 * the test is lexical once the existing prefix has been resolved. */
bool is_within_directory(const std::string& path, const std::string& root);
bool is_within_data_directory(const std::string& path);

/* --- Path helpers -------------------------------------------------------- */
/* These return the generic form, with '/' separators, matching upstream's
 * path convention. Windows accepts '/' everywhere std::filesystem does. */

std::string path_join(std::string_view base, std::string_view leaf);
std::string parent_path(std::string_view path);
std::string filename(std::string_view path);
/* Filename without the extension: "LOG_001.TXT" -> "LOG_001". */
std::string stem(std::string_view path);
/* Extension including the dot: "LOG_001.TXT" -> ".TXT"; "" when there is none. */
std::string extension(std::string_view path);
/* "a/b.txt", ".c8" -> "a/b.c8". An empty replacement strips the extension.
 * The leading '.' of the replacement is optional. */
std::string replace_extension(std::string_view path,
                              std::string_view new_extension);

/* Returns `name`, or `name` with "_1", "_2", ... inserted before the extension
 * until nothing of that name exists in `directory` — upstream's
 * get_unique_filename() from ui_fileman.cpp. Returns the leaf name only, not a
 * path. Returns an empty string in the pathological case where no free name is
 * found within 100000 tries. */
std::string unique_filename(std::string_view directory, std::string_view name);

/* --- Whole-file and line-oriented text I/O ------------------------------- */

/* Reads the whole file into `out` verbatim, bytes unchanged. Intended for the
 * small config/playlist/freqman files the apps deal in. */
FsResult read_file(const std::string& path, std::string& out);

/* Writes `content`, creating or truncating. */
FsResult write_file(const std::string& path, std::string_view content);

/* Splits on '\n' with a trailing '\r' stripped, so both LF and CRLF files
 * read the same. A final line with no terminator is still returned; a file
 * that ends with a newline does not yield a trailing empty line. An empty
 * file yields no lines. */
FsResult read_lines(const std::string& path, std::vector<std::string>& out);

/* Same splitting, streamed a line at a time so the text editor and freqman
 * need not hold a whole file. Return false from `callback` to stop early;
 * stopping early is not an error. */
FsResult for_each_line(const std::string& path,
                       const std::function<bool(std::string_view line)>& callback);

}  // namespace core

#endif /*__MB200_FS_UTILS_H__*/
