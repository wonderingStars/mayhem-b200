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
/* The portal's own JsonValue (app_data.hpp) is write-only by design; this is
 * the project's one JSON *parser*, already in the same static library, and
 * POST /api/input is the first route with a request body to decode. */
#include "../radio/network_radio.hpp"
#include "../radio/receiver_model.hpp"
#include "../ui/display.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <utility>

namespace remote {

/* The sdrlink client's JSON parser (src/radio/network_radio.hpp) under a short
 * name: it is the project's only JSON *parser* — app_data.hpp's JsonValue is
 * write-only by design — and POST /api/input is the first portal route with a
 * request body to decode. Aliased once here rather than spelled out at every
 * use, and kept distinct from remote::JsonValue, which is the encoder. */
namespace json = radio::net;

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

/* --- Remote input parsing ------------------------------------------------ */

/* Only the five-way switch and Back. KeyEvent::Dfu exists but is deliberately
 * not reachable from the network: it is the firmware's recovery entry point,
 * not a navigation key. */
bool map_key_name(const std::string& name, ui::KeyEvent& out) {
    if (name == "up") { out = ui::KeyEvent::Up; return true; }
    if (name == "down") { out = ui::KeyEvent::Down; return true; }
    if (name == "left") { out = ui::KeyEvent::Left; return true; }
    if (name == "right") { out = ui::KeyEvent::Right; return true; }
    if (name == "select") { out = ui::KeyEvent::Select; return true; }
    if (name == "back") { out = ui::KeyEvent::Back; return true; }
    return false;
}

bool map_touch_phase(const std::string& phase, ui::TouchEvent::Type& out) {
    if (phase == "start") { out = ui::TouchEvent::Type::Start; return true; }
    if (phase == "move") { out = ui::TouchEvent::Type::Move; return true; }
    if (phase == "end") { out = ui::TouchEvent::Type::End; return true; }
    return false;
}

/* True when `v` is a JSON number holding an exact integer inside [lo, hi].
 * A fractional or out-of-range coordinate is not a coordinate this display
 * has — it is dropped rather than clamped, because clamping would silently
 * turn a client's bug into a touch somewhere the operator never asked for. */
bool exact_integer_in(const json::JsonValue& v, double lo, double hi, int32_t& out) {
    if (v.type != json::JsonValue::Type::Number) return false;
    const double d = v.number_value;
    if (!std::isfinite(d) || d < lo || d > hi) return false;
    if (d != std::floor(d)) return false;
    out = static_cast<int32_t>(d);
    return true;
}

bool parse_one_event(const json::JsonValue& e, RemoteInput& out) {
    if (!e.is_object()) return false;

    const json::JsonValue* type = e.find("type");
    if (type == nullptr) return false;
    const std::string kind = type->as_string();

    if (kind == "key") {
        const json::JsonValue* key = e.find("key");
        if (key == nullptr) return false;
        ui::KeyEvent mapped{};
        if (!map_key_name(key->as_string(), mapped)) return false;

        const json::JsonValue* down = e.find("down");
        /* "down" is required by the contract; a missing one is a malformed
         * event, not an implied press — guessing here would latch a key. */
        if (down == nullptr || down->type != json::JsonValue::Type::Bool) return false;

        out.action = down->bool_value ? RemoteInput::Action::KeyDown : RemoteInput::Action::KeyUp;
        out.event.type = host::Event::Type::Key;
        out.event.key = mapped;
        return true;
    }

    if (kind == "encoder") {
        const json::JsonValue* delta = e.find("delta");
        if (delta == nullptr) return false;
        int32_t detents = 0;
        if (!exact_integer_in(*delta, -32768.0, 32767.0, detents)) return false;

        out.action = RemoteInput::Action::Dispatch;
        out.event.type = host::Event::Type::Encoder;
        out.event.encoder = detents;
        return true;
    }

    if (kind == "touch") {
        const json::JsonValue* x = e.find("x");
        const json::JsonValue* y = e.find("y");
        const json::JsonValue* phase = e.find("phase");
        if (x == nullptr || y == nullptr || phase == nullptr) return false;

        int32_t px = 0;
        int32_t py = 0;
        if (!exact_integer_in(*x, 0.0, static_cast<double>(ui::screen_width - 1), px)) return false;
        if (!exact_integer_in(*y, 0.0, static_cast<double>(ui::screen_height - 1), py)) return false;

        ui::TouchEvent::Type t{};
        if (!map_touch_phase(phase->as_string(), t)) return false;

        out.action = RemoteInput::Action::Dispatch;
        out.event.type = host::Event::Type::Touch;
        out.event.touch = {{px, py}, t};
        return true;
    }

    if (kind == "char") {
        const json::JsonValue* c = e.find("c");
        if (c == nullptr) return false;
        int32_t code = 0;
        if (!exact_integer_in(*c, 0x20, 0xFF, code)) return false;

        out.action = RemoteInput::Action::Dispatch;
        out.event.type = host::Event::Type::Character;
        out.event.character = static_cast<ui::KeyboardEvent>(code);
        return true;
    }

    return false; /* unknown type: dropped and counted, never an error */
}

}  // namespace

