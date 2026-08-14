/*
 * mayhem-b200 — the shutdown request path (src/core/quit.hpp).
 *
 * This is the safety valve: without it the ONLY way to stop the program was
 * closing its window, so Ctrl+C, closing the console, a Windows logoff, and a
 * --hidden run with no window at all were all hard kills that skipped main()'s
 * teardown — the code that ends the transmit burst, stops the streams and
 * releases the B200. What must never regress: a request is visible to the main
 * loop, and a caller that waits for the teardown actually blocks until it is
 * reported done (the console control handler returns as soon as this wait
 * returns, and Windows kills the process the moment it does).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "quit.hpp"

#include <atomic>
#include <chrono>
#include <thread>

TEST(quit_is_not_requested_until_something_asks) {
    core::reset_quit_state_for_test();
    CHECK(!core::quit_requested());
}

TEST(request_quit_is_visible_to_the_main_loop) {
    core::reset_quit_state_for_test();
    core::request_quit();
    CHECK(core::quit_requested());
}

TEST(request_quit_is_idempotent) {
    core::reset_quit_state_for_test();
    core::request_quit();
    core::request_quit();
    core::request_quit();
    CHECK(core::quit_requested());
}

TEST(a_quit_requested_from_another_thread_is_seen) {
    /* The real caller is a Win32 control handler, which runs on a thread the
     * OS injects — never the main thread. */
    core::reset_quit_state_for_test();
    std::thread t{[] { core::request_quit(); }};
    t.join();
    CHECK(core::quit_requested());
}

TEST(waiting_for_shutdown_times_out_when_it_never_completes) {
    /* The handler must not block forever if the main loop is wedged: Windows
     * gives it only a few seconds before killing the process regardless. */
    core::reset_quit_state_for_test();
    const auto start = std::chrono::steady_clock::now();
    const bool completed = core::wait_for_shutdown_complete(50);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(!completed);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() >= 40);
}

TEST(waiting_for_shutdown_returns_once_teardown_reports_done) {
    core::reset_quit_state_for_test();
    std::atomic<bool> waiter_returned{false};
    std::thread waiter{[&] {
        /* Generous timeout: we are asserting it returns EARLY, on the signal. */
        if (core::wait_for_shutdown_complete(5000)) waiter_returned.store(true);
    }};

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    core::notify_shutdown_complete();  /* what main() calls after the teardown */
    waiter.join();

    CHECK(waiter_returned.load());
}

TEST(waiting_after_shutdown_already_completed_returns_at_once) {
    /* Ordering hazard: the control handler can arrive after main() has already
     * finished tearing down. It must not then block for the full timeout. */
    core::reset_quit_state_for_test();
    core::notify_shutdown_complete();
    const auto start = std::chrono::steady_clock::now();
    const bool completed = core::wait_for_shutdown_complete(5000);
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(completed);
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 1000);
}
