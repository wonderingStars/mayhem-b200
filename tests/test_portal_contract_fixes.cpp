/*
 * mayhem-b200 — regression tests for four portal defects found by review of
 * the browser-Mayhem change-set, each of which had green tests on both sides
 * of the wire while being wrong in the middle.
 *
 *   1. GET /api/panel ignored ?have_image_rev=N. The serializer honoured the
 *      parameter and the Go portal forwarded it, but the C++ route never read
 *      it, so an open image panel re-sent its whole base64 payload on every
 *      poll. Tested through a real socket, because the route is the only
 *      place the bug lived — panel_payload()'s own tests passed throughout.
 *   2. seq_is_newer(seq, 0) answered "no" for the whole upper half of the
 *      counter, stranding the one value contract 1 gives a special meaning:
 *      a bare GET /api/screen, and the Go hub's resync after a restart, both
 *      send after=0.
 *   3. AppBridge remembered the current app from the last launch request
 *      instead of deriving it from the navigation stack. Remote key presses
 *      navigate the device, so the portal served one app's panel under
 *      another app's title as soon as the operator used the live screen for
 *      what it is for.
 *   4. POST /api/input reported queue evictions as this request's "dropped",
 *      i.e. told a client that five keys it had just successfully sent were
 *      refused.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "app_context.hpp"
#include "app_registry.hpp"
#include "audio_out.hpp"
#include "receiver_model.hpp"
#include "remote/app_bridge.hpp"
#include "remote/app_data.hpp"
#include "remote/remote_server.hpp"
#include "ui.hpp"
#include "ui_navigation.hpp"
#include "ui_widget.hpp"
#include "usrp_radio.hpp"

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/* Defined in src/remote/app_bridge.cpp. Declared here rather than in the
 * header because it is an implementation detail of the frame cache; the same
 * idiom the console tests use for remote::console_data_from(). */
namespace remote {
bool seq_is_newer(uint32_t seq, uint32_t after);
}  // namespace remote

namespace {

bool has(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

/* ===========================================================================
 * 2. seq_is_newer
 * ========================================================================= */

}  // namespace

TEST(screen_seq_after_zero_means_the_current_frame_at_any_seq) {
    /* Contract 1: "GET /api/screen -> the current frame immediately". after=0
     * is how both a bare GET and the Go hub's restart resync ask for that, so
     * it has to be answered whatever the counter has reached. The signed
     * difference alone said no for every seq with the top bit set — half the
     * counter's range, ~414 days at 60 Hz of damage. */
    CHECK(remote::seq_is_newer(1, 0));
    CHECK(remote::seq_is_newer(0x7FFFFFFFu, 0));
    CHECK(remote::seq_is_newer(0x80000000u, 0));
    CHECK(remote::seq_is_newer(0xFFFFFFFFu, 0));
}

TEST(screen_seq_ordering_is_unchanged_for_real_sequence_numbers) {
    CHECK(remote::seq_is_newer(2, 1));
    CHECK(!remote::seq_is_newer(1, 1));
    CHECK(!remote::seq_is_newer(1, 2));
}

TEST(screen_seq_still_treats_the_wrap_as_newer) {
    /* The reason the signed difference is there in the first place: a client
     * holding a seq just below the wrap must not be parked for two billion
     * frames once the counter rolls over. */
    CHECK(remote::seq_is_newer(3, 0xFFFFFFFEu));
    CHECK(!remote::seq_is_newer(0xFFFFFFFEu, 3));
}

/* ===========================================================================
 * 3. Which app is open is read off the navigation stack
 * ========================================================================= */

namespace {

/* Same harness shape as the provider test files', copied rather than shared
 * because appending to another agent's test file is how two change-sets
 * collide. Nothing here opens a device: on_show() calls receiver.start(),
 * which fails at start_rx() on a closed radio and returns false without
 * spawning a DSP thread. */
struct BridgeHarness {
    radio::UsrpRadio radio{};
    audio::AudioOut audio{};
    radio::ReceiverModel receiver{radio, audio};
    ui::NavigationView nav{{0, 0, 240, 304}};

