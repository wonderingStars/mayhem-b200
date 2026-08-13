/*
 * mayhem-b200 — screen streaming (GET /api/screen) and remote input
 * (POST /api/input).
 *
 * These are wire-format tests before they are anything else. The C++ side and
 * the Go portal are built from the same written contract but from separate
 * code, and this project has already been bitten by the two drifting apart
 * with both halves' tests green — so the frame header is checked byte by byte
 * and offset by offset here, not by round-tripping it through the encoder that
 * produced it.
 *
 * Three of these guard hangs rather than wrong answers: a long poll that never
 * wakes, and a stop() that waits out a ten-second poll before returning, both
 * fail by taking the test binary hostage. Each therefore runs its blocking half
 * on its own thread with a deadline, so the failure mode is a reported failure
 * and not a wedged CI job.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "test_main.hpp"

#include "remote/app_bridge.hpp"
#include "remote/remote_server.hpp"
#include "ui/display.hpp"

#if defined(_WIN32)
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

namespace {

/* A throwaway loopback client. test_remote_server.cpp has the same handful of
 * primitives, but they live in that file's anonymous namespace and reaching
 * them would mean appending these tests to a file another agent may be editing
 * — so this is a deliberate copy of the small parts needed here, plus the
 * receive timeout the long-poll tests need and that one does not. */
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

    /* Bounds every read in these tests: without it a long poll that fails to
     * wake parks the whole binary in recv() instead of failing an assertion. */
    void set_read_timeout_ms(int ms) {
#if defined(_WIN32)
        const DWORD tv = static_cast<DWORD>(ms);
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        timeval tv{};
        tv.tv_sec = ms / 1000;
        tv.tv_usec = (ms % 1000) * 1000;
        setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    }

    bool send_line(const std::string& s) {
#if defined(_WIN32)
        return ::send(sock_, s.data(), static_cast<int>(s.size()), 0) ==
               static_cast<int>(s.size());
#else
        return ::send(sock_, s.data(), s.size(), MSG_NOSIGNAL) ==
               static_cast<ssize_t>(s.size());
#endif
    }

    /* Reads until the peer closes, which is what "Connection: close" gives us.
     * Binary-safe: a frame body is full of NULs. */
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

/* Body of an HTTP response, i.e. everything past the blank line. Returns an
 * empty string when the response has no header terminator at all. */
std::string response_body(const std::string& response) {
    const auto end = response.find("\r\n\r\n");
    if (end == std::string::npos) return {};
    return response.substr(end + 4);
}

uint16_t read_u16_le(const std::string& s, size_t offset) {
    return static_cast<uint16_t>(static_cast<uint8_t>(s[offset]) |
                                 (static_cast<uint8_t>(s[offset + 1]) << 8));
}

uint32_t read_u32_le(const std::string& s, size_t offset) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[offset])) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[offset + 1])) << 8) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[offset + 2])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(s[offset + 3])) << 24);
}

/* Paints the whole screen and captures it, which is what a real frame is: the
 * fill raises the display's damage generation, capture_screen_frame() notices
 * and publishes. Returns the new sequence number. */
uint32_t paint_and_capture(ui::Color c) {
    host::display.fill_rectangle(host::display.screen_rect(), c);
    remote::AppBridge::instance().capture_screen_frame();
    return remote::AppBridge::instance().screen_frame_seq();
}

/* Nothing else in this binary ever enqueues remote input, but tests share one
 * AppBridge singleton, so each input test starts from a known-empty queue. */
void clear_input_queue() {
    std::vector<remote::RemoteInput> discard;
    remote::AppBridge::instance().drain_input_queue(discard);
}

}  // namespace

/* --- Contract 1: frame layout -------------------------------------------- */

/* MUST be the first test in this file: capture_screen_frame() is called from
 * nowhere else in the test binary (main.cpp is not linked in), so seq is still
 * 0 here and nowhere later. Asserting the precondition rather than skipping on
 * it means a future reordering fails loudly instead of quietly testing nothing. */
