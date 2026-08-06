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
 *
 * main.cpp is expected to call drain_launch_queue() immediately before
 * NavigationView::service(), and refresh() once per frame after that (see
 * its own comment for the exact placement and why).
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_REMOTE_APP_BRIDGE_H__
#define __MB200_REMOTE_APP_BRIDGE_H__

#include "app_data.hpp"

#include "../ui/ui_recent_entries.hpp"

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace ui {
class View;
}  // namespace ui

namespace remote {

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
     * and why the HTTP-facing getters below never call a provider directly. */
    void refresh();

    /* Applies queued launch()/home() requests against app::globals().nav:
     * pop_to_root(), then push() the requested app's freshly-constructed
     * view. Returns true if a request was applied (i.e. the nav stack was
     * asked to change) so main.cpp can note it same as any other frame's
     * push/pop. A request queued while app::globals().nav is null (nav not
     * wired up yet) is dropped rather than applied — draining is a no-op in
     * that case, just as it is with an empty queue. */
    bool drain_launch_queue();

    /* --- Any thread -----------------------------------------------------
     * Every getter below reads only the cache refresh() maintains (or, for
     * apps_json(), the app registry, which is populated once by static
     * Registrars before main() runs and never mutated after — safe to read
     * from any thread at any time past that point). */
    std::string apps_json() const;
    std::string current_app_json() const;
    std::string panel_json() const;
    std::string status_json() const;

    /* Queues a UI action; always safe to call from any thread. `app_id` is
     * not validated here — callers (remote_server.cpp) check it against the
     * registry first so the HTTP response can say "unknown app id"; an
     * unknown id that slips through is silently dropped when drained. */
    void request_launch(std::string app_id);
    void request_home();

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