    radio::RadioDevice* saved_radio{app::globals().radio};
    radio::ReceiverModel* saved_receiver{app::globals().receiver};
    ui::NavigationView* saved_nav{app::globals().nav};

    BridgeHarness() {
        app::globals().radio = &radio;
        app::globals().receiver = &receiver;
        app::globals().nav = &nav;

        nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304})); /* root */
        nav.service();
    }

    ~BridgeHarness() {
        /* AppBridge is a process-global singleton, so a test that leaves it
         * believing an app is open hands that state to every later test in
         * the binary — and to every -count= rerun. */
        remote::AppBridge::instance().request_home();
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();
        remote::AppBridge::instance().refresh();

        app::globals().radio = saved_radio;
        app::globals().receiver = saved_receiver;
        app::globals().nav = saved_nav;
    }

    /* Opens an app the way the PORTAL does: through the launch queue. */
    void launch(const std::string& app_id) {
        remote::AppBridge::instance().request_launch(app_id);
        remote::AppBridge::instance().drain_launch_queue();
        nav.service();
    }

    /* Opens an app the way the DEVICE does: the registry's factory, pushed
     * straight onto the stack. This is byte for byte what main_menu.cpp's
     * launch() does, and it is the path a remote key press takes — the portal
     * is never told about it, which is exactly what used to go wrong. */
    bool open_from_the_menu(const std::string& app_id) {
        const auto* entry = app::AppRegistry::instance().by_id(app_id);
        if (entry == nullptr || !entry->factory) return false;
        auto view = entry->factory();
        if (!view) return false;
        nav.push(std::move(view));
        nav.service();
        return true;
    }

    std::string panel() {
        remote::AppBridge::instance().refresh();
        return remote::AppBridge::instance().panel_json();
    }

    std::string current() {
        remote::AppBridge::instance().refresh();
        return remote::AppBridge::instance().current_app_json();
    }
};

}  // namespace

TEST(current_app_follows_the_device_when_the_operator_navigates_by_key) {
    /* The reported failure, reproduced: the portal launches one app, the
     * operator (or a browser key press, which is the same thing) leaves it
     * and opens another, and the portal used to go on serving the first app's
     * id, title and panel — so the app actually running was unreachable. */
    BridgeHarness h;
    h.launch("calculator");
    CHECK(has(h.current(), "\"id\":\"calculator\""));

    /* Back to the menu and into another app, entirely on the device. */
    h.nav.pop_to_root();
    h.nav.service();
    CHECK(h.open_from_the_menu("pocsag"));

    const std::string current = h.current();
    CHECK(has(current, "\"id\":\"pocsag\""));
    CHECK(has(current, "\"title\":\"POCSAG RX\""));
    CHECK(!has(current, "calculator"));

    /* And the panel follows, which is the part an operator actually sees: the
     * POCSAG console, not Calculator's placeholder. */
    const std::string panel = h.panel();
    CHECK(has(panel, "\"app_id\":\"pocsag\""));
    CHECK(has(panel, "\"panel_kind\":\"console\""));
}

TEST(current_app_is_home_once_the_operator_backs_out_on_the_device) {
    BridgeHarness h;
    h.launch("calculator");

    h.nav.pop_to_root();
    h.nav.service();

    const std::string current = h.current();
    CHECK(has(current, "\"id\":\"\""));
    CHECK(has(current, "\"title\":\"Home\""));
    CHECK(has(h.panel(), "Home -- no app is open."));
}

TEST(current_app_survives_a_sub_view_the_app_pushed_itself) {
    /* The other half of deriving from the stack: an app's own sub-views were
     * not made by a registered factory, so the walk has to keep going down
     * rather than concluding that no app is open. */
    BridgeHarness h;
    h.launch("pocsag");
    h.nav.push(std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304}));
    h.nav.service();
    CHECK_EQ(h.nav.depth(), size_t{3});

    CHECK(has(h.current(), "\"id\":\"pocsag\""));
}

