# mayhem-b200 web GUI

A browser-based control panel and live spectrum/waterfall display for any
server that speaks the **sdrlink protocol** — newline-delimited JSON control
messages plus a binary IQ stream, documented in full in
[`../PROTOCOL.md`](../PROTOCOL.md) (or, if you obtained this directory on
its own, alongside the sdrlink server it talks to). It is part of
[mayhem-b200](https://github.com/wonderingStars/mayhem-b200) and, like the
rest of that project, is free and open source software.

## License

**GPL-2.0-or-later.** Every source file in this directory carries its own
`SPDX-License-Identifier: GPL-2.0-or-later` header. This is true even for
the handful of files (the `internal/web` GUI package and its static
assets) that started life in a sibling project under the MIT license —
their author holds the copyright and has relicensed that code into
mayhem-b200, which is itself GPL-2.0-or-later end to end.

## What this is

`mayhem-webgui` is a single self-contained binary with three parts:

- **`internal/sdrclient`** — a Go client for the sdrlink wire protocol: the
  control connection (hello handshake, every command in PROTOCOL.md
  section 2.1, concurrent in-flight requests matched by id) and the binary
  IQ stream connection (24-byte frame header, cf32/ci16/ci8 sample decoding,
  seq-gap and overflow detection). This is a fresh implementation written
  directly against the published protocol specification — it does not
  depend on any particular server's source code, only on what's on the
  wire.
- **`internal/webadapter`** — glue that makes an `sdrclient.Client` satisfy
  the browser-facing GUI's own `Backend`/`Device` interfaces, including a
  non-blocking fan-out so one slow browser tab can never stall sample
  delivery to any other subscriber (or to the server connection itself).
- **`internal/web`** — the actual browser UI: a dark-themed single page
  app (embedded, no external assets, no build step) with a device panel,
  receiver/transmitter controls, a live spectrum + waterfall display driven
  over WebSocket, and a status panel. The FFT is computed server-side, so
  the browser only ever receives reduced, already-in-dB spectrum bins, not
  raw IQ.

## Running it

```sh
go build -o mayhem-webgui ./cmd/mayhem-webgui
./mayhem-webgui -server 127.0.0.1:5960 -http :8080 -device driver=uhd
```

Flags:

| Flag | Default | Meaning |
|---|---|---|
| `-server` | `127.0.0.1:5960` | `host:port` of the sdrlink control connection to drive. The IQ stream connection is derived as the same host, port+1 (PROTOCOL.md's own reference-server convention; there is no wire command to discover a non-default stream port any other way — mayhem-b200's existing C++ `NetworkRadio` client resolves this the same way). |
| `-http` | `:8080` | Address this web GUI's own HTTP server listens on. Open `http://127.0.0.1:8080/` in a browser once it's running. |
| `-device` | *(empty)* | A device args string (e.g. `driver=uhd,serial=EDR04ZDB2`, or `driver=mock` against a test server) to open automatically at startup. Leave empty to start with nothing open and pick a device from the GUI's own device panel instead — the panel calls the server's `list_devices`/`open` commands for you. |

`mayhem-webgui` does not care what produced the sdrlink server it's
pointed at: the reference `sdrlink-server` in this project's sibling
repository, someone else's independent implementation of PROTOCOL.md, or a
test harness serving a mock device. That is the entire point of the
protocol being an open, published specification rather than a private
wire format — **anyone may write their own client or server for it, in any
language, under any licence.**

## mayhem-portal — the app grid

`mayhem-portal` is a second, separate binary: a browser grid of all of
mayhem-b200's ~103 apps (grouped by category, searchable, with a clear
badge on hardware-limited ones), that launches any of them and renders
whatever structured data they publish. It is a JSON proxy in front of
mayhem-b200's own **app-portal HTTP API** (default
`http://127.0.0.1:8090`; `../doc/REMOTE-UI.md` documents that API and the
screen-mirror WebSocket this serves on top of it) — not the
sdrlink protocol `mayhem-webgui` speaks. That's why it's a separate
process rather than a mode of `mayhem-webgui`: the two have no backend in
common, and nothing is gained by merging them. Run both, on different
`-http` ports, against the same running mayhem-b200 instance.

```sh
go build -o mayhem-portal ./cmd/mayhem-portal
./mayhem-portal -backend http://127.0.0.1:8090 -http :8081
```

Flags:

| Flag | Default | Meaning |
|---|---|---|
| `-backend` | `http://127.0.0.1:8090` | Base URL of mayhem-b200's app-portal HTTP API. |
| `-http` | `:8081` | Address this portal's own HTTP server listens on. Open `http://127.0.0.1:8081/` once it's running. |
| `-backend-timeout` | `3s` | Timeout for each request to the app-portal API — bounds how long a browser tab can ever be left waiting if mayhem-b200 is wedged. |
| `-tiles` | `https://tile.openstreetmap.org/{z}/{x}/{y}.png` | Basemap tile URL template, with `{z}`/`{x}`/`{y}` placeholders. `-tiles=off` disables the basemap entirely (the ADS-B map then draws its graticule only). Point it at another provider to use one — but read that provider's usage policy first; the built-in defaults are written for OpenStreetMap's. |
| `-tiles-cache` | *(user cache dir)* | Directory for the on-disk tile cache. Defaults to `<user cache dir>/mayhem-b200/tiles` (`%LOCALAPPDATA%\mayhem-b200\tiles` on Windows, `~/.cache/mayhem-b200/tiles` on Linux). |
| `-tiles-cache-size` | `268435456` (256 MiB) | Size budget in bytes for that directory. Least-recently-used tiles are evicted when it is exceeded. |

