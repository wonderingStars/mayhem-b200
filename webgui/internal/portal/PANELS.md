# Panel renderers

How the browser draws whatever the currently active mayhem-b200 app has published,
as fetched from `GET /api/panel` (`internal/portal/client`, `Panel{app_id, panel_kind,
title, data}`). This document is the contract between that data and the JS in
`server/static/panels/`.

Nothing here duplicates or re-implements any app's decoder/DSP logic. Every field
below is either already sitting in the app's C++ state (a `RecentEntriesTable`, the
`ReceiverModel`, a console log) or a straightforward JSON-shaping of it. The C++ side
of "publish this as JSON" is not part of this change; this document defines the shape
it should target.

## Status as of this change

`internal/portal/server` (the HTTP handlers that poll mayhem-b200's own API and serve
the browser page) did not exist yet when this started — `internal/portal/client` was
mid-flight in the same tree, and `server/`, `appindex/` and `cmd/mayhem-portal`
appeared over the course of writing this. That shell — `server/static/app.js` — built
its own independent, and simpler, registry: `window.MayhemPortal.registerPanel(kind,
render)`, with built-in `kv` (a read-only key/value grid) and a minimal `table`
(`{columns: string[], rows: string[][]}`, no sorting/selection/highlighting/widths).

Rather than have two disconnected registries, `panels/bridge.js` (load order matters:
last, after `app.js` and every `panels/<kind>.js`) walks `MayhemPanels.kinds()` and
registers each one with `MayhemPortal.registerPanel`, exactly the extension mechanism
`app.js`'s own doc comment invites ("call `MayhemPortal.registerPanel(...)` from a
script loaded after it"). Net effect: `app.js`'s minimal `table` is superseded by the
richer one below (this repo now has exactly one `table` shape — the one documented
here, not the array-of-arrays one `app.js` shipped with), `kv` is untouched (nothing
below defines it), and `spectrum`/`receiver`/`console`/`map`/`screen` are new. See
`index.html`'s script tags for the load order this depends on.

The registry itself (`registry.js`'s `window.MayhemPanels`) stays a real, standalone
implementation, not a stub — usable on its own (as `harness.html` does, with no
`MayhemPortal` in sight), and **keep the `render(el, data)` call signature** if
anything about the integration above changes; every panel file is written against it
and nothing else.

## The registry contract

`server/static/panels/registry.js` defines one global, `window.MayhemPanels`:

```js
MayhemPanels.registerPanel(kind, render)   // called once per panel file, at load time
MayhemPanels.renderPanel(kind, el, data)   // call every time you have (new) data to show
MayhemPanels.renderAuto(el, panel)         // convenience: reads panel.panel_kind itself
MayhemPanels.kinds()                       // -> string[] of registered kinds, for debugging
```

- `render(el, data)` is the entire contract a panel file implements. `el` is a
  container element the panel owns completely (it may freely set `el.innerHTML`,
  attach listeners, etc.) `data` is the **panel-kind-specific payload only** — i.e.
  the `Panel.Data` field already unmarshalled, *not* the `{app_id, panel_kind,
  title, data}` envelope. If the host wants the envelope's `title` shown, either
  put it in `el`'s surrounding chrome (recommended — it's shell-level, not panel-
  level) or, since every kind below treats `title`/`app_name` as an optional
  passthrough field, pass it through inside `data` too.
- **Call `render(el, data)` again for every update** — on a poll tick, a WebSocket
  push, whatever the transport ends up being. There is no separate "mount" vs
  "update" call: every panel diffs its previous state against the new `data` itself
  (keyed by `el`, via `el.__mp*` fields) rather than requiring the host to track
  panel lifecycle. This is why the contract stayed the one-function shape the task
  asked for — see each panel file's top comment for exactly what state it keeps.
- If `el` is reused for a **different** `kind` between calls, `renderPanel` detects
  the change (via `el.dataset.mpKind`) and clears any stashed per-panel state before
  calling the new renderer, so stale DOM/state never leaks across an app switch.
- Unregistered kinds, and any panel called through `renderPanel` that finds
  `el.dataset.mpKind` unset, fall back to the `screen` panel (see below) with a
  synthesized reason — useful during development and an honest default in
  production.