TEST(registry_stamps_the_app_id_onto_the_view_its_factory_made) {
    auto& registry = app::AppRegistry::instance();
    const auto* entry = registry.by_id("calculator");
    CHECK(entry != nullptr);
    if (entry == nullptr) return;

    auto view = entry->factory();
    CHECK(view != nullptr);
    if (!view) return;
    CHECK_STR_EQ(view->app_id(), "calculator");
}

TEST(a_view_no_app_factory_made_carries_no_app_id) {
    /* A menu, or an app's own sub-view. Answering with a guess here is what
     * would put one app's data under another app's name. */
    ui::View plain{ui::Rect{0, 0, 240, 304}};
    CHECK_STR_EQ(plain.app_id(), "");
}

TEST(the_app_id_dies_with_the_view_that_carried_it) {
    /* Why the id lives on the view and not in a side table keyed by pointer.
     * A table outlives the view it describes, and the allocator hands the
     * same address to whatever is built next — very often a sub-view of a
     * DIFFERENT app, which the portal would then have reported under the dead
     * app's name. Freeing an app view and building a plain one in its place
     * has to leave nothing behind, however the addresses fall. */
    auto& registry = app::AppRegistry::instance();
    const auto* entry = registry.by_id("calculator");
    CHECK(entry != nullptr);
    if (entry == nullptr) return;

    auto app_view = entry->factory();
    CHECK(app_view != nullptr);
    if (!app_view) return;
    CHECK_STR_EQ(app_view->app_id(), "calculator");
    app_view.reset();

    /* Enough allocations of the right shape that one of them is very likely
     * to land on the block just freed; the assertion holds either way, which
     * is the point — the answer cannot depend on the allocator. */
    for (int i = 0; i < 16; i++) {
        auto plain = std::make_unique<ui::View>(ui::Rect{0, 0, 240, 304});
        CHECK_STR_EQ(plain->app_id(), "");
    }
}

/* ===========================================================================
 * 1. GET /api/panel?have_image_rev=N and 4. POST /api/input accounting
 *
 * Both live in the HTTP route and nowhere else, so both are driven over a
 * real loopback socket.
 * ========================================================================= */

namespace {

/* A throwaway loopback client. The other portal test files each keep their
 * own copy in their own anonymous namespace for the same reason this one
 * does: reaching into another agent's test file to share it is how two
 * change-sets collide. */
class TestClient {
   public:
    ~TestClient() { close(); }

