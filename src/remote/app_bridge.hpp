/*
 * mayhem-b200 — the seam between a running app and the web portal.
 *
 * Apps compute structured results already (ui::RecentEntriesTable rows, FFT
 * bins, radio::ReceiverModel state); this file lets that data be *read*
 * without changing how any app produces it. Nothing here rewrites, ports or
 * reimplements a decoder — see app_data.hpp for the shapes and
 * remote_server.hpp for what turns them into HTTP responses.
 *
 * THREADING. NavigationView defers push()/pop() to service(), which only the
 * UI thread calls — pushing or popping from any other thread races the view
 * stack and (per its own comment) can destroy an object whose method is
 * currently executing. This class extends that rule to *all* app-facing
 * state, not just push/pop:
 *
 *   - request_launch()/request_home() (any thread, typically the HTTP
 *     server's) only ever enqueue a request. drain_launch_queue() (UI thread
 *     only) is what actually calls NavigationView::push()/pop_to_root().
 *   - Panel providers (receiver, spectrum, and any an app opts into) are
 *     invoked ONLY from refresh() (UI thread only, once per frame), which
 *     then caches the resulting PanelData/status behind a mutex. The HTTP
 *     handlers (apps_json/current_app_json/panel_json/status_json) never
 *     call a provider or touch app::globals().nav themselves — they only
 *     ever read the cache. That is what makes it safe to call them from a
 *     connection-handling thread while the UI thread is mid-frame.
 *   - The screen stream follows the same split. capture_screen_frame() (UI
 *     thread only) reads host::display and publishes an RGB565 snapshot
 *     behind frame_mutex_; screen_frame() (any thread) only ever reads that
 *     snapshot. Remote input is the launch queue's twin: queue_input() from
 *     any thread, drain_input_queue() on the UI thread, which is the only
 *     place a host::Event ever reaches app::EventDispatcher.
 *
 * main.cpp is expected to call drain_launch_queue() immediately before
 * NavigationView::service(), refresh() once per frame after that (see its own
 * comment for the exact placement and why), drain_input_queue() alongside the
 * window's own event pump, and capture_screen_frame() after present().
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_REMOTE_APP_BRIDGE_H__
#define __MB200_REMOTE_APP_BRIDGE_H__

#include "app_data.hpp"

#include "../ui/ui_recent_entries.hpp"
#include "../ui/window.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui {
class View;
}  // namespace ui

namespace remote {

/* --- Screen frames (GET /api/screen) -------------------------------------
 *
 * The wire layout of one frame, fixed by the portal protocol. Named here
 * rather than spelled out in remote_server.cpp so the encoder and the tests
 * that pin it byte-for-byte read the same constants:
 *
 *   offset size field
 *   0      4    magic "MBSF"
 *   4      1    version = 1
 *   5      1    format  = 1 (raw RGB565, little-endian)
 *   6      2    width   u16 LE
 *   8      2    height  u16 LE
 *   10     4    seq     u32 LE
 *   14     2    reserved = 0
 *   16     w*h*2 payload, rows top to bottom
 */
inline constexpr char kScreenFrameMagic[4] = {'M', 'B', 'S', 'F'};
inline constexpr uint8_t kScreenFrameVersion = 1;
inline constexpr uint8_t kScreenFormatRgb565 = 1;
inline constexpr size_t kScreenFrameHeaderBytes = 16;

/* --- Remote input (POST /api/input) --------------------------------------
 *
 * One queued remote action, in the same vocabulary both window layers use.
 * A key PRESS is note_key_down() followed by a dispatched Event; a key
 * RELEASE is note_key_up() and NO Event at all — window.cpp's WM_KEYUP and
 * window_linux.cpp's KeyRelease both do exactly that, and copying it is what
 * keeps ui::key_is_long_pressed() honest for a remote client instead of
 * leaving every remote key latched down forever. Everything else (encoder,
 * touch, character) is just an Event to dispatch.
 *
 * Type::Quit is deliberately unreachable from here: parse_input_events()
 * never produces one, so the portal cannot close the application. */
struct RemoteInput {
    enum class Action : uint8_t {
        KeyDown,  /* note_key_down(event.key), then dispatch(event) */
        KeyUp,    /* note_key_up(event.key) only; nothing is dispatched */
        Dispatch, /* dispatch(event) */
    };

    Action action{Action::Dispatch};
    host::Event event{};
};

/* Parses a POST /api/input body — {"events":[ {...}, ... ]} — into actions.
 *
 * Returns false only when `body` is not parseable JSON or carries no "events"
 * array; that is the one case the caller answers with 400. Anything inside the
 * array that this build does not understand (an unknown "type", a key name
 * that is not one of the five-way switch's, a touch outside 0..239 / 0..319, a
 * character outside 0x20..0xFF) is silently skipped and counted in `dropped`,
 * never an error — a newer client sending an event this build has not learned
 * yet must not lose the events around it.
 *
 * Lives here rather than in remote_server.cpp so the wire shape is testable
 * without standing up a socket. */