bool parse_input_events(const std::string& body, std::vector<RemoteInput>& out, size_t& dropped) {
    out.clear();
    dropped = 0;

    json::JsonValue root;
    std::string error;
    if (!json::json_parse(body, root, error)) return false;
    if (!root.is_object()) return false;

    const json::JsonValue* events = root.find("events");
    if (events == nullptr || !events->is_array()) return false;

    for (const auto& e : events->array_value) {
        RemoteInput ri;
        if (parse_one_event(e, ri))
            out.push_back(ri);
        else
            dropped++;
    }
    return true;
}

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
    status.set("link", JsonValue::string("disconnected"));
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
    /* Not the same question as device_ok: a networked radio that dropped is
     * "reconnecting" while it climbs its retry ladder, and showing that as a
     * plain offline device hides the fact that it is coming back. */
    status.set("link", JsonValue::string(ctx.radio != nullptr ? ctx.radio->link_state_string()
                                                             : "disconnected"));
    status.set("receiving", JsonValue::boolean((ctx.receiver != nullptr) && ctx.receiver->running()));
    status.set("transmitting", JsonValue::boolean((ctx.radio != nullptr) && ctx.radio->tx_running()));
    status.set("version", JsonValue::string(app::kVersion));

    JsonValue levels = JsonValue::object();
    levels.set("channel_db", JsonValue::number(ctx.receiver != nullptr ? ctx.receiver->channel_level_db() : -140.0));
    levels.set("rf_db", JsonValue::number(ctx.receiver != nullptr ? ctx.receiver->rf_level_db() : -140.0));
    status.set("levels", std::move(levels));

    /* Which app is current is DERIVED from the navigation stack, not
     * remembered from the last launch request.
     *
     * drain_launch_queue() used to be the only writer, which was true enough
     * while the portal could only navigate by launching. It stopped being
     * true the moment POST /api/input landed: a remote key press walks the
     * device's own menus exactly as the local encoder does, and so does the
     * operator standing in front of it. A remembered id then names whatever
     * was launched last, so the portal serves Calculator's title and
     * Calculator's panel while the device is sitting in Replay — and once the
     * kind goes stale with it, the app actually running is unreachable.
     *
     * Walk the stack downward from the top. An app's own sub-views (a config
     * page, a details view) were not made by a registered factory and resolve
     * to nothing, so the first view that does resolve is the app the operator
     * is inside — which is also what the providers want, since their data
     * lives on the view the app pushed first. Nothing resolving means only
     * menus are on the stack, i.e. Home. */
    std::string app_id;
    if (ctx.nav != nullptr) {
        for (size_t i = 0; i < ctx.nav->depth() && app_id.empty(); i++) {
            const ui::View* v = ctx.nav->at_depth(i);
            if (v != nullptr) app_id = v->app_id();
        }
    }

    PanelData panel;
    std::string app_name;
    if (app_id.empty()) {
        /* No app, but not necessarily Home: the category menus are views on
         * the stack too, and calling the Receive menu "Home" is the same kind
         * of small lie as calling Replay "Calculator". The menu's own title is
         * used when the stack is deeper than the root; the root menu's title
         * is "Mayhem B200", which is not what "where am I" should answer, so
         * that one stays "Home". */
        std::string place = "Home";
        if (ctx.nav != nullptr && !ctx.nav->is_root()) {
            std::string nav_title = ctx.nav->current_title();
            if (!nav_title.empty()) place = std::move(nav_title);
        }
        panel.kind = PanelKind::Screen;
        panel.screen.message = place + " -- no app is open.";
        /* current_app_name_ carries it so current_app_json()'s title agrees
         * with the panel; the id stays empty, which is what "no app" means. */
        app_name = std::move(place);
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
    current_app_id_ = std::move(app_id);
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
                /* refresh() is what decides the current app (it derives it
                 * from the stack); this is only so a GET landing between the
                 * request and the next refresh does not answer with the app
                 * the operator just left. The push/pop is deferred to
                 * service(), so the stack cannot be consulted yet here. */
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
            /* Same as the Home branch: a provisional answer for the gap
             * before the next refresh(), which re-derives it from the stack. */
            std::lock_guard<std::mutex> lk(cache_mutex_);
            current_app_id_ = req.app_id;
        }
        changed = true;
    }
    return changed;
}