    bool connect_to(uint16_t port) {
        close();
        sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock_ == remote::kInvalidSocket) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            close();
            return false;
        }
        return true;
    }

    bool send_all(const std::string& s) {
#if defined(_WIN32)
        return ::send(sock_, s.data(), static_cast<int>(s.size()), 0) ==
               static_cast<int>(s.size());
#else
        return ::send(sock_, s.data(), s.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(s.size());
#endif
    }

    std::string read_all() {
        std::string out;
        char buf[8192];
        for (;;) {
#if defined(_WIN32)
            const int n = ::recv(sock_, buf, static_cast<int>(sizeof(buf)), 0);
#else
            const ssize_t n = ::recv(sock_, buf, sizeof(buf), 0);
#endif
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
        return out;
    }

    void close() {
        if (sock_ == remote::kInvalidSocket) return;
#if defined(_WIN32)
        closesocket(sock_);
#else
        ::close(sock_);
#endif
        sock_ = remote::kInvalidSocket;
    }

   private:
    remote::socket_t sock_{remote::kInvalidSocket};
};

std::string response_body(const std::string& response) {
    const auto end = response.find("\r\n\r\n");
    if (end == std::string::npos) return {};
    return response.substr(end + 4);
}

std::string http_get(uint16_t port, const std::string& target) {
    TestClient c;
    if (!c.connect_to(port)) return {};
    if (!c.send_all("GET " + target +
                    " HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"))
        return {};
    return c.read_all();
}

std::string http_post(uint16_t port, const std::string& target, const std::string& body) {
    TestClient c;
    if (!c.connect_to(port)) return {};
    if (!c.send_all("POST " + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: " +
                    std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body))
        return {};
    return c.read_all();
}

/* --- the image panel under test -----------------------------------------
 *
 * No B200 is attached in a test run, so no APT/WeFax/SSTV pass ever decodes
 * and every real image provider honestly publishes rev 0 with no pixels —
 * which is the one case where have_image_rev cannot be observed, since rev 0
 * never carries pixels anyway. So a provider is registered here on a real app
 * that has none of its own (Calculator), publishing a small image with a real
 * rev.
 *
 * It is deliberately inert unless armed: disarmed it reproduces, character
 * for character, the "no provider" panel AppBridge::refresh() would have
 * produced by itself, so registering it cannot change the answer any other
 * test in this binary gets from GET /api/panel. AppBridge has no way to
 * unregister a provider, which is why that matters.
 *
 * GET /api/apps is the one exception, and it is unavoidable: registering a
 * provider is exactly what makes an app advertise a panel_kind, so once any
 * test below has run, "calculator" carries panel_kind "image" in the app
 * list for the rest of the binary. The apps_json tests therefore never assert
 * anything about calculator — they use apps that no provider anywhere in this
 * binary registers. */
bool g_image_provider_armed = false;
constexpr uint32_t kTestImageRev = 7;

remote::PanelData calculator_image_panel(ui::View&) {
    remote::PanelData p;
    if (!g_image_provider_armed) {
        p.kind = remote::PanelKind::Screen;
        p.screen.message = "No structured data provider for 'Calculator' yet.";
        return p;
    }

    p.kind = remote::PanelKind::Image;
    p.image.app_name = "Calculator";
    p.image.width = 2;
    p.image.height = 1;
    p.image.rev = kTestImageRev;
    p.image.rgb = {255, 0, 0, 0, 255, 0};
    return p;
}

/* Arms the provider for one test and disarms it again however the test ends. */
struct ArmedImageProvider {
    ArmedImageProvider() { g_image_provider_armed = true; }
    ~ArmedImageProvider() { g_image_provider_armed = false; }
};

struct ServerHarness {
    BridgeHarness bridge{};
    remote::RemoteServer server{};

    bool start() {
        remote::AppBridge::instance().register_provider("calculator", remote::PanelKind::Image,
                                                        calculator_image_panel);
        if (!server.start(0)) return false;
        bridge.launch("calculator");
        remote::AppBridge::instance().refresh();
        return true;
    }

    uint16_t port() const { return server.port(); }
};

}  // namespace

TEST(panel_route_sends_the_pixels_when_the_client_says_it_has_nothing) {
    ArmedImageProvider armed;
    ServerHarness h;
    CHECK(h.start());
    if (h.port() == 0) return;

    const std::string body = response_body(http_get(h.port(), "/api/panel"));
    CHECK(has(body, "\"panel_kind\":\"image\""));
    CHECK(has(body, "\"rev\":7"));
    CHECK(has(body, "data_b64"));
}

TEST(panel_route_omits_the_pixels_when_the_client_already_has_that_rev) {
    /* The defect: the route parsed no query at all, so this answered with the
     * full base64 payload every time — ~194 kB per poll for a real APT frame,
     * which is the entire reason `rev` exists. */
    ArmedImageProvider armed;
    ServerHarness h;
    CHECK(h.start());
    if (h.port() == 0) return;

    const std::string body = response_body(http_get(h.port(), "/api/panel?have_image_rev=7"));
    CHECK(has(body, "\"panel_kind\":\"image\""));
    CHECK(has(body, "\"rev\":7"));
    CHECK(!has(body, "data_b64"));
    /* The geometry still goes out: the client needs it to know its canvas is
     * still the right size for the picture it is holding. */
    CHECK(has(body, "\"width\":2"));
}

