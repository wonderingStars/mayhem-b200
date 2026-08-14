/*
 * mayhem-b200 — the one place a shutdown is requested from.
 *
 * Until this existed the ONLY way to stop the program was closing its window
 * (WM_CLOSE -> Event::Type::Quit -> the main loop breaks -> main()'s cleanup
 * sequence runs). Everything else — Ctrl+C, closing the console, Windows
 * logging off or shutting down, a hidden-window build with no window to close,
 * Task Manager — killed the process outright, which matters here because the
 * teardown is what stops the TX stream with an end-of-burst, stops the RX
 * stream, drops the UHD handle and releases the USB device. None of that runs
 * on a TerminateProcess, so a hard kill can leave a transmitter mid-burst and
 * the B200 claimed until the OS reaps the endpoint.
 *
 * So: any thread may ask for a quit, the main loop notices and runs the SAME
 * cleanup it always did, and a caller that needs the teardown to have actually
 * finished (the console control handler — Windows gives it only a few seconds
 * before killing the process anyway) can block on it.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MB200_CORE_QUIT_HPP
#define MB200_CORE_QUIT_HPP

namespace core {

/* Ask the main loop to exit and run its normal cleanup. Thread-safe,
 * idempotent, and safe to call from a Win32 control handler. Never blocks. */
void request_quit();

/* True once request_quit() has been called. The main loop polls this. */
bool quit_requested();

/* Called by main() once the teardown sequence has finished. */
void notify_shutdown_complete();

/* Blocks until notify_shutdown_complete() or the timeout. Returns true if the
 * shutdown actually completed. Used by the console control handler so the
 * process does not die before the radio is idled. */
bool wait_for_shutdown_complete(int timeout_ms);

/* Test seam: forget both flags. */
void reset_quit_state_for_test();

}  // namespace core

#endif  // MB200_CORE_QUIT_HPP
