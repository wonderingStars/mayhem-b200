/*
 * mayhem-b200 — the web portal's HTTP server.
 *
 * A small, dependency-free HTTP/1.1 server on raw sockets — Winsock on
 * Windows (the project already links ws2_32 for src/radio/network_radio.cpp's
 * sdrlink client), BSD sockets elsewhere; see the compatibility block at the
 * top of remote_server.cpp for the handful of primitives that differ.
 * Every request is served on its own short-lived thread and answered with
 * "Connection: close" — there is no keep-alive and no pipelining — which
 * keeps the parser in remote_server.cpp small at the cost of a new TCP
 * handshake per request. For a local control-panel API polled a few times a
 * second that cost is not worth the complexity of a persistent-connection
 * state machine.
 *
 * Routes (see remote_server.cpp's route() for the exact behaviour):
 *
 *   GET  /api/apps                  all registered apps, grouped by category
 *   GET  /api/apps/current          {id, title, panel_kind} of the open app
 *   POST /api/apps/{id}/launch      queue: push that app's view
 *   POST /api/apps/home             queue: pop to root
 *   GET  /api/panel                 current PanelData as JSON
 *   GET  /api/status                device label, receiving/transmitting, levels
 *   GET  /api/screen                one framebuffer frame (binary, see below)
 *   POST /api/input                 queue key/encoder/touch/char events
 *
 * Every handler reads through remote::AppBridge, never app::globals()
 * directly — see app_bridge.hpp's file header for the thread-safety
 * contract that rests on.
 *
 * GET /api/screen is the exception to "no long-lived requests": with
 * ?after=SEQ&wait_ms=MS it holds the connection open until a frame newer than
 * SEQ exists or MS milliseconds (capped at 10000) pass, answering 204 on
 * timeout and 204 when nothing has been captured at all yet. Blocking is
 * affordable because each connection already owns its thread; stop() releases
 * anyone still waiting so shutdown does not stall behind a 10-second poll.
 * The 200 body is application/octet-stream with a 16-byte header — the layout
 * is written out once, next to the constants that encode it, in
 * app_bridge.hpp.
 *
 * POST /api/input is remote CONTROL, not just remote viewing: an event posted
 * here reaches app::EventDispatcher exactly as a local keypress does, so a
 * client on the LAN can drive any app, transmit apps included. That is the
 * same trust boundary as the rest of this server (see start()), but it is a
 * materially larger consequence of it.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_REMOTE_SERVER_H__
#define __MB200_REMOTE_SERVER_H__

#if defined(_WIN32)

/* WIN32_LEAN_AND_MEAN (a project-wide compile definition, see CMakeLists.txt
 * and scripts/checkfile.cmd) keeps <windows.h> from pulling in the original
 * <winsock.h>, so it does not matter whether this header is included before
 * or after <windows.h> elsewhere in a translation unit. */
#include <winsock2.h>

/* winsock2.h pulls in <windows.h>, whose <wingdi.h> defines a GDI
 * RGB(r,g,b) macro — WIN32_LEAN_AND_MEAN does not exclude GDI. That macro
 * collides with ui::Color::RGB(uint32_t) (src/ui/ui.hpp) wherever a
 * translation unit includes both this header and ui.hpp, in either order.
 * Undefined immediately so it never reaches a header included after this
 * one (ui.hpp, transitively, through app_bridge.hpp/app_registry.hpp). */
#ifdef RGB
#undef RGB
#endif

#endif /* _WIN32 */

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace remote {

/* A listening socket handle. Winsock's SOCKET is an unsigned handle with its
 * own INVALID_SOCKET sentinel; a BSD socket is a plain file descriptor with -1.
 * Naming the difference here keeps the member below, and the accept loop in
 * remote_server.cpp, written once for both. No POSIX socket header is included
 * from this header — remote_server.cpp is the only place that calls into the
 * socket API, and pulling <sys/socket.h> into every translation unit that
 * includes this one would be gratuitous. */
#if defined(_WIN32)
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
#endif

class RemoteServer {
   public:
    RemoteServer();
    ~RemoteServer();

    RemoteServer(const RemoteServer&) = delete;
    RemoteServer& operator=(const RemoteServer&) = delete;

    /* Starts listening on 0.0.0.0:port (port 0 asks the OS for a free port;
     * see port() for what it actually picked) and spawns the accept-loop
     * thread. Returns false and sets last_error() on failure. No-op (returns
     * false) if already running.
     *
     * There is no authentication, and since POST /api/input landed this is
     * full remote control of the radio — transmit apps included — not a
     * read-only view of it. Do not expose the port past a trusted network.
     *
     * It is weaker than "a terminal on this machine", which is how this
     * paragraph used to describe it, and the difference matters because it
     * is not about the network at all. read_request() consults only
     * Content-Length and ignores Content-Type, and every response carries
     * Access-Control-Allow-Origin: *. A POST with a CORS-"simple" content
     * type therefore needs no preflight, so ANY page the operator's browser
     * happens to load can drive this API at 127.0.0.1 without the port being
     * reachable from anywhere. (Chrome's private-network-access preflight
     * blunts the public-page->loopback case on recent versions; a page served
     * from the LAN, or from localhost, has no such barrier.) CORS stops that
     * page READING the reply; it does not stop the key press landing.
     *
     * Closing that would mean requiring a preflight-triggering header on
     * /api/input, which contract 2 does not specify — deliberately left
     * as a documented exposure rather than changed unilaterally. */
    bool start(uint16_t port);

    /* Stops accepting new connections and joins the accept thread. Requests
     * already being handled on their own worker threads are left to finish
     * on their own (each has a bounded receive timeout, see
     * remote_server.cpp, so none can hang forever); a /api/screen long poll
     * is additionally woken here rather than left to run out its wait. Safe
     * to call when not running. */
    void stop();

    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }
    const std::string& last_error() const { return last_error_; }

   private:
    void accept_loop();

    std::atomic<bool> running_{false};
    socket_t listen_socket_{kInvalidSocket};
    uint16_t port_{0};
    std::string last_error_{};
    std::thread accept_thread_{};
};

}  // namespace remote

#endif /*__MB200_REMOTE_SERVER_H__*/