TEST(screen_has_no_frame_before_the_first_capture) {
    auto& bridge = remote::AppBridge::instance();
    CHECK_EQ(bridge.screen_frame_seq(), 0u);

    std::string frame;
    CHECK(!bridge.screen_frame(0, 0, frame));
    CHECK(frame.empty());
}

TEST(screen_frame_header_is_byte_exact) {
    auto& bridge = remote::AppBridge::instance();

    const ui::Color colour{0x1234};
    const uint32_t seq = paint_and_capture(colour);
    CHECK(seq != 0u);

    std::string frame;
    CHECK(bridge.screen_frame(0, 0, frame));

    const size_t w = static_cast<size_t>(host::display.width());
    const size_t h = static_cast<size_t>(host::display.height());
    CHECK_EQ(frame.size(), remote::kScreenFrameHeaderBytes + w * h * 2);

    /* Offset by offset, against the contract's own table. */
    CHECK_EQ(frame[0], 'M');
    CHECK_EQ(frame[1], 'B');
    CHECK_EQ(frame[2], 'S');
    CHECK_EQ(frame[3], 'F');
    CHECK_EQ(static_cast<uint8_t>(frame[4]), 1u);  /* version */
    CHECK_EQ(static_cast<uint8_t>(frame[5]), 1u);  /* format: raw RGB565 */
    CHECK_EQ(read_u16_le(frame, 6), static_cast<uint16_t>(w));
    CHECK_EQ(read_u16_le(frame, 8), static_cast<uint16_t>(h));
    CHECK_EQ(read_u32_le(frame, 10), seq);
    CHECK_EQ(read_u16_le(frame, 14), static_cast<uint16_t>(0));

    /* Payload is little-endian RGB565, so the low byte comes first. Checking
     * the first, a middle and the last pixel covers row-major ordering as well
     * as the byte order. */
    CHECK_EQ(read_u16_le(frame, remote::kScreenFrameHeaderBytes), colour.v);
    CHECK_EQ(read_u16_le(frame, remote::kScreenFrameHeaderBytes + (w * h) /* mid */), colour.v);
    CHECK_EQ(read_u16_le(frame, remote::kScreenFrameHeaderBytes + (w * h - 1) * 2), colour.v);
    CHECK_EQ(static_cast<uint8_t>(frame[remote::kScreenFrameHeaderBytes]),
             static_cast<uint8_t>(colour.v & 0xFF));
    CHECK_EQ(static_cast<uint8_t>(frame[remote::kScreenFrameHeaderBytes + 1]),
             static_cast<uint8_t>(colour.v >> 8));
}

/* The seq's whole job is "the display reported damage". A capture with nothing
 * drawn in between must not manufacture a frame, or every long poll in every
 * browser turns into a 60 Hz busy loop on an idle screen. */
TEST(screen_seq_advances_only_on_damage) {
    auto& bridge = remote::AppBridge::instance();

    const uint32_t first = paint_and_capture(ui::Color{0x0821});

    bridge.capture_screen_frame();
    bridge.capture_screen_frame();
    CHECK_EQ(bridge.screen_frame_seq(), first);

    const uint32_t second = paint_and_capture(ui::Color{0xF81F});
    CHECK_EQ(second, first + 1);
}

/* Capture reads the same scroll-mapped view of the framebuffer the window
 * layer composites, and must not consume the window's damage flag doing it —
 * take_damage() still has to report the draw afterwards, or the desktop window
 * goes blank on both platforms while the portal streams happily. */
TEST(screen_capture_leaves_the_window_damage_flag_alone) {
    (void)host::display.take_damage();  /* start from a clean flag */

    host::display.fill_rectangle({0, 0, 8, 8}, ui::Color{0x07E0});
    remote::AppBridge::instance().capture_screen_frame();

    CHECK(host::display.take_damage());
}

