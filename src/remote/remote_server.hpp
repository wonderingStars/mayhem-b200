/*
 * mayhem-b200 — the web portal's HTTP server.
 *
 * A small, dependency-free HTTP/1.1 server on raw Winsock (the project
 * already links ws2_32 for src/radio/network_radio.cpp's sdrlink client).
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
 *
 * Every handler reads through remote::AppBridge, never app::globals()
 * directly — see app_bridge.hpp's file header for the thread-safety
 * contract that rests on.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_REMOTE_SERVER_H__
#define __MB200_REMOTE_SERVER_H__

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

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

namespace remote {

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
     * There is no authentication: this is a LAN control surface for the
     * radio sitting next to it, the same trust boundary as opening a
     * terminal on the machine running mayhem-b200. Do not expose the port
     * past a trusted network. */
    bool start(uint16_t port);

    /* Stops accepting new connections and joins the accept thread. Requests
     * already being handled on their own worker threads are left to finish
     * on their own (each has a bounded receive timeout, see
     * remote_server.cpp, so none can hang forever). Safe to call when not
     * running. */
    void stop();

    bool running() const { return running_.load(); }
    uint16_t port() const { return port_; }
    const std::string& last_error() const { return last_error_; }

   private:
    void accept_loop();

    std::atomic<bool> running_{false};
    SOCKET listen_socket_{INVALID_SOCKET};
    uint16_t port_{0};
    std::string last_error_{};
    std::thread accept_thread_{};
};

}  // namespace remote

#endif /*__MB200_REMOTE_SERVER_H__*/