/* --- Screen frames -------------------------------------------------------- */

namespace {

void put_u16_le(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void put_u32_le(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>((v >> 16) & 0xFF));
    s.push_back(static_cast<char>((v >> 24) & 0xFF));
}

}  // namespace

/* Not file-local, so tests/test_portal_contract_fixes.cpp can pin it at the
 * two boundaries that matter (0, and the 2^31 point) without manufacturing
 * two billion frames to get there.
 *
 * "Is `seq` newer than `after`" over a wrapping uint32. The plain `seq > after`
 * the protocol text implies would, once the counter wraps, park every client
 * whose `after` is near 0xFFFFFFFF until the counter caught up again. Signed
 * difference is the same answer everywhere except across the wrap, where it is
 * the right one — network_radio.cpp's frames_dropped_between() reasons about
 * its own seq the same way. */
bool seq_is_newer(uint32_t seq, uint32_t after) {
    /* after == 0 is not a sequence number, it is the contract's "give me the
     * current frame" (a bare GET /api/screen, and the Go hub's resync after a
     * backend restart, both send it). Answering it by signed difference made
     * it mean "only if seq < 2^31", so once the counter passed 0x80000000
     * every such request would 204 for the next two billion frames. seq 0 is
     * reserved for "nothing captured yet" and is rejected by the callers. */
    if (after == 0) return true;
    return static_cast<int32_t>(seq - after) > 0;
}