TEST(screen_returns_no_content_when_nothing_is_newer) {
    auto& bridge = remote::AppBridge::instance();
    const uint32_t seq = paint_and_capture(ui::Color{0x4208});

    std::string frame;
    /* after == the current seq: there is a frame, but not a newer one. */
    CHECK(!bridge.screen_frame(seq, 0, frame));

    /* And the same with a short wait, which must expire rather than return
     * the frame the caller already has. */
    const auto started = std::chrono::steady_clock::now();
    CHECK(!bridge.screen_frame(seq, 80, frame));
    const auto waited = std::chrono::steady_clock::now() - started;
    CHECK(waited >= std::chrono::milliseconds(60));
    CHECK(waited < std::chrono::seconds(2));
}

TEST(screen_long_poll_wakes_on_the_next_damaged_frame) {
    auto& bridge = remote::AppBridge::instance();
    const uint32_t before = paint_and_capture(ui::Color{0x001F});

    std::atomic<bool> done{false};
    std::atomic<bool> got_frame{false};
    uint32_t seen_seq = 0;
    std::string frame;

    std::thread waiter([&] {
        got_frame.store(bridge.screen_frame(before, 5000, frame));
        if (got_frame.load()) seen_seq = read_u32_le(frame, 10);
        done.store(true);
    });

    /* Long enough that the waiter is genuinely parked on the condition
     * variable rather than racing to it — otherwise this would also pass with
     * no wakeup at all. */
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(!done.load());

    const uint32_t after = paint_and_capture(ui::Color{0xFFE0});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    CHECK(done.load());
    waiter.join();

    CHECK(got_frame.load());
    CHECK_EQ(seen_seq, after);
}

/* --- Contract 1 over HTTP ------------------------------------------------- */

TEST(screen_route_serves_the_frame_as_binary) {
    paint_and_capture(ui::Color{0xAAAA});

    remote::RemoteServer server;
    CHECK(server.start(0));

    TestClient client;
    CHECK(client.connect_to(server.port()));
    client.set_read_timeout_ms(5000);
    CHECK(client.send_line("GET /api/screen HTTP/1.1\r\nHost: x\r\n\r\n"));

    const std::string response = client.read_all();
    CHECK(response.rfind("HTTP/1.1 200 OK", 0) == 0);
    CHECK(response.find("application/octet-stream") != std::string::npos);

    const std::string body = response_body(response);
    CHECK(body.size() == remote::kScreenFrameHeaderBytes +
                             static_cast<size_t>(host::display.width()) *
                                 static_cast<size_t>(host::display.height()) * 2);
    CHECK(body.compare(0, 4, "MBSF") == 0);
    CHECK_EQ(read_u32_le(body, 10), remote::AppBridge::instance().screen_frame_seq());

    server.stop();
}

/* The query string is the whole long poll, and read_request() used to throw it
 * away. If that regresses, ?after=<current seq> reads as a bare /api/screen and
 * this returns 200 with the frame the client already has. */
TEST(screen_route_honours_after_and_answers_204) {
    const uint32_t seq = paint_and_capture(ui::Color{0x5555});

    remote::RemoteServer server;
    CHECK(server.start(0));

    TestClient client;
    CHECK(client.connect_to(server.port()));
    client.set_read_timeout_ms(5000);
    CHECK(client.send_line("GET /api/screen?after=" + std::to_string(seq) +
                           "&wait_ms=50 HTTP/1.1\r\nHost: x\r\n\r\n"));

    const std::string response = client.read_all();
    CHECK(response.rfind("HTTP/1.1 204 No Content", 0) == 0);
    /* A 204 carries no body, so it must not advertise a length either. */
    CHECK(response.find("Content-Length") == std::string::npos);
    CHECK(response_body(response).empty());

    server.stop();
}

