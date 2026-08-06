/*
 * mayhem-b200 — the seam between a running app and the web portal.
 *
 * Two built-in providers are registered below, for "audio" (the analog audio
 * receiver, analog_audio_app.hpp) and "lookingglass" (the wideband spectrum
 * view, spectrum_app.hpp). Both read radio::ReceiverModel directly through
 * app::globals().receiver rather than through the view — the receiver state
 * and the spectrum tap do not belong to either view, they belong to the
 * shared ReceiverModel those views merely display, so there is nothing
 * app-specific to read and no need to touch analog_audio_app.hpp or
 * spectrum_app.hpp to expose one. Every other app id falls back to
 * PanelKind::Screen until it opts in (see app_bridge.hpp's ProviderRegistrar).
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "app_bridge.hpp"

#include "../apps/about_app.hpp"
#include "../apps/app_context.hpp"
#include "../apps/app_registry.hpp"
#include "../apps/ui_navigation.hpp"
#include "../dsp/fft.hpp"
#include "../radio/receiver_model.hpp"

#include <array>
#include <utility>

namespace remote {

namespace {

/* --- Built-in provider: receiver state ---------------------------------- */

PanelData receiver_panel(ui::View&) {
    PanelData p;
    auto* r = app::globals().receiver;
    if (r == nullptr) {
        p.kind = PanelKind::Screen;
        p.screen.message = "No receiver attached.";
        return p;
    }

    p.kind = PanelKind::Receiver;
    p.receiver.mode = r->mode_name();
    p.receiver.frequency_hz = r->target_frequency();
    p.receiver.gain_db = r->gain();
    p.receiver.squelch = r->squelch_level();
    p.receiver.volume = r->volume();
    p.receiver.channel_level_db = r->channel_level_db();
    p.receiver.rf_level_db = r->rf_level_db();
    p.receiver.squelch_open = r->squelch_open();
    p.receiver.running = r->running();
    return p;
}

/* --- Built-in provider: spectrum ----------------------------------------
 *
 * Same FFT size and window as AnalogAudioView's own waterfall
 * (analog_audio_app.cpp), so the numbers shown here match what the device's
 * own display would show rather than a different resolution invented for
 * the portal. */
constexpr size_t kSpectrumFftSize = 1024;

/* UI-thread-only scratch space (this function is only ever called from
 * AppBridge::refresh(), see app_bridge.hpp's file header). A function-local
 * static rather than an AppBridge member because it is purely an
 * implementation detail of this one provider. */
struct SpectrumScratch {
    dsp::Fft fft{kSpectrumFftSize};
    std::vector<float> window = dsp::make_window(dsp::WindowType::BlackmanHarris, kSpectrumFftSize);
    std::vector<dsp::cfloat> samples{};
    std::vector<float> spectrum_db{};
};

PanelData spectrum_panel(ui::View&) {
    PanelData p;
    auto* r = app::globals().receiver;
    if (r == nullptr) {
        p.kind = PanelKind::Screen;
        p.screen.message = "No receiver attached.";
        return p;
    }

    static SpectrumScratch scratch;
    p.kind = PanelKind::Spectrum;
    p.spectrum.centre_hz = r->target_frequency();
    p.spectrum.span_hz = r->sampling_rate();

    /* take_spectrum_samples() returning false, or fewer samples than the FFT
     * needs, means nothing is ready yet — bins_db is left empty rather than
     * filled with stale or synthesized data. */
    if (r->take_spectrum_samples(scratch.samples, scratch.fft.size()) &&
        scratch.samples.size() >= scratch.fft.size()) {
        scratch.fft.spectrum_db(scratch.samples.data(), scratch.window, scratch.spectrum_db);
        p.spectrum.bins_db.assign(scratch.spectrum_db.begin(), scratch.spectrum_db.end());
    }
    return p;
}

}  // namespace

AppBridge& AppBridge::instance() {
    static AppBridge bridge;
    return bridge;
}