void AppBridge::capture_screen_frame() {
    auto& d = host::display;

    /* Non-destructive: take_damage() belongs to the window layer and reading
     * it here would leave the window blank on both platforms. */
    const uint32_t generation = d.damage_generation();
    if (generation == last_capture_generation_) return;
    last_capture_generation_ = generation;

    const int w = d.width();
    const int h = d.height();
    if (w <= 0 || h <= 0) return;

    const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
    capture_scratch_.resize(pixels);
    d.composite_rgb565(capture_scratch_.data());

    /* Serialised little-endian explicitly rather than memcpy'd, so the wire
     * format does not quietly depend on the host's byte order. */
    capture_encoded_.resize(pixels * 2);
    for (size_t i = 0; i < pixels; i++) {
        const uint16_t v = capture_scratch_[i];
        capture_encoded_[i * 2] = static_cast<uint8_t>(v & 0xFF);
        capture_encoded_[i * 2 + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
    }

    {
        std::lock_guard<std::mutex> lk(frame_mutex_);
        /* Swap rather than copy: capture_encoded_ takes over the previous
         * frame's already-right-sized buffer, so a steady 60 Hz stream does no
         * per-frame allocation at all. */
        frame_payload_.swap(capture_encoded_);
        frame_width_ = static_cast<uint16_t>(w);
        frame_height_ = static_cast<uint16_t>(h);
        /* seq 0 is reserved for "nothing captured yet", so skip it on wrap. */
        if (++frame_seq_ == 0) frame_seq_ = 1;
    }
    frame_cv_.notify_all();
}

bool AppBridge::screen_frame(uint32_t after, int wait_ms, std::string& out) const {
    if (wait_ms < 0) wait_ms = 0;
    if (wait_ms > 10000) wait_ms = 10000;

    std::unique_lock<std::mutex> lk(frame_mutex_);
    const uint64_t epoch = wait_epoch_;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);

    /* wait_until with a predicate handles spurious wakeups and returns false
     * exactly once the deadline has passed without the predicate holding. */
    frame_cv_.wait_until(lk, deadline, [this, after, epoch] {
        return wait_epoch_ != epoch || (frame_seq_ != 0 && seq_is_newer(frame_seq_, after));
    });

    if (frame_seq_ == 0 || !seq_is_newer(frame_seq_, after)) return false;

    out.clear();
    out.reserve(kScreenFrameHeaderBytes + frame_payload_.size());
    out.append(kScreenFrameMagic, sizeof(kScreenFrameMagic));
    out.push_back(static_cast<char>(kScreenFrameVersion));
    out.push_back(static_cast<char>(kScreenFormatRgb565));
    put_u16_le(out, frame_width_);
    put_u16_le(out, frame_height_);
    put_u32_le(out, frame_seq_);
    put_u16_le(out, 0); /* reserved */
    out.append(reinterpret_cast<const char*>(frame_payload_.data()), frame_payload_.size());
    return true;
}

uint32_t AppBridge::screen_frame_seq() const {
    std::lock_guard<std::mutex> lk(frame_mutex_);
    return frame_seq_;
}

void AppBridge::cancel_screen_waits() {
    {
        std::lock_guard<std::mutex> lk(frame_mutex_);
        wait_epoch_++;
    }
    frame_cv_.notify_all();
}

/* --- Remote input --------------------------------------------------------- */

size_t AppBridge::queue_input(const std::vector<RemoteInput>& events) {
    std::lock_guard<std::mutex> lk(input_mutex_);
    size_t evicted = 0;
    for (const auto& e : events) {
        if (input_queue_.size() >= kMaxQueuedInputs) {
            input_queue_.pop_front();
            evicted++;
        }
        input_queue_.push_back(e);
    }
    return evicted;
}

bool AppBridge::drain_input_queue(std::vector<RemoteInput>& out) {
    out.clear();
    std::deque<RemoteInput> local;
    {
        std::lock_guard<std::mutex> lk(input_mutex_);
        std::swap(local, input_queue_);
    }
    if (local.empty()) return false;
    out.assign(local.begin(), local.end());
    return true;
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
    v.set("title", JsonValue::string(current_app_name_.empty() ? std::string{"Home"} : current_app_name_));
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
std::string AppBridge::panel_json(uint32_t have_image_rev) const {
    std::lock_guard<std::mutex> lk(cache_mutex_);
    JsonValue v = JsonValue::object();
    v.set("app_id", JsonValue::string(current_app_id_));
    v.set("panel_kind", JsonValue::string(panel_kind_name(panel_cache_.kind)));
    v.set("title", JsonValue::string(
                       current_app_name_.empty() ? std::string{"Home"} : current_app_name_));
    v.set("data", panel_payload(panel_cache_, have_image_rev));
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
