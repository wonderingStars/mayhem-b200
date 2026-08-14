/*
 * mayhem-b200 — shutdown request flag. See quit.hpp.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "quit.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace core {

namespace {

/* Plain atomic, not guarded by the mutex: request_quit() can be called from a
 * Win32 control handler, which runs on a thread the OS injects, and taking a
 * lock there would risk a deadlock against whatever the main thread holds. */
std::atomic<bool> g_quit_requested{false};

std::mutex g_mutex;
std::condition_variable g_done_cv;
bool g_shutdown_complete = false;

}  // namespace

void request_quit() {
    g_quit_requested.store(true, std::memory_order_release);
}

bool quit_requested() {
    return g_quit_requested.load(std::memory_order_acquire);
}

void notify_shutdown_complete() {
    {
        std::lock_guard<std::mutex> g{g_mutex};
        g_shutdown_complete = true;
    }
    g_done_cv.notify_all();
}

bool wait_for_shutdown_complete(int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = 0;
    std::unique_lock<std::mutex> lock{g_mutex};
    return g_done_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                              [] { return g_shutdown_complete; });
}

void reset_quit_state_for_test() {
    g_quit_requested.store(false, std::memory_order_release);
    std::lock_guard<std::mutex> g{g_mutex};
    g_shutdown_complete = false;
}

}  // namespace core