- There is deliberately no `destroy(el)` hook. Panels that register timers (the
  table's and console's live "age" ticking) share one process-wide interval
  (`registry.js`'s `trackAge`) that prunes any tracked node once
  `!node.isConnected`, so a panel swapped out of the DOM cleans itself up within a
  second without the host having to call anything.

## Shared look

`panels/panels.css` defines the palette panels render with — the same dark theme as
`internal/web/static/style.css` (`--mp-bg #0a0e14`, `--mp-panel #121821`, `--mp-accent
#34d8c3`, `--mp-mono`/`--mp-sans` font stacks, etc.), under `--mp-*`-prefixed custom
properties so loading both stylesheets on one page can never clobber the other's
variables. Load `panels.css` once; every panel file assumes its classes exist.

## Panel kinds

### `table`

The big one — ADS-B, AIS, POCSAG, TPMS, ERT, APRS... anything that fills a
`ui::RecentEntriesTable`. Column semantics deliberately mirror
`RecentEntriesColumns` (`src/ui/ui_recent_entries.hpp`): a column `width` of `0` (or
omitted) means "take whatever space is left", exactly like the firmware's
zero-width convention — so a backend adapter walking `RecentEntriesColumns` can
translate it field-for-field.

```jsonc
{
  "title": "ADS-B RX",                    // optional, shown in the panel's own toolbar
  "columns": [
    { "key": "icao", "label": "ICAO", "width": 7, "mono": true },
    { "key": "callsign", "label": "Callsign", "width": 9 },
    { "key": "alt", "label": "Alt (ft)", "width": 7, "align": "right" },
    { "key": "info", "label": "Info", "width": 0 }            // 0 = fill remaining space
  ],
  "rows": [
    {
      "key": "A1B2C3",                    // stable identity; RecentEntries<T>::Key
      "updated_at_ms": 1754300000000,     // epoch ms this entry last changed — drives
                                           // the live "age" column AND new/updated-row
                                           // highlight detection (see below)
      "cells": { "icao": "A1B2C3", "callsign": "UAL123", "alt": "35,000", "info": "Squawk 4102" }
    }
  ],
  "row_count": 42,                        // optional; defaults to rows.length
  "row_limit": 64                         // optional; shows a "capped" hint if row_count >= row_limit
}
```

Column fields: `key` (matches a `cells` key), `label`, `width` (characters; `0`/absent
= flex), `mono` (bool — monospace, for IDs/hex), `align` (`left`|`right`|`center`).
Cell values are always pre-formatted strings — the app already knows whether "35,000"
or "35000 ft" is right for it; the table only lays them out. An `Age` column is
appended automatically from `updated_at_ms` (no explicit column entry needed for it).

#### The C++ backend's flat shape

The shape above is what this panel is *designed* around, but it is not what the C++
backend sends. `src/remote/app_data.cpp`'s `to_json(TableData)` — the single funnel
every `src/remote/provider_*.cpp` publishing a `PanelKind::Table` goes through — emits

```jsonc
{ "columns": ["MMSI", "Name/Call"], "rows": [["244660320", "EVER GIVEN"]] }
```

column labels as bare strings, rows as positional arrays, no per-row `key` and no
`updated_at_ms`. That is the shape `app.js`'s original built-in table renderer was
written against; when `bridge.js` replaced it with this richer one, nothing
reconciled the two, and since the portal passes `data` through untouched
(`client.Panel.Data` is `json.RawMessage`) **every** table panel the backend
published — AIS, BLE, EPIRB, ERT, NRF, Search, SubCar, 2-Tone, Weather and APRS —
rendered with blank headers and empty cells.

`table.js` therefore normalizes the flat shape (`normalize()`); data already in the
documented shape is passed through untouched. Two things are genuinely missing from
the flat shape and are **not** invented:

- **Row identity.** Rows are keyed by position, so the new-row/updated-row highlights
  key on position rather than identity. Keying on the first cell instead would
  collapse two rows that happen to share it, which the device's own screen does not.
- **Age.** There is no timestamp, and the `Age` column would otherwise report every
  row as "now" forever. The column is dropped entirely for this shape rather than
  filled with a value that reads as real.

`testdata/table_backend.json` is a fixture in this shape (cells produced by the AIS
decoder in `tests/test_provider_ais_ble.cpp`), wired to its own harness button and
pinned by `cmd/panels-harness`'s `TestBackendTableFixtureIsTheFlatCppShape`. A
backend that later grows real keys and timestamps should send the documented shape
and gets sorting-by-identity and the age column back for free.

Behaviour: click a header to sort by that column (client-side, over whatever rows are
currently loaded — cycles asc → desc → the server's original order); click a row to
open a detail pane below the table with every cell's full label/value; new keys (not
in the previous `render()` call) get a fading highlight, keys whose `updated_at_ms`
advanced get a subtler one — both purely client-side, so the backend never has to
compute a "new" flag itself. Row DOM nodes are reused and only reordered/patched on
change (keyed by `key`), which is what keeps hundreds of rows smooth without full
virtualization.

### `spectrum`

Reuses the exact JSON shape `internal/web/spectrum.go`'s `SpectrumFrame`/idle frame
already produce — same field names, same units, same "browser never sees raw IQ"
design — because that shape is already proven; there is no reason for a second one.

```jsonc
// a live frame
{ "type": "spectrum", "timestamp_ms": 1754300000000, "center_hz": 446000000,
  "sample_rate_hz": 2000000, "bins_db": [-91.2, -90.8, "... "], "floor_db": -100, "ceil_db": -34 }
// nothing to show yet
{ "type": "idle", "reason": "rx not streaming" }
```

Draws a trace canvas (dB gridlines + auto-scale toggle, exactly like
`internal/web/static/app.js`'s `drawSpectrumLine`) above a scrolling waterfall
(`drawWaterfallRow`, same colour ramp, ported verbatim for visual consistency with the
existing radio GUI) plus a frequency axis strip along the bottom labelled in MHz,
computed from `center_hz ± sample_rate_hz/2`.

### `receiver`

```jsonc
{
  "app_name": "Narrowband FM Audio", "mode": "NFM", "mode_detail": "11k",
  "frequency_hz": 446006250, "frequency_step_hz": 25000,
  "gain_db": 32, "gain_min_db": 0, "gain_max_db": 60, "gain_step_db": 1,
  "squelch": 24, "squelch_min": 0, "squelch_max": 99,
  "volume": 60, "volume_min": 0, "volume_max": 99,
  "level_db": -46.5, "level_min_db": -100, "level_max_db": 0,
  "squelch_open": true, "agc": true, "channel_bandwidth_hz": 11000,
  "controls": {
    "frequency": { "endpoint": "/api/receiver/frequency", "body_key": "hz" },
    "gain": { "endpoint": "/api/receiver/gain", "body_key": "db" },
    "squelch": { "endpoint": "/api/receiver/squelch", "body_key": "level" },
    "volume": { "endpoint": "/api/receiver/volume", "body_key": "level" },
    "agc": { "endpoint": "/api/receiver/agc", "body_key": "on" }
  }
}
```

Large frequency readout (click to edit, MHz, steps by `frequency_step_hz`), gain/
squelch/volume as slider+number pairs, a signal-level meter (`level_db` normalized
into `[level_min_db, level_max_db]`) with a squelch-open badge. Any control whose
`controls.<name>` entry is missing renders **read-only** rather than guessing an
endpoint — most decoder apps have no user-settable frequency, and this is how that
degrades honestly. Every control POSTs `{ "<body_key>": value }` to `endpoint` and
redraws itself from the response, the same "server says what actually took effect"
pattern as `internal/web/static/app.js`'s `RangeControl`.

### `console`

```jsonc
{
  "app_name": "POCSAG", "max_lines": 500,
  "lines": [
    { "seq": 1, "ts_ms": 1754300000000, "text": "POCSAG 512:1001 ALPHA Hello world" }
  ]
}
```

`text` may contain the C++ side's `STR_COLOR_*` escapes (`src/ui/ui.hpp`: `\x1B`
followed by one byte, `0x00`-`0x0F` indexing `term_colors[16]` in `src/ui/ui.cpp`,
`0x10` = `STR_COLOR_FOREGROUND` meaning "reset to the console's default text
colour"). `console.js` parses those bytes into coloured `<span>`s using the *exact*
RGB values from `ui.hpp`'s `Color::` factory functions (e.g. `dark_red()` is
`{159,0,0}`, not a guessed "dark red") — a backend simply forwarding an app's
existing `Console::writeln` string needs no reformatting to look right here.
Monospace, autoscroll (pauses itself if the user scrolls up, with a "N new ↓" pill to
resume), lines are appended/deduped by `seq` so re-sending the whole buffer every poll
is cheap client-side, and the DOM is capped at `max_lines` (default 1000).

### `map`

```jsonc
{
  "app_name": "ADS-B RX",
  "markers": [
    { "id": "A1B2C3", "lat": 51.47, "lon": -0.4543, "label": "UAL123", "heading": 272,
      "kind": "aircraft", "detail": "FL350 · 412 kt · Squawk 4102" }
  ]
}
```

Offline canvas map (no tile service): equirectangular projection with a latitude-
cosine x-correction, a lat/lon graticule that thins out as you zoom in, pan by drag,
zoom by wheel/buttons, "fit to markers" on first load and on demand. `heading`
(degrees, optional) rotates a marker's arrow glyph; without it, a plain dot. `kind`
picks a colour (`aircraft`/`vessel`/anything else); click a marker for a detail
tooltip. Used by ADS-B/APRS today, but the shape is generic to "things with a
position".

### `adsb`

```jsonc
{
  "app_name": "ADS-B RX",
  "home": { "lat": 51.4775, "lon": -0.4614 },   // omitted entirely when unknown
  "stats": { "frames_seen": 48213, "frames_accepted": 9127 },
  "aircraft": [
    { "icao": "4CA2D5", "callsign": "EIN17G",
      "has_pos": true, "lat": 51.5312, "lon": -0.4021,   // lat/lon omitted when has_pos is false
      "altitude_ft": 3175, "on_ground": false,
      "speed_kt": 189, "heading_deg": 268, "vertical_rate_fpm": 1856,
      "squawk": 6041, "messages": 412, "age_s": 1, "rssi": 61,
      "state": "current", "info": "…" }
  ]
}
```

`rssi` is the frame amplitude the app's Amp column shows, not a calibrated
dBm: the smoothed power of a frame's preamble referred to the receiver's own
noise floor, scaled so that the number is roughly twice that power ratio. It is
useful for ranking aircraft against each other and for watching one aircraft
fade, and 0..255 covers about 21 dB of signal above the floor.

A dedicated traffic view for ADS-B RX, in the spirit of globe.adsbexchange.com:
an altitude-coloured map (warm low, cool high — the tar1090 ramp) with aircraft
glyphs rotated to their track and fading position trails, cross-linked to a
sortable table. Clicking a target on either side selects it on the other.
Filters: callsign/ICAO search, altitude band, maximum range, positions-only.
Also range rings and bearing/distance from a receiver position, follow-selected,
imperial/metric toggle, and a pause that freezes the table so a row can be read
without it moving underneath. Preferences and the home position persist in
`localStorage`.

**The projection is true Web Mercator (EPSG:3857), and that is a correctness
requirement rather than a style choice.** Under the traffic there is an
OpenStreetMap basemap, and OSM tiles *are* Web Mercator. An equirectangular plot
can be made to agree with them at exactly one latitude and nowhere else, so
every target is misplaced vertically, by an error that worsens with latitude and
grows quadratically with distance from the map centre: matching scale at 51.6°N,
a contact half a degree north of centre sits ~310 m from the streets it is
actually over, one degree north ~1.25 km. That is not something a constant
offset can absorb — the two projections simply are not the same shape, and
the discrepancy is worst zoomed out, which is exactly the overview a traffic
view is for. So this panel uses the standard slippy-map formulas
(latitude clamped to ±85.05112878 before projecting) and its zoom maps onto the
tile zoom `z`, which is what keeps aircraft, trails, range rings and the tiles
underneath them all agreeing with each other. The `map` kind above stays
equirectangular; it has no tiles to agree with.

Tiles are fetched from the portal, never straight from the browser to OSM:

```
GET /api/tiles/{z}/{x}/{y}.png
  200  the tile, image/png
       Cache-Control: public, max-age=604800
       X-Tile-Cache: hit | miss | revalidated
  400  malformed or out-of-range z/x/y (valid: 0 <= z <= 19, 0 <= x,y < 2^z)
  503  application/json {"error":"tiles_unavailable","message":"…"} — tiles
       disabled by configuration, or upstream unreachable (i.e. offline)
```

The panel treats **any** non-200 as "no basemap": it keeps drawing its graticule
and carries on. A B200 on a bench with no internet is a normal operating case,
not a failure, so that path is silent and must never produce a broken page.

The rest of the tile handling is obligation rather than preference. It comes
from the [OSM tile usage policy](https://operations.osmfoundation.org/policies/tiles/),
which is what permits this to use OSM's servers at all:

- **"© OpenStreetMap contributors" is always visible on the map**, bottom-right
  by convention. Not behind a toggle, not underneath other UI, not off-screen.
- **The tile cache is strictly demand-driven.** It holds only tiles the panel
  actually requested in order to draw them, keeps each for at least the 7 days
  the policy requires, and revalidates with `If-None-Match`/`If-Modified-Since`
  rather than refetching blind. Bulk downloading, pre-seeding an area or a zoom
  level, building tile archives and offline map features are all *prohibited* —
  so there is deliberately no "download this area", no warm-up and no prefetch,
  and none should be added later. The only thing that ever causes a fetch is a
  tile the operator's current view needs in order to be drawn.
- **The upstream request identifies this app**, because library defaults such as
  `Go-http-client/1.1` are explicitly prohibited. The `User-Agent` names the
  application and gives a contact URL:

  ```
  mayhem-b200/0.9.0 (+https://github.com/wonderingStars/mayhem-b200)
  ```

Availability is best-effort with no SLA, and being blocked without notice is
possible — which is the other reason the 503 path above has to be uneventful,
and why a `429`/`403` from upstream stops the proxy contacting it at all for a
minute (doubling on repeats, up to half an hour). Cached tiles carry on being
served while that back-off runs; the panel cannot be relied on to stop asking,
because it only gives up when *nothing* has ever loaded.

`src/remote/provider_adsb.cpp` fills this in from the app's own
`AircraftTracker`; **no ADS-B decoding happens on the web side**, and the app
itself is unmodified. Three things follow from that, and are deliberate:

- **Absent fields are absent, not zero.** `has_pos` is the flag to filter and
  draw on; `lat`/`lon`, `altitude_ft`, the velocity fields and `squawk` are only
  emitted once actually received. An aircraft with no fix must never be drawn at
  0N 0E, and one with no velocity message must not read as stationary heading
  due north.
- **`home` is omitted unless the operator sets one.** There is no GPS on a B200
  and the ported UI never asks for a position, so the panel prompts for one and
  remembers it rather than the backend inventing an origin that would make every
  range and bearing quietly, plausibly wrong.
- **`on_ground` is currently always false.** This port does not decode
  surface-position frames (ME types 5–8; `SURFACE_POS_L`/`H` are declared but
  unused), so there is no trustworthy ground indication to publish. The field
  and its rendering exist so that adding that decoding later needs no protocol
  change.

**Trails are the browser's own**, accumulated across polls, because the C++ side
keeps a trail only for the aircraft whose detail page is open. They start over on
reload; nothing else depends on them.

### `screen` (fallback)

```jsonc
{ "app_name": "Jammer", "category": "Transmit", "reason": "no structured view yet" }
```

An honest card, not an error page: names the app, shows its category if given, and
says plainly that it has no structured web view yet. This is also what
`renderPanel` falls back to for an unregistered `kind`, with a synthesized `reason`.

## Fixtures

`internal/portal/testdata/*.json` has one (or, for `spectrum`, two) fixture per kind,
matching the shapes above exactly. `harness.html` (see below) is driven entirely from
these files — there is no other source of demo data — so they double as the contract's
executable documentation.

## Standalone harness

`cmd/panels-harness` (`go run ./cmd/panels-harness`, default `http://127.0.0.1:8091/`)
serves `internal/portal` from disk (no build step, no embedding) so `harness.html` can
load `server/static/panels/*.js` and `testdata/*.json` over real HTTP — `fetch()` of
`file://` pages is blocked by browsers, so this is the difference between "the panels
demonstrably work" and "trust me". It is a dev-only tool: apart from
`/api/tiles/{z}/{x}/{y}.png` — which it serves to the same contract as the real server,
so the `adsb` basemap behaves identically in both — it implements none of
`internal/portal/server`'s real endpoints (`/api/apps`, `/api/panel`, ...) and is not
meant to. Pick a kind from the sidebar to load its fixture; "Simulate live updates"
re-renders on a timer with the fixture perturbed (spectrum bins jittered, table rows
aged/added, console lines appended) to exercise the diffing/highlight/autoscroll paths
a single static render can't show.
