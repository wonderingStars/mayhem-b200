/*
 * mayhem-b200 — where the app keeps its files.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "file_path.hpp"

#include <cstdlib>
#include <filesystem>

namespace core {

namespace {

std::string home_directory() {
#if defined(_WIN32)
    /* USERPROFILE is set for interactive sessions; HOMEDRIVE+HOMEPATH is the
     * fallback on older or service contexts. */
    if (const char* profile = std::getenv("USERPROFILE"); profile != nullptr)
        return profile;

    const char* drive = std::getenv("HOMEDRIVE");
    const char* path = std::getenv("HOMEPATH");
    if (drive != nullptr && path != nullptr) return std::string{drive} + path;
#else
    if (const char* home = std::getenv("HOME"); home != nullptr) return home;
#endif
    return {};
}

}  // namespace

std::string data_directory() {
    const auto home = home_directory();
    if (home.empty()) return ".";

    std::filesystem::path p{home};
    const auto documents = p / "Documents";

    /* Prefer Documents when it exists; otherwise sit directly in the profile. */
    std::error_code ec;
    if (std::filesystem::is_directory(documents, ec))
        return (documents / "mayhem-b200").string();

    return (p / "mayhem-b200").string();
}

std::string captures_directory() {
    return (std::filesystem::path{data_directory()} / "CAPTURES").string();
}

std::string freqman_directory() {
    return (std::filesystem::path{data_directory()} / "FREQMAN").string();
}

bool ensure_directory(const std::string& path) {
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) return true;

    std::filesystem::create_directories(path, ec);
    return std::filesystem::is_directory(path, ec);
}

}  // namespace core