TEST(panel_route_sends_the_pixels_when_the_clients_rev_is_stale) {
    ArmedImageProvider armed;
    ServerHarness h;
    CHECK(h.start());
    if (h.port() == 0) return;

    const std::string body = response_body(http_get(h.port(), "/api/panel?have_image_rev=6"));
    CHECK(has(body, "\"rev\":7"));
    CHECK(has(body, "data_b64"));
}

TEST(panel_route_treats_a_malformed_have_image_rev_as_having_nothing) {
    /* Same rule as ?after= and ?wait_ms=: the honest fallback is well defined
     * and is not worth a 400. "Client has nothing" is the safe direction —
     * it costs bytes, never a wrong picture. */
    ArmedImageProvider armed;
    ServerHarness h;
    CHECK(h.start());
    if (h.port() == 0) return;

    for (const char* q : {"?have_image_rev=abc", "?have_image_rev=", "?have_image_rev=-7",
                          "?have_image_rev=99999999999999999999"}) {
        const std::string body = response_body(http_get(h.port(), std::string{"/api/panel"} + q));
        CHECK(has(body, "data_b64"));
    }
}

TEST(input_route_counts_only_this_requests_events_as_dropped) {
    ServerHarness h;
    CHECK(h.start());
    if (h.port() == 0) return;

    const std::string body = response_body(http_post(
        h.port(), "/api/input",
        R"({"events":[{"type":"key","key":"up","down":true},)"
        R"({"type":"encoder","delta":2},)"
        R"({"type":"nonsense"}]})"));
    CHECK(has(body, "\"queued\":2"));
    CHECK(has(body, "\"dropped\":1"));

    std::vector<remote::RemoteInput> drained;
    remote::AppBridge::instance().drain_input_queue(drained);
}

TEST(input_route_does_not_report_accepted_events_as_dropped_when_the_queue_is_full) {
    /* The defect: `dropped` folded in the queue's evictions, which discard the
     * OLDEST entries — events from earlier requests that were already, and
     * truthfully, reported as queued. A client that sent five keys and had all
     * five accepted was told all five were refused. */
    ServerHarness h;
    CHECK(h.start());
    if (h.port() == 0) return;

    std::vector<remote::RemoteInput> filler;
    for (size_t i = 0; i < remote::AppBridge::kMaxQueuedInputs; i++) {
        remote::RemoteInput ri;
        ri.action = remote::RemoteInput::Action::Dispatch;
        ri.event.type = host::Event::Type::Encoder;
        ri.event.encoder = 1;
        filler.push_back(ri);
    }
    remote::AppBridge::instance().queue_input(filler);

    const std::string body = response_body(http_post(
        h.port(), "/api/input", R"({"events":[{"type":"key","key":"select","down":true}]})"));
    CHECK(has(body, "\"queued\":1"));
    CHECK(has(body, "\"dropped\":0"));

    std::vector<remote::RemoteInput> drained;
    remote::AppBridge::instance().drain_input_queue(drained);
}

/* ===========================================================================
 * 5. GET /api/apps carries panel_kind per app
 *
 * The browser's app grid wants to badge which apps have a real native view.
 * It cannot derive that: a panel kind existed only for the app that happened
 * to be OPEN (GET /api/panel, GET /api/apps/current), so app.js's
 * nativePanelKindFor() had nothing to read and drew no badge at all, and any
 * list of "apps with panels" kept on the browser side would be wrong the
 * first time a provider was added or dropped here.
 *
 * The rules these pin, in the order they matter:
 *   - an app with a registered provider advertises that provider's kind;
 *   - an app without one carries no panel_kind KEY at all, not an empty
 *     string (Go's omitempty would drop an "" on the way to the browser
 *     anyway, so a "" that meant something could not survive the hop);
 *   - "screen" is never advertised. Every app can be mirrored as a
 *     framebuffer, so publishing it would badge all ~104 tiles as native.
 * ========================================================================= */