AppBridge::AppBridge() {
    register_provider("audio", receiver_panel);
    register_provider("lookingglass", spectrum_panel);

    /* Gives GET /api/status a valid, honestly-"nothing yet" body if it lands
     * before the UI thread's first refresh() — same shape refresh() itself
     * writes, just with every field at its not-yet-known default rather than
     * an empty "{}" the frontend would have to special-case. */
    JsonValue status = JsonValue::object();
    status.set("device", JsonValue::string("no device"));
    status.set("device_ok", JsonValue::boolean(false));
    status.set("receiving", JsonValue::boolean(false));
    status.set("transmitting", JsonValue::boolean(false));
    status.set("version", JsonValue::string(app::kVersion));
    JsonValue levels = JsonValue::object();
    levels.set("channel_db", JsonValue::number(-140.0));
    levels.set("rf_db", JsonValue::number(-140.0));
    status.set("levels", std::move(levels));
    status_json_cache_ = status.dump();
}

void AppBridge::register_provider(std::string app_id, PanelProviderFn fn) {
    std::lock_guard<std::mutex> lk(providers_mutex_);
    providers_[std::move(app_id)] = std::move(fn);
}

void AppBridge::refresh() {
    auto& ctx = app::globals();

    /* Status snapshot: always computed, independent of whether an app is
     * open. Every field here is read the same way main.cpp's own status bar
     * update already reads it. */
    JsonValue status = JsonValue::object();
    const bool device_open = (ctx.radio != nullptr) && ctx.radio->is_open();
    status.set("device", JsonValue::string(device_open ? ctx.radio->caps().mboard : std::string{"no device"}));
    status.set("device_ok", JsonValue::boolean(device_open));
    status.set("receiving", JsonValue::boolean((ctx.receiver != nullptr) && ctx.receiver->running()));
    status.set("transmitting", JsonValue::boolean((ctx.radio != nullptr) && ctx.radio->tx_running()));
    status.set("version", JsonValue::string(app::kVersion));

    JsonValue levels = JsonValue::object();
    levels.set("channel_db", JsonValue::number(ctx.receiver != nullptr ? ctx.receiver->channel_level_db() : -140.0));
    levels.set("rf_db", JsonValue::number(ctx.receiver != nullptr ? ctx.receiver->rf_level_db() : -140.0));
    status.set("levels", std::move(levels));

    /* Panel for whichever app is current. */
    std::string app_id;
    {
        std::lock_guard<std::mutex> lk(cache_mutex_);
        app_id = current_app_id_;
    }

    PanelData panel;
    std::string app_name;
    if (app_id.empty()) {
        panel.kind = PanelKind::Screen;
        panel.screen.message = "Home -- no app is open.";
    } else {
        const auto* entry = app::AppRegistry::instance().by_id(app_id);
        app_name = (entry != nullptr) ? entry->display_name : app_id;

        PanelProviderFn provider;
        {
            std::lock_guard<std::mutex> lk(providers_mutex_);
            const auto it = providers_.find(app_id);
            if (it != providers_.end()) provider = it->second;
        }

        ui::View* top = (ctx.nav != nullptr) ? ctx.nav->top() : nullptr;
        if (provider && top != nullptr) {
            panel = provider(*top);
        } else {
            panel.kind = PanelKind::Screen;
            panel.screen.message = "No structured data provider for '" + app_name + "' yet.";
        }
    }

    const bool can_go_back = (ctx.nav != nullptr) && !ctx.nav->is_root();

    std::lock_guard<std::mutex> lk(cache_mutex_);
    current_app_name_ = std::move(app_name);
    panel_cache_ = std::move(panel);
    can_go_back_ = can_go_back;
    status_json_cache_ = status.dump();
}