bool parse_input_events(const std::string& body,
                        std::vector<RemoteInput>& out,
                        size_t& dropped);

/* Produces the PanelData for whichever view is currently open for its app.
 * Invoked ONLY from AppBridge::refresh(), i.e. only ever on the UI thread —
 * a provider may freely read the view's own state, exactly as on_frame_sync()
 * does, because it runs on the same thread. `view` is the live top-of-stack
 * View for this app id and is never null when a provider is called. */
using PanelProviderFn = std::function<PanelData(ui::View& view)>;

/* Generic adapter from any ui::RecentEntriesTable-backed container to
 * TableData. One template covers every app's Entry type: nothing here knows
 * about a specific app's layout, that knowledge lives entirely in the RowFn
 * the caller supplies. This is the "small adapter interface an app can opt
 * into": a provider wires one up with a ProviderRegistrar (below), the same
 * self-registration idiom app::Registrar already uses, so adding one is a
 * self-contained new file rather than an edit here.
 *
 * The providers live in their own src/remote/provider_*.cpp rather than in
 * the apps, which are ported code and are left byte-for-byte alone; an app
 * qualifies only if the container is already reachable through a public
 * member (see provider_aprs.cpp). See app_bridge.cpp's file header for the
 * two app ids that get a provider without needing to opt in at all. */
template <typename Entries, typename RowFn>
TableData table_data_from_entries(const ui::RecentEntriesColumns& columns,
                                   const Entries& entries,
                                   RowFn row_fn,
                                   size_t max_rows = 200) {
    TableData t;
    for (const auto& c : columns) t.columns.push_back(c.first);

    size_t n = 0;
    for (const auto& e : entries) {
        if (n >= max_rows) break;
        t.rows.push_back(row_fn(e));
        n++;
    }
    return t;
}

class AppBridge {
   public:
    static AppBridge& instance();

    AppBridge(const AppBridge&) = delete;
    AppBridge& operator=(const AppBridge&) = delete;

    /* Opt-in registration, safe to call at static-init time (as
     * app::Registrar is) or later; either way it only ever touches its own
     * map under a mutex. A second registration for the same app id replaces
     * the first. */
    void register_provider(std::string app_id, PanelProviderFn fn);

    /* --- UI thread only ----------------------------------------------- */

    /* Recomputes the cached status/current-app/panel snapshot from
     * app::globals() and whichever provider matches the app currently open
     * (if any). See the file header for why this must run on the UI thread
     * and why the HTTP-facing getters below never call a provider directly.
     *
     * This is the authority on which app is current, and it works it out from
     * the navigation stack (app::AppRegistry::id_for_view) rather than from
     * what was last launched — remote key presses and the local operator both
     * navigate without going through drain_launch_queue(). Call it after
     * NavigationView::service() so it sees a settled stack. */
    void refresh();

    /* Applies queued launch()/home() requests against app::globals().nav:
     * pop_to_root(), then push() the requested app's freshly-constructed
     * view. Returns true if a request was applied (i.e. the nav stack was
     * asked to change) so main.cpp can note it same as any other frame's
     * push/pop. A request queued while app::globals().nav is null (nav not
     * wired up yet) is dropped rather than applied — draining is a no-op in
     * that case, just as it is with an empty queue. */
    bool drain_launch_queue();

    /* Snapshots host::display into the frame cache and bumps the frame
     * sequence, but ONLY when the display reports new damage since the last
     * call — the frame seq is defined as "increments when the display reported
     * damage", so a static screen must not manufacture frames for a long-poll
     * to return. Uses Display::damage_generation(), never take_damage(): that
     * one is destructive, and consuming it here would stop the window layer
     * repainting on both platforms.
     *
     * Call after present(), not before: the framebuffer is not painted until
     * paint_widget_tree(), so a capture earlier in the frame publishes the
     * previous one. */
    void capture_screen_frame();

    /* Hands back everything queued by queue_input() and clears the queue,
     * exactly as drain_launch_queue() does with launch requests. Returns true
     * if `out` is non-empty. Swapping the whole deque out (rather than popping
     * one at a time) bounds the work per frame even while HTTP threads keep
     * enqueuing. */
    bool drain_input_queue(std::vector<RemoteInput>& out);

    /* --- Any thread -----------------------------------------------------
     * Every getter below reads only the cache refresh() maintains (or, for
     * apps_json(), the app registry, which is populated once by static
     * Registrars before main() runs and never mutated after — safe to read
     * from any thread at any time past that point). */
    std::string apps_json() const;
    std::string current_app_json() const;
    /* `have_image_rev` is the rev the client says it already holds, threaded
     * in from GET /api/panel?have_image_rev=N. It reaches the serializer and
     * nothing else: only PanelKind::Image consults it, to omit `data_b64`
     * when the browser's canvas is already showing that rev. Defaulted so
     * every caller that has no client state (tests, the launch routes) keeps
     * the "client has nothing, send the pixels" behaviour. */
    std::string panel_json(uint32_t have_image_rev = 0) const;
    std::string status_json() const;