/* A wait_ms past the 10 s cap is CLAMPED to it, not rejected. Getting that
 * backwards (falling back to 0 on an out-of-range value) turns a client that
 * asked to block for a minute into one polling flat out — which is exactly
 * what a live --portal did before this was fixed: wait_ms=60000 answered 204
 * in 4 ms. The test cannot afford to time the 10 s ceiling itself, so it
 * proves the useful half: the request really waits, and really wakes. */
TEST(screen_route_clamps_an_over_long_wait_instead_of_ignoring_it) {
    const uint32_t seq = paint_and_capture(ui::Color{0x2222});

    remote::RemoteServer server;
    CHECK(server.start(0));

    /* Everything the reader thread touches is shared-owned, never a stack
     * reference: on the wedged path below the thread is DETACHED and this
     * test returns, and a leaked thread finishing a socket read into a
     * destroyed local is memory corruption that detonates in whatever test
     * runs later — observed as a 1-in-15 segfault in an unrelated jammer
     * test under machine load, 2026-08-13. A leak must be a true leak. */
    auto client = std::make_shared<TestClient>();
    CHECK(client->connect_to(server.port()));
    client->set_read_timeout_ms(8000);
    CHECK(client->send_line("GET /api/screen?after=" + std::to_string(seq) +
                            "&wait_ms=60000 HTTP/1.1\r\nHost: x\r\n\r\n"));

    auto replied_shared = std::make_shared<std::atomic<bool>>(false);
    auto response_shared = std::make_shared<std::string>();
    std::thread reader([client, replied_shared, response_shared] {
        *response_shared = client->read_all();
        replied_shared->store(true);
    });
    auto& replied = *replied_shared;
    auto& response = *response_shared;

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(!replied.load());  /* a dropped wait_ms would have 204'd instantly */

    const uint32_t next = paint_and_capture(ui::Color{0x3333});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!replied.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(replied.load());

    if (replied.load()) {
        reader.join();
        CHECK(response.rfind("HTTP/1.1 200 OK", 0) == 0);
        const std::string body = response_body(response);
        CHECK(body.size() > remote::kScreenFrameHeaderBytes);
        if (body.size() > remote::kScreenFrameHeaderBytes)
            CHECK_EQ(read_u32_le(body, 10), next);
        server.stop();
    } else {
        /* Wedged: stop() first so the handler is released, then leak the
         * reader rather than joining a thread that may never return. */
        server.stop();
        reader.detach();
    }
}

/* A wait_ms that is not a number at all falls back to "do not wait", which is
 * the well-defined default rather than a 400. */
TEST(screen_route_treats_an_unparseable_wait_as_no_wait) {
    const uint32_t seq = paint_and_capture(ui::Color{0x1111});

    remote::RemoteServer server;
    CHECK(server.start(0));

    TestClient client;
    CHECK(client.connect_to(server.port()));
    client.set_read_timeout_ms(5000);
    const auto started = std::chrono::steady_clock::now();
    CHECK(client.send_line("GET /api/screen?after=" + std::to_string(seq) +
                           "&wait_ms=soon HTTP/1.1\r\nHost: x\r\n\r\n"));

    const std::string response = client.read_all();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    CHECK(response.rfind("HTTP/1.1 204 No Content", 0) == 0);
    CHECK(elapsed < std::chrono::seconds(2));

    server.stop();
}

/* --- Contract 2: input ---------------------------------------------------- */