namespace {

/* The one app object out of GET /api/apps' flat array, as raw JSON text. An
 * app entry contains no nested object, so the first '}' after its "id" closes
 * it — which is what makes "this key is absent" checkable against the actual
 * wire text rather than against a struct that could silently default it. */
std::string app_object(const std::string& apps_json, const std::string& id) {
    const std::string key = "{\"id\":\"" + id + "\"";
    const size_t start = apps_json.find(key);
    if (start == std::string::npos) return {};
    const size_t end = apps_json.find('}', start);
    if (end == std::string::npos) return {};
    return apps_json.substr(start, end - start + 1);
}

}  // namespace

TEST(apps_json_advertises_the_panel_kind_each_provider_registered) {
    const std::string apps = remote::AppBridge::instance().apps_json();

    /* One app per distinct panel kind that a provider registers, so a single
     * kind wired up wrongly cannot hide behind the others. */
    const std::pair<const char*, const char*> expected[] = {
        {"adsbrx", "adsb"},        {"ais", "ais"},          {"pocsag", "console"},
        {"aprsrx", "geotable"},    {"noaaapt_rx", "image"}, {"wardrivemap", "map"},
        {"ert", "table"},          {"audio", "receiver"},   {"lookingglass", "spectrum"},
    };
    for (const auto& [id, kind] : expected) {
        const std::string obj = app_object(apps, id);
        CHECK(!obj.empty());
        if (obj.empty()) continue;
        CHECK(has(obj, std::string{"\"panel_kind\":\""} + kind + "\""));
    }
}

TEST(apps_json_omits_the_panel_kind_key_for_an_app_with_no_provider) {
    /* FM Radio and AFSK RX have no src/remote/provider_*.cpp, and nothing in
     * this test binary registers one for them (the only test-registered
     * provider is on Calculator). Absent must stay absent: the badge is drawn
     * from the key's presence, so an empty string here would claim a native
     * view that does not exist. */
    const std::string apps = remote::AppBridge::instance().apps_json();

    for (const char* id : {"fmradio", "afsk_rx"}) {
        const std::string obj = app_object(apps, id);
        CHECK(!obj.empty());
        if (obj.empty()) continue;
        /* The whole key, not just the value: "panel_kind":"" would pass a
         * check for the absence of a kind name and still be on the wire. */
        CHECK(!has(obj, "panel_kind"));
        /* ...while the rest of the entry is unchanged. */
        CHECK(has(obj, "\"hardware_limited\":"));
    }
}

TEST(apps_json_never_advertises_the_screen_panel_kind) {
    /* PanelKind::Screen is the framebuffer mirror EVERY app has. It is a
     * legitimate answer on GET /api/panel and never a legitimate one here. */
    const std::string apps = remote::AppBridge::instance().apps_json();
    CHECK(!has(apps, "\"panel_kind\":\"screen\""));
}

TEST(app_summary_json_omits_panel_kind_unless_the_app_really_has_one) {
    /* The serializer on its own, with no bridge and no registry: this is the
     * gate the "never advertise screen" rule lives behind, so it is pinned
     * where it is implemented as well as through the endpoint. */
    remote::AppSummary s;
    s.id = "fmradio";
    s.name = "FM Radio";
    s.category = "Receive";
    s.icon_name = "fmradio";

    CHECK(!has(remote::to_json(s).dump(), "panel_kind"));

    /* A provider that declares Screen is saying "no native view"; publishing
     * that literally would badge the tile. */
    s.panel_kind = remote::PanelKind::Screen;
    CHECK(!has(remote::to_json(s).dump(), "panel_kind"));

    /* A real kind goes out under the wire key Go and the browser read. */
    s.panel_kind = remote::PanelKind::GeoTable;
    CHECK(has(remote::to_json(s).dump(), "\"panel_kind\":\"geotable\""));
}