bool AppBridge::drain_launch_queue() {
    std::deque<QueuedRequest> local;
    {
        std::lock_guard<std::mutex> lk(queue_mutex_);
        std::swap(local, queue_);
    }
    if (local.empty()) return false;

    auto& ctx = app::globals();
    if (ctx.nav == nullptr) return false; /* not wired up yet; drop the batch */

    bool changed = false;
    for (auto& req : local) {
        if (req.kind == RequestKind::Home) {
            ctx.nav->pop_to_root();
            {
                std::lock_guard<std::mutex> lk(cache_mutex_);
                current_app_id_.clear();
            }
            changed = true;
            continue;
        }

        const auto* entry = app::AppRegistry::instance().by_id(req.app_id);
        if (entry == nullptr) continue; /* unknown id slipped through; ignore defensively */

        auto view = entry->factory();
        if (!view) continue;

        ctx.nav->pop_to_root();
        ctx.nav->push(std::move(view));
        {
            std::lock_guard<std::mutex> lk(cache_mutex_);
            current_app_id_ = req.app_id;
        }
        changed = true;
    }
    return changed;
}

std::string AppBridge::apps_json() const {
    static constexpr std::array<app::Category, 8> kAllCategories{
        app::Category::Home,        app::Category::Receive, app::Category::Transmit,
        app::Category::Transceiver, app::Category::Utilities, app::Category::Games,
        app::Category::Settings,    app::Category::Debug,
    };

    /* A FLAT array under "apps", not a pre-grouped {"categories":[...]}.
     * internal/portal/appindex is what buckets these by category, in this
     * same canonical order, and it is also where the portal's category
     * exclusions live — so grouping here as well would be both redundant and
     * a second place for the two halves to disagree. Iterating the categories
     * in order still gives the flat list a sensible default ordering for any
     * client that does no grouping of its own. */
    JsonValue apps_arr = JsonValue::array();
    for (auto cat : kAllCategories) {
        for (const auto* e : app::AppRegistry::instance().by_category(cat)) {
            AppSummary s;
            s.id = e->id;
            s.name = e->display_name;
            s.category = app::category_name(e->category);
            s.hardware_limited = e->hardware_limited;
            s.icon_name = e->id;
            apps_arr.push_back(to_json(s));
        }
    }

    JsonValue root = JsonValue::object();
    root.set("apps", std::move(apps_arr));
    return root.dump();
}

std::string AppBridge::current_app_json() const {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    JsonValue v = JsonValue::object();
    v.set("id", JsonValue::string(current_app_id_));
    v.set("title", JsonValue::string(current_app_id_.empty() ? std::string{"Home"} : current_app_name_));
    v.set("panel_kind", JsonValue::string(panel_kind_name(panel_cache_.kind)));
    /* can_go_back is what enables the browser's back control; without it the
     * portal can never leave an app it opened. Cached by refresh() rather
     * than read from nav here, because this runs on a connection thread. */
    v.set("can_go_back", JsonValue::boolean(can_go_back_));
    return v.dump();
}

/* The envelope PANELS.md specifies and internal/portal/client decodes:
 * {app_id, panel_kind, title, data}. It is NOT to_json(PanelData)'s
 * self-describing {kind, <kind>: {...}} form — the browser's renderPanel()
 * dispatches on `panel_kind` and hands the renderer `data` alone, and
 * client.Panel.HasData() keys off `panel_kind` being non-empty, so emitting
 * the other shape here makes every app render as "no structured view yet"
 * even when its provider published a full panel. */
std::string AppBridge::panel_json() const {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    JsonValue v = JsonValue::object();
    v.set("app_id", JsonValue::string(current_app_id_));
    v.set("panel_kind", JsonValue::string(panel_kind_name(panel_cache_.kind)));
    v.set("title", JsonValue::string(
                       current_app_id_.empty() ? std::string{"Home"} : current_app_name_));
    v.set("data", panel_payload(panel_cache_));
    return v.dump();
}

std::string AppBridge::status_json() const {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    return status_json_cache_;
}

void AppBridge::request_launch(std::string app_id) {
    std::lock_guard<std::mutex> lk(queue_mutex_);
    queue_.push_back({RequestKind::Launch, std::move(app_id)});
}

void AppBridge::request_home() {
    std::lock_guard<std::mutex> lk(queue_mutex_);
    queue_.push_back({RequestKind::Home, std::string{}});
}

ProviderRegistrar::ProviderRegistrar(std::string app_id, PanelProviderFn fn) {
    AppBridge::instance().register_provider(std::move(app_id), std::move(fn));
}

}  // namespace remote
