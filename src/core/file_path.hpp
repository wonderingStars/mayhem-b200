/*
 * mayhem-b200 — where the app keeps its files.
 *
 * The firmware has an SD card with a fixed layout (/CAPTURES, /FREQMAN, ...).
 * On the host the equivalent lives under the user's Documents folder so
 * recordings do not land in the build tree.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_FILE_PATH_H__
#define __MB200_FILE_PATH_H__

#include <string>

namespace core {

/* Root of the app's data, e.g. C:/Users/<name>/Documents/mayhem-b200.
 * Falls back to the current directory if the home folder cannot be resolved. */
std::string data_directory();

std::string captures_directory();  /* <data>/CAPTURES */
std::string freqman_directory();   /* <data>/FREQMAN  */

/* Creates `path` and any missing parents. Returns true if it exists afterwards. */
bool ensure_directory(const std::string& path);

}  // namespace core

#endif /*__MB200_FILE_PATH_H__*/