TEST(input_events_of_every_type_round_trip_the_queue) {
    auto& bridge = remote::AppBridge::instance();
    clear_input_queue();

    const std::string body = R"({"events":[
        {"type":"key","key":"up","down":true},
        {"type":"key","key":"back","down":false},
        {"type":"encoder","delta":-3},
        {"type":"touch","x":12,"y":300,"phase":"move"},
        {"type":"char","c":65}
    ]})";

    std::vector<remote::RemoteInput> parsed;
    size_t dropped = 99;
    CHECK(remote::parse_input_events(body, parsed, dropped));
    CHECK_EQ(dropped, static_cast<size_t>(0));
    CHECK_EQ(parsed.size(), static_cast<size_t>(5));

    CHECK_EQ(bridge.queue_input(parsed), static_cast<size_t>(0));

    std::vector<remote::RemoteInput> drained;
    CHECK(bridge.drain_input_queue(drained));
    CHECK_EQ(drained.size(), static_cast<size_t>(5));

    /* A key PRESS is note_key_down() plus a dispatched Event... */
    CHECK(drained[0].action == remote::RemoteInput::Action::KeyDown);
    CHECK(drained[0].event.type == host::Event::Type::Key);
    CHECK(drained[0].event.key == ui::KeyEvent::Up);

    /* ...and a key RELEASE is note_key_up() and no Event, exactly as both
     * window layers do it. Getting this wrong latches remote keys down
     * forever, which key_is_long_pressed() would then believe. */
    CHECK(drained[1].action == remote::RemoteInput::Action::KeyUp);
    CHECK(drained[1].event.key == ui::KeyEvent::Back);

    CHECK(drained[2].action == remote::RemoteInput::Action::Dispatch);
    CHECK(drained[2].event.type == host::Event::Type::Encoder);
    CHECK_EQ(drained[2].event.encoder, -3);

    CHECK(drained[3].action == remote::RemoteInput::Action::Dispatch);
    CHECK(drained[3].event.type == host::Event::Type::Touch);
    CHECK_EQ(drained[3].event.touch.point.x(), 12);
    CHECK_EQ(drained[3].event.touch.point.y(), 300);
    CHECK(drained[3].event.touch.type == ui::TouchEvent::Type::Move);

    CHECK(drained[4].action == remote::RemoteInput::Action::Dispatch);
    CHECK(drained[4].event.type == host::Event::Type::Character);
    CHECK_EQ(drained[4].event.character, static_cast<ui::KeyboardEvent>('A'));

    /* Drained means gone. */
    std::vector<remote::RemoteInput> again;
    CHECK(!bridge.drain_input_queue(again));
}

TEST(input_every_key_name_maps_to_its_switch) {
    struct Case {
        const char* name;
        ui::KeyEvent key;
    };
    const Case cases[] = {
        {"up", ui::KeyEvent::Up},         {"down", ui::KeyEvent::Down},
        {"left", ui::KeyEvent::Left},     {"right", ui::KeyEvent::Right},
        {"select", ui::KeyEvent::Select}, {"back", ui::KeyEvent::Back},
    };

    for (const auto& c : cases) {
        std::vector<remote::RemoteInput> parsed;
        size_t dropped = 0;
        const std::string body =
            std::string{R"({"events":[{"type":"key","key":")"} + c.name + R"(","down":true}]})";
        CHECK(remote::parse_input_events(body, parsed, dropped));
        CHECK_EQ(dropped, static_cast<size_t>(0));
        CHECK_EQ(parsed.size(), static_cast<size_t>(1));
        if (parsed.size() == 1) CHECK(parsed[0].event.key == c.key);
    }
}

TEST(input_unknown_and_malformed_events_are_dropped_and_counted) {
    const std::string body = R"({"events":[
        {"type":"gesture","fingers":2},
        {"type":"key","key":"dfu","down":true},
        {"type":"key","key":"up"},
        {"type":"touch","x":240,"y":10,"phase":"start"},
        {"type":"touch","x":10,"y":320,"phase":"start"},
        {"type":"touch","x":10,"y":10,"phase":"hover"},
        {"type":"char","c":31},
        {"type":"char","c":256},
        {"type":"encoder"},
        "not an object",
        {"type":"key","key":"select","down":true}
    ]})";

    std::vector<remote::RemoteInput> parsed;
    size_t dropped = 0;
    /* Unknown content is never an error: the one good event still lands. */
    CHECK(remote::parse_input_events(body, parsed, dropped));
    CHECK_EQ(parsed.size(), static_cast<size_t>(1));
    CHECK_EQ(dropped, static_cast<size_t>(10));
    if (parsed.size() == 1) {
        CHECK(parsed[0].action == remote::RemoteInput::Action::KeyDown);
        CHECK(parsed[0].event.key == ui::KeyEvent::Select);
    }

    /* The boundaries the cases above sit just outside of are accepted. */
    std::vector<remote::RemoteInput> edges;
    CHECK(remote::parse_input_events(
        R"({"events":[{"type":"touch","x":239,"y":319,"phase":"end"},{"type":"char","c":32},{"type":"char","c":255}]})",
        edges, dropped));
    CHECK_EQ(dropped, static_cast<size_t>(0));
    CHECK_EQ(edges.size(), static_cast<size_t>(3));
}