/* --- Panel payload field names ARE the contract ------------------------------
 *
 * The spectrum panel showed an empty axis reading 0.000 MHz on every machine
 * while 1024 real bins flowed underneath it: the emitter had invented its own
 * dialect (centre_hz/span_hz/mode) while the renderer read the names
 * PANELS.md documents (center_hz/sample_rate_hz/type/floor_db/ceil_db), and
 * the harness fixtures — hand-written to the DOCUMENTED shape — rendered
 * beautifully. Both halves green, seam broken: the same failure mode
 * contract_test.go's header describes for the envelope, one level down.
 * These pin the C++ emission to the documented names so the dialect cannot
 * come back. */

TEST(spectrum_payload_speaks_the_documented_dialect) {
    remote::SpectrumData s;
    s.centre_hz = 446'000'000;
    s.span_hz = 2'000'000.0;
    s.bins_db = {-91.5f, -80.0f, -101.25f};

    const std::string j = remote::to_json(s).dump();
    CHECK(j.find("\"type\":\"spectrum\"") != std::string::npos);
    CHECK(j.find("\"center_hz\":") != std::string::npos);
    CHECK(j.find("\"sample_rate_hz\":") != std::string::npos);
    CHECK(j.find("\"bins_db\":[") != std::string::npos);
    /* Per-frame render scale, min and max of the bins actually sent. */
    CHECK(j.find("\"floor_db\":-101.25") != std::string::npos);
    CHECK(j.find("\"ceil_db\":-80") != std::string::npos);
    /* The dialect must be dead, not merely accompanied. */
    CHECK(j.find("centre_hz") == std::string::npos);
    CHECK(j.find("span_hz") == std::string::npos);
}

TEST(spectrum_payload_with_no_bins_is_an_idle_frame) {
    remote::SpectrumData s;
    s.centre_hz = 446'000'000;
    s.span_hz = 2'000'000.0;

    const std::string j = remote::to_json(s).dump();
    CHECK(j.find("\"type\":\"idle\"") != std::string::npos);
    CHECK(j.find("\"reason\":") != std::string::npos);
    /* An idle frame claims nothing it does not have. */
    CHECK(j.find("center_hz") == std::string::npos);
    CHECK(j.find("floor_db") == std::string::npos);
}

TEST(receiver_payload_carries_the_documented_meter_and_bounds) {
    remote::ReceiverData r;
    r.mode = "NFM";
    r.frequency_hz = 446'006'250;
    r.gain_db = 32.0;
    r.channel_level_db = -46.5f;
    r.gain_min_db = 0.0;
    r.gain_max_db = 76.0;
    r.gain_range_valid = true;

    const std::string j = remote::to_json(r).dump();
    /* level_db is what the renderer's meter reads (PANELS.md); its absence
     * is a meter that sits empty forever. */
    CHECK(j.find("\"level_db\":-46.5") != std::string::npos);
    CHECK(j.find("\"level_min_db\":") != std::string::npos);
    CHECK(j.find("\"level_max_db\":") != std::string::npos);
    CHECK(j.find("\"gain_min_db\":0") != std::string::npos);
    CHECK(j.find("\"gain_max_db\":76") != std::string::npos);
    CHECK(j.find("\"squelch_min\":0") != std::string::npos);
    CHECK(j.find("\"squelch_max\":99") != std::string::npos);
    CHECK(j.find("\"volume_max\":99") != std::string::npos);
}

TEST(receiver_payload_omits_gain_bounds_when_no_device_reported_them) {
    remote::ReceiverData r;
    r.mode = "NFM";
    /* gain_range_valid stays false: Range{0,0} is "unknown" in this
     * codebase, and absent bounds render the control read-only rather than
     * inventing a scale. */
    const std::string j = remote::to_json(r).dump();
    CHECK(j.find("gain_min_db") == std::string::npos);
    CHECK(j.find("gain_max_db") == std::string::npos);
}