`cmd/panels-harness`, the dev-only static server for the panel renderers,
accepts the same three `-tiles` flags and mounts the identical route, so the
ADS-B map can be verified against real coastlines without running the whole
portal.

### The basemap tile proxy

The ADS-B panel's map is drawn in **Web Mercator (EPSG:3857)** — the
projection OpenStreetMap tiles are rendered in. That is not a cosmetic
choice: overlaying Mercator tiles on an equirectangular plot misplaces every
aircraft vertically, by kilometres at UK latitudes over a one-degree span.

Tiles are proxied through `mayhem-portal` rather than fetched by the browser
directly, because `internal/portal/tiles` is where the
[OSM tile usage policy](https://operations.osmfoundation.org/policies/tiles/)
can actually be complied with:

- Every upstream request carries a `User-Agent` naming this application with
  a contact URL. Library defaults are explicitly prohibited by the policy,
  and a browser will not let page script set the header at all.
- Tiles are cached on disk for at least seven days, keyed by z/x/y **and** a
  hash of the URL template, so switching providers cannot serve one
  provider's squares from another's files. Stale entries are re-validated
  conditionally (`If-None-Match`/`If-Modified-Since`), so the usual answer to
  an expired tile is a 304 with no image bytes at all.
- The cache is **strictly demand-driven**: it only ever holds tiles a browser
  actually asked to display. Bulk downloading, pre-seeding areas or zoom
  levels, building tile archives and offline features are prohibited by the
  policy — there is deliberately no prefetch, "download this area" or warm-up
  feature, and none should be added.
- `(c) OpenStreetMap contributors` attribution is displayed on the map itself.
- **A tile server that refuses us is left alone.** A `429 Too Many Requests`
  or a `403` stops all upstream requests for a minute, doubling on each
  repeat up to half an hour, because continuing to knock is what turns a
  temporary rate limit into a permanent block. Cached tiles keep being
  served throughout, so the map does not blank. A plain-`http` template
  pointed at OpenStreetMap's own servers is refused at startup, since the
  policy requires HTTPS there; `http` to any other host (a local mirror, a
  server on the same bench) is still accepted.

Availability is best-effort with no SLA, so every failure degrades quietly:
an unreachable tile server serves a stale cached tile if there is one, and
otherwise returns `503 {"error":"tiles_unavailable"}`, which the panel treats
as "no basemap" and falls back to the graticule it drew before any of this
existed. A missing or blocked tile server can never produce a broken page.

Structure:

- **`internal/portal/client`** — a Go client for mayhem-b200's app-portal
  API (`GET /api/apps`, `GET /api/apps/current`, `POST
  /api/apps/{id}/launch`, `POST /api/apps/home`, `GET /api/panel`, `GET
  /api/status`). Every failure is normalized into a typed `*client.Error`
  that distinguishes "mayhem-b200 isn't reachable at all" from "mayhem-b200
  answered with an error", with a timeout on every call so a wedged or
  absent backend can never hang a caller.
- **`internal/portal/appindex`** — pure, unit-tested logic that groups apps
  by category in canonical (`app::Category`) order. Search-as-you-type and
  the category filter chips are deliberately client-side JS instead
  (`server/static/app.js`), over the already-fetched list — no round trip
  needed for filtering ~103 in-memory items.
- **`internal/portal/tiles`** — the caching basemap tile proxy behind `GET
  /api/tiles/{z}/{x}/{y}.png` (see above): strict validation of z/x/y before
  anything reaches the filesystem or the network, a 7-day on-disk cache with
  conditional revalidation and LRU eviction, bounded upstream concurrency,
  and coalescing so a fast pan cannot fire duplicate requests for the same
  square. Its tests drive it entirely through an injected
  `http.RoundTripper` and clock — no test in this module ever contacts a real
  tile server.
- **`internal/portal/server`** — the HTTP server: the embedded UI plus a
  thin `/api/...` proxy in front of `internal/portal/client` that adds
  category grouping and honest error translation (backend unreachable →
  503 `backend_unavailable`; a real app-level error from mayhem-b200, e.g.
  launching an unknown id → that same status and message, passed through
  unchanged).
- **`internal/portal/server/static`** — the browser UI itself: vanilla
  JS/CSS, no framework, no build step, no external CDN. `app.js` documents
  and exposes the panel-renderer extension point,
  `MayhemPortal.registerPanel(kind, render)` — see that file's top comment
  for the full contract, and `internal/portal/PANELS.md` /
  `server/static/panels/` for the richer renderer library built against it
  (table/spectrum/receiver/console/map, plus an honest fallback).

## Development

```sh
go build ./...
go vet ./...
gofmt -l .        # should print nothing
go test -count=1 ./...
```

Unit tests never require a live sdrlink server: `internal/sdrclient` and
`internal/webadapter`'s tests drive the wire protocol over in-memory
`net.Pipe()` connections (and, for the one test that opens a real IQ
stream socket, a loopback-only `net.Listener`), scripted by the test itself
to stand in for a server. To exercise the whole chain against a real
server, point `-server`/`-device driver=mock` at any sdrlink-protocol
server running its built-in mock/simulated device — no radio hardware
required.

`internal/portal`'s tests are similarly self-contained: `internal/portal/client`'s
tests run an `httptest` server (plus a closed real TCP port for the
"mayhem-b200 is not running" case) rather than needing a real mayhem-b200
process, and `internal/portal/server`'s tests use both an in-package fake
`Backend` and a real `*client.Client` pointed at an `httptest` server (the
latter specifically to exercise the `*client.Error` → HTTP status
translation). To try the whole thing against a real mayhem-b200, just point
`-backend` at its running app-portal API.