/* Only a body that is not contract 2 at all is an error — that is the one case
 * the route answers 400 to. */
TEST(input_rejects_a_body_that_is_not_an_events_envelope) {
    std::vector<remote::RemoteInput> parsed;
    size_t dropped = 0;

    CHECK(!remote::parse_input_events("", parsed, dropped));
    CHECK(!remote::parse_input_events("not json", parsed, dropped));
    CHECK(!remote::parse_input_events("[]", parsed, dropped));
    CHECK(!remote::parse_input_events(R"({"evts":[]})", parsed, dropped));
    CHECK(!remote::parse_input_events(R"({"events":{}})", parsed, dropped));

    /* An empty batch is well formed, just empty. */
    CHECK(remote::parse_input_events(R"({"events":[]})", parsed, dropped));
    CHECK_EQ(parsed.size(), static_cast<size_t>(0));
    CHECK_EQ(dropped, static_cast<size_t>(0));
}

/* The queue is bounded like the window's own: oldest out, newest kept, and the
 * evictions reported rather than hidden. */
TEST(input_queue_is_bounded_and_reports_what_it_evicted) {
    auto& bridge = remote::AppBridge::instance();
    clear_input_queue();

    std::vector<remote::RemoteInput> batch;
    for (int i = 0; i < 300; i++) {
        remote::RemoteInput ri;
        ri.action = remote::RemoteInput::Action::Dispatch;
        ri.event.type = host::Event::Type::Encoder;
        ri.event.encoder = i;
        batch.push_back(ri);
    }

    CHECK_EQ(bridge.queue_input(batch), static_cast<size_t>(300 - 256));

    std::vector<remote::RemoteInput> drained;
    CHECK(bridge.drain_input_queue(drained));
    CHECK_EQ(drained.size(), static_cast<size_t>(256));
    /* The survivors are the most recent ones: 44..299. */
    if (drained.size() == 256) {
        CHECK_EQ(drained.front().event.encoder, 44);
        CHECK_EQ(drained.back().event.encoder, 299);
    }
}

TEST(input_route_queues_events_and_reports_the_count) {
    clear_input_queue();

    remote::RemoteServer server;
    CHECK(server.start(0));

    const std::string body =
        R"({"events":[{"type":"key","key":"select","down":true},{"type":"nope"}]})";
    TestClient client;
    CHECK(client.connect_to(server.port()));
    client.set_read_timeout_ms(5000);
    CHECK(client.send_line("POST /api/input HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n" + body));

    const std::string response = client.read_all();
    CHECK(response.rfind("HTTP/1.1 200 OK", 0) == 0);
    CHECK(response.find("\"queued\":1") != std::string::npos);
    CHECK(response.find("\"dropped\":1") != std::string::npos);

    std::vector<remote::RemoteInput> drained;
    CHECK(remote::AppBridge::instance().drain_input_queue(drained));
    CHECK_EQ(drained.size(), static_cast<size_t>(1));

    server.stop();
}

