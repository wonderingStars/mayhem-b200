/*
 * mayhem-b200 — anonymous usage ping. See telemetry.hpp.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "telemetry.hpp"

#include "file_path.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>

#if defined(_WIN32)
/* WIN32_LEAN_AND_MEAN and NOMINMAX are already set on the compile command line
 * (see CMakeLists.txt), so they are not redefined here. */
#include <windows.h>
#include <winhttp.h>
#endif

namespace core::telemetry {

/* Unset by default: a stock build sends nothing. The maintainer sets this to
 * their own deployed Worker URL (analytics-worker/) to start counting. */
const char* kTelemetryEndpoint = "";

namespace {

std::filesystem::path state_dir() {
    return std::filesystem::path{core::data_directory()};
}

/* A random 128-bit id, hex. Created once and reused; this is the whole of the
 * "who" the count knows. */
std::string load_or_create_install_id() {
    std::error_code ec;
    const auto dir = state_dir();
    std::filesystem::create_directories(dir, ec);
    const auto path = dir / "telemetry_id";

    {
        std::ifstream in(path);
        std::string id;
        if (in && std::getline(in, id)) {
            /* Bounded hex only, never trust a tampered file back onto the wire. */
            if (id.size() == 32 && id.find_first_not_of("0123456789abcdef") == std::string::npos)
                return id;
        }
    }

    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string id;
    id.reserve(32);
    for (int i = 0; i < 32; i++) id.push_back(hex[rd() & 0xF]);

    std::ofstream out(path, std::ios::trunc);
    if (out) out << id << "\n";
    return id;
}

/* Days since the epoch, local. The throttle stamp — one ping per calendar day. */
long today_stamp() {
    const auto now = std::chrono::system_clock::now();
    return static_cast<long>(std::chrono::duration_cast<std::chrono::hours>(now.time_since_epoch()).count() / 24);
}

/* True and records today's stamp if we have NOT pinged today; false otherwise. */
bool claim_today() {
    const auto path = state_dir() / "telemetry_last";
    const long today = today_stamp();
    {
        std::ifstream in(path);
        long last = 0;
        if (in && (in >> last) && last == today) return false;
    }
    std::ofstream out(path, std::ios::trunc);
    if (out) out << today << "\n";
    return true;
}

const char* os_name() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string json_escape_min(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\')
            out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

#if defined(_WIN32)
/* One-shot HTTPS POST via WinHTTP (always present on Windows, TLS handled for
 * us). Everything is best-effort; any failure is swallowed. */
void post_windows(const std::string& url, const std::string& body) {
    std::wstring wurl(url.begin(), url.end());

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[256] = {0}, path[1024] = {0};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 255;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 1023;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return;

    HINTERNET session = WinHttpOpen(L"mayhem-b200",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return;
    /* Short, bounded timeouts: this must never hold anything up. */
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 4000);

    HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
    if (connect) {
        const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET req = WinHttpOpenRequest(connect, L"POST", path, nullptr,
                                           WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (req) {
            const wchar_t* headers = L"Content-Type: application/json\r\n";
            WinHttpSendRequest(req, headers, static_cast<DWORD>(-1),
                               const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                               static_cast<DWORD>(body.size()), 0);
            WinHttpReceiveResponse(req, nullptr);
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
}
#endif

void post(const std::string& url, const std::string& body) {
#if defined(_WIN32)
    post_windows(url, body);
#else
    /* Linux/macOS: shell out to curl if present (nearly always is), fully
     * detached and silent. A build wanting a hard dependency-free path can
     * replace this with libcurl. */
    (void)url;
    (void)body;
    std::string cmd = "curl -s -m 5 -X POST -H 'Content-Type: application/json' -d '" + body +
                      "' '" + url + "' >/dev/null 2>&1";
    /* std::system blocks, but we are already on a detached thread. */
    (void)std::system(cmd.c_str());
#endif
}

}  // namespace

void ping_on_startup(const std::string& version, bool enabled) {
    if (!enabled) return;
    if (kTelemetryEndpoint == nullptr || kTelemetryEndpoint[0] == '\0') return; /* unconfigured build */

    const std::string url = kTelemetryEndpoint;

    try {
        if (!claim_today()) return; /* already counted today */
    } catch (...) {
        return;
    }

    std::string id;
    try {
        id = load_or_create_install_id();
    } catch (...) {
        return;
    }
    if (id.empty()) return;

    const std::string body = std::string{"{\"id\":\""} + json_escape_min(id) +
                             "\",\"version\":\"" + json_escape_min(version) +
                             "\",\"os\":\"" + os_name() + "\"}";

    std::thread([url, body] {
        try {
            post(url, body);
        } catch (...) {
        }
    }).detach();
}

}  // namespace core::telemetry