    /* Queues a UI action; always safe to call from any thread. `app_id` is
     * not validated here — callers (remote_server.cpp) check it against the
     * registry first so the HTTP response can say "unknown app id"; an
     * unknown id that slips through is silently dropped when drained. */
    void request_launch(std::string app_id);
    void request_home();

    /* Waits for a frame newer than `after` and writes the complete wire frame
     * (16-byte header + payload) to `out`. Returns false — the caller's 204 —
     * when the wait expires, when cancel_screen_waits() fires, or when nothing
     * has been captured at all yet.
     *
     * `after == 0` with a frame present returns it immediately, which is what
     * makes a bare GET /api/screen "the current frame". `wait_ms` is clamped to
     * [0, 10000] here so no caller can park a connection thread indefinitely.
     * Blocking is fine: RemoteServer gives every connection its own thread and
     * this waits on frame_cv_ alone, holding no state any other handler needs. */
    bool screen_frame(uint32_t after, int wait_ms, std::string& out) const;

    /* Current frame sequence; 0 means nothing has been captured yet. */
    uint32_t screen_frame_seq() const;

    /* Releases every thread parked in screen_frame() with a 204 and makes any
     * wait that starts before they unwind return immediately too. Called from
     * RemoteServer::stop(): handler threads are detached and stop() does not
     * join them, but a process that exits while ten of them sit in a 10 s wait
     * still stalls, and the wait must not outlive the server that invited it.
     * Waits started after stop() returns are unaffected — the same singleton
     * has to keep working if the server is started again. */
    void cancel_screen_waits();

    /* Bound on the pending-input queue, public because the /api/input route
     * has to know it to report honestly (see queue_input). */
    static constexpr size_t kMaxQueuedInputs = 256;

    /* Enqueues remote input from any thread. Returns how many entries had to
     * be evicted to make room (the queue is bounded at kMaxQueuedInputs,
     * dropping oldest so the most recent intent survives — the same rule
     * window.cpp's Impl::push() uses).
     *
     * An eviction is NOT the same thing as this batch being refused, and the
     * /api/input response is careful about the difference: evicting the
     * oldest entries discards events from EARLIER requests, which were
     * truthfully told at the time that they were queued. Reporting them as
     * this request's "dropped" would tell a client that five key presses it
     * just sent were refused when all five were accepted. Only a request
     * longer than the whole queue loses events of its own. */
    size_t queue_input(const std::vector<RemoteInput>& events);

   private:
    AppBridge();

    enum class RequestKind { Launch, Home };
    struct QueuedRequest {
        RequestKind kind;
        std::string app_id;
    };

    mutable std::mutex queue_mutex_;
    std::deque<QueuedRequest> queue_;

    mutable std::mutex providers_mutex_;
    std::unordered_map<std::string, PanelProviderFn> providers_;

    mutable std::mutex cache_mutex_;
    std::string current_app_id_{};
    std::string current_app_name_{};
    PanelData panel_cache_{};
    bool can_go_back_{false};
    std::string status_json_cache_{};

    /* The frame cache gets its own mutex rather than sharing cache_mutex_: a
     * 150 kB publish at 60 Hz would otherwise contend with every JSON getter
     * on the connection threads. */
    mutable std::mutex frame_mutex_;
    mutable std::condition_variable frame_cv_;
    std::vector<uint8_t> frame_payload_{}; /* RGB565 LE, rows top to bottom */
    uint16_t frame_width_{0};
    uint16_t frame_height_{0};
    uint32_t frame_seq_{0}; /* 0 = nothing captured yet */
    uint64_t wait_epoch_{0};

    /* UI-thread only: no lock, never read from a connection thread. */
    uint32_t last_capture_generation_{0};
    std::vector<uint16_t> capture_scratch_{};
    std::vector<uint8_t> capture_encoded_{};

    mutable std::mutex input_mutex_;
    std::deque<RemoteInput> input_queue_;
};

/* File-scope self-registration helper, mirroring app::Registrar:
 *
 *   namespace { const remote::ProviderRegistrar reg_foo{"foo", [](ui::View& v){
 *       auto& view = static_cast<FooView&>(v);
 *       return remote::PanelData{...};
 *   }}; }
 *
 * declared at file scope in a src/remote/provider_*.cpp. Note that `v` is
 * nav->top(), which is NOT the app's root view once the local operator has
 * drilled into a detail page — walk NavigationView::at_depth() for the view
 * that owns the data rather than static_cast'ing the top blindly. */
struct ProviderRegistrar {
    ProviderRegistrar(std::string app_id, PanelProviderFn fn);
};

}  // namespace remote

#endif /*__MB200_REMOTE_APP_BRIDGE_H__*/