TEST(input_route_rejects_a_malformed_body) {
    remote::RemoteServer server;
    CHECK(server.start(0));

    const std::string body = "{oops";
    TestClient client;
    CHECK(client.connect_to(server.port()));
    client.set_read_timeout_ms(5000);
    CHECK(client.send_line("POST /api/input HTTP/1.1\r\nHost: x\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n" + body));

    CHECK(client.read_all().rfind("HTTP/1.1 400 Bad Request", 0) == 0);
    server.stop();
}

/* --- Shutdown ------------------------------------------------------------- */

/* The regression this guards is a HANG, not a wrong answer. A /api/screen
 * handler may be parked for ten seconds; if stop() does not release it, the
 * process sits there on exit and the client waits on a server that is already
 * gone. Both halves are watched against a deadline so a regression reports a
 * failure instead of wedging the binary. */
TEST(server_stop_releases_an_in_flight_long_poll_promptly) {
    const uint32_t seq = paint_and_capture(ui::Color{0x8410});

    auto* server = new remote::RemoteServer();
    CHECK(server->start(0));

    /* Shared-owned, not stack references: both threads below are DETACHED on
     * the wedged path and this test returns — see the site above for the
     * segfault this caused. A leak must be a true leak, never a dangle. */
    auto client = std::make_shared<TestClient>();
    CHECK(client->connect_to(server->port()));
    client->set_read_timeout_ms(8000);
    /* after == the current seq, so nothing can satisfy this poll on its own;
     * only the cancellation can end it before its 10 s wait. */
    CHECK(client->send_line("GET /api/screen?after=" + std::to_string(seq) +
                            "&wait_ms=10000 HTTP/1.1\r\nHost: x\r\n\r\n"));

    auto replied_shared = std::make_shared<std::atomic<bool>>(false);
    auto response_shared = std::make_shared<std::string>();
    std::thread reader([client, replied_shared, response_shared] {
        *response_shared = client->read_all();
        replied_shared->store(true);
    });
    auto& replied = *replied_shared;
    auto& response = *response_shared;

    /* Let the handler actually reach the wait before stopping. */
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    CHECK(!replied.load());

    auto stopped_shared = std::make_shared<std::atomic<bool>>(false);
    const auto stop_started = std::chrono::steady_clock::now();
    std::thread stopper([server, stopped_shared] {
        server->stop();
        stopped_shared->store(true);
    });
    auto& stopped = *stopped_shared;

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!stopped.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(stopped.load());

    /* Two seconds is generous for "promptly" and still an order of magnitude
     * inside the 10 s the poll asked for, so a lost cancellation cannot pass. */
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!replied.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(replied.load());

    if (stopped.load() && replied.load()) {
        stopper.join();
        reader.join();
        const auto elapsed = std::chrono::steady_clock::now() - stop_started;
        CHECK(elapsed < std::chrono::seconds(5));
        CHECK(response.rfind("HTTP/1.1 204 No Content", 0) == 0);
        delete server;
    } else {
        /* Something is wedged and still holds these; leaking beats crashing
         * the rest of the suite on a destroyed object. */
        stopper.detach();
        reader.detach();
    }
}

/* A cancelled wait must not poison the singleton: the server can be started
 * again, and the next poll has to behave normally. */
TEST(screen_long_poll_works_again_after_a_cancelled_wait) {
    auto& bridge = remote::AppBridge::instance();
    bridge.cancel_screen_waits();

    const uint32_t before = paint_and_capture(ui::Color{0x0400});

    std::string frame;
    /* No wait: unaffected by the cancellation either way. */
    CHECK(bridge.screen_frame(before - 1, 0, frame));

    std::atomic<bool> done{false};
    std::atomic<bool> got{false};
    std::thread waiter([&] {
        std::string f;
        got.store(bridge.screen_frame(before, 5000, f));
        done.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    paint_and_capture(ui::Color{0x0010});

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    CHECK(done.load());
    waiter.join();
    CHECK(got.load());
}
