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

#### The C++ backend's flat shape

As with `table`, the shape above is what this panel is *designed* around and not what
the C++ backend sends. `src/remote/app_data.cpp`'s `to_json(ConsoleData)` — the funnel
for every `PanelKind::Console` — emits bare strings and nothing else:

```jsonc
{ "lines": ["sync acquired", "POCSAG512 addr=1001001 …"] }
```

no `seq`, no `ts_ms`, no `app_name`, no `max_lines`. Fed here verbatim, `line.text` was
`undefined` on every line, so the panel drew the right *number* of rows, every one of
them blank, and counted them as real content. `console.js` therefore normalizes the
flat shape to `{seq: <array index>, text}`; data already in the documented shape is
returned untouched and still dedupes by its real `seq`.

**No timestamp is synthesized.** `ts_ms` is left absent rather than filled with the
moment the browser polled, and the time column is suppressed for these lines — the
same rule that removed the `table` panel's `Age` column for the flat shape. A line's
real age is unknown, and "now" is a lie about it.

**`seq` is a position, not an identity, and the renderer does not treat it as one.**
`ui::Console` bounds its `std::deque` at `max_lines` (256 by default) and pops the
front once full (`src/ui/ui_widget.cpp`, `Console::write`), so what a poll returns is a
sliding window: after the buffer wraps, index 0 is a different line than it was last
time. Deduping on that index — the obvious reading of "append by `seq`" — would
silently stop showing new lines from the 257th onward, on exactly the long-running
decode session this panel exists for. So the flat shape is diffed by **content**: the
longest tail of what is already on screen that matches the head of the new buffer is
the overlap, and only what follows it is appended. Repeated identical lines resolve
toward the longer match, i.e. toward appending less, which can never duplicate.
A backend that later grows a real monotonic sequence number should send the documented
shape and gets identity-based dedup back for free.

**The four shipping backends send no colour escapes.** `ui::Console` stores them
(the apps write `STR_COLOR_*` inline and the device's `Painter`, not the app, is
what consumes them), but `console_data_from()` — the single reader behind POCSAG,
ACARS, FLEX and Tetra, in `src/remote/provider_acars.cpp` — runs every line through
`strip_color_escapes()` first, including the escapes `Console::write()`'s wrapping
splits across two lines. So no `0x1B` reaches the wire from any panel shipping
today, and `console.js`'s `TERM_COLORS` parser is currently exercised only by the
fixture and by the documented rich shape.

`testdata/console_backend.json` keeps its escapes deliberately: it is the flat
`{"lines":[...]}` envelope carrying the raw text a `ui::Console` holds *before*
stripping, which is what keeps the escape parser covered and what a future backend
forwarding lines verbatim would send. It is wired to its own harness button. Do not
read it as a capture of what the C++ side emits — for that, the escapes come off.

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

A canvas map with an OpenStreetMap street basemap under the markers: pan by drag,
zoom by wheel/buttons, "fit to markers" on first load and on demand, and a lat/lon
graticule over the streets — or instead of them, when there are none. `heading`
(degrees, optional) rotates a marker's arrow glyph; without it, a plain dot. `kind`
picks a colour (`aircraft`/`vessel`/anything else); click a marker for a detail
tooltip.

WardriveMap is the only backend that publishes this kind directly
(`src/remote/provider_wardrive.cpp`); ADS-B and AIS each have their own richer
kind, and APRS, EPIRB RX and Radiosonde RX publish `geotable`, which mounts this
renderer as its upper half. The basemap reaches all of them through that one
renderer: `geotable.js` composes `map.js` and needed no change to gain it.

**The projection is true Web Mercator (EPSG:3857)**, for the reason spelled out at
length in the `adsb` section below: OSM tiles *are* Web Mercator, and the
equirectangular projection this panel used before tiles can be made to agree with
them at exactly one latitude and nowhere else — ~310 m out half a degree from the
centre at UK latitudes, ~1.25 km a degree out, worst zoomed out. Both map panels now
use the same slippy-map formulas, the same continuous zoom mapped onto the tile `z`,
and the same latitude clamp of ±85.05112878.

Tiles come from the portal's own `GET /api/tiles/{z}/{x}/{y}.png` — the endpoint
documented under `adsb`, on the same terms, with no server change: `internal/portal/tiles`
is mounted once on `tiles.Route` and serves whichever panel asks. Root-relative, so
the page never names a host and the portal still works air-gapped. The two OSM tile
usage policy obligations that land on the browser side apply here identically:
the credit is on screen whenever tiles are, and requests are strictly demand-driven
(only tiles the current viewport is about to draw — no prefetch, no warm-up, no
"download this area").

**The degrade is the load-bearing half of this design, because most of the time there
is no basemap at all.** A bench B200 with no internet, an air-gapped install and a
portal started with `-tiles off` (which answers 503) are all normal operating cases,
not failures, so:

- **Markers first, streets when they arrive.** Nothing waits for a tile. The first
  frame is painted before any tile can have loaded and already carries every marker.
- **Any non-200 is just "no basemap".** A failed tile is remembered as failed and
  never retried in a draw loop; once several requests have failed with *nothing* ever
  having loaded, the layer gives up entirely rather than storming a dead endpoint —
  measured at one screenful (12 requests) before it stops asking, after which
  re-rendering, panning and zooming issue none. Giving up is per-panel-instance and
  permanent; a reload is what retries a portal that has since found a network.
- **The fallback is the graticule this panel always drew.** It is drawn on every
  frame, over the tiles at low contrast and alone when there are none, so a tile that
  never arrives leaves the graticule showing through its slot. There is no state in
  which this panel is a half-loaded checkerboard, and none in which it is blank.
- **The whole report is one quiet line**, bottom-right: "Basemap unavailable —
  running offline". No dialog, nothing to dismiss, and the map keeps working around
  it.
- **The OSM credit is shown only on a frame that actually painted a tile.** An
  offline graticule owes OpenStreetMap nothing, and crediting it for a map it did not
  supply would be a false claim about where the picture came from.

Adding the basemap changed no payload: the marker contract below is untouched, and
`testdata/cpp_panel_map.json` and the payload contract test are as they were. What
guards the presentation instead is `server/map_basemap_test.go`, which *runs*
`panels/map.js` under node against a stub DOM
(`server/testdata/map_basemap_probe.js`) and asserts on what it did — which URLs it
asked for (fed back through the proxy's own `tiles.ParsePath`), that markers are on
the first frame before any tile answers, that a dead endpoint is dropped, that the
graticule and the markers survive it, and when the credit appears. It skips where
node is unavailable; the licence and air-gap guards in the same file are plain string
checks and never skip.


#### The C++ backend's marker shape

As with `table` and `console`, the shape above is the renderer's, not the wire's.
`src/remote/app_data.cpp`'s `to_json(MapData)` emits `{lat, lon, label,
heading_deg?}` — the heading named for its unit (as the `adsb` panel names it), and
**no `id` at all**:

```jsonc
{ "app_name": "WardriveMap", "markers": [ { "lat": 51.47, "lon": -0.4543,
  "label": "BT-Hub-4021", "heading_deg": 272 } ] }
```

`panels/map.js` normalizes that in `normalizeMarkers()` before drawing, and every
host of the renderer goes through it. Both differences had teeth:

- reading only `heading` meant a marker that said which way it was pointing was
  drawn as a plain dot. Absent in both spellings still means a dot — never a 0 that
  would read as "due north".
- marker selection is keyed on `id`. With none on the wire every marker compared
  equal, so one click highlighted *all* of them and the tooltip printed the first
  marker's name and coordinates — a real value belonging to a different entity. The
  synthesized key is `label` (the entry's own identity in every app that publishes a
  position) falling back to the array index, and it is never displayed: the tooltip
  prints `label || id`.

Do not re-add that adaptation in a caller. `geotable.js` used to carry its own copy,
which is exactly why the direct `map` kind went without one.

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
underneath them all agreeing with each other. The `map` kind above has the same
basemap and therefore the same requirement, and uses the same formulas.

Tiles are fetched from the portal — by this panel and by `map` above, from the
one endpoint — never straight from the browser to OSM:

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

### `ais`

The vessel equivalent of `adsb`, and the kind AIS RX publishes: a Web Mercator
chart of ship traffic — hull-shaped targets rotated to their heading, six-minute
course/speed vectors, per-MMSI trails and an OpenStreetMap basemap — cross-linked
to a sortable ship table. AIS used to publish `geotable`; that kind is unchanged
and still serves APRS RX and WardriveMap.

```jsonc
{
  "vessels": [
    {
      "mmsi":        "244660320",          // string, ALWAYS present (the key)
      "name":        "EVER GIVEN",         // omitted when the formatted name is empty
      "callsign":    "PBSC",               // omitted when the formatted call sign is empty
      "destination": "ROTTERDAM",          // omitted when the formatted destination is empty
      "lat": 50.7712, "lon": -1.2984,      // BOTH omitted unless the app's own gate passes
      "sog_kn":      12.4,                 // knots; omitted when unavailable (raw 1023)
      "cog_deg":     96.3,                 // degrees; omitted when unavailable (raw 3600)
      "heading_deg": 94,                   // degrees; omitted when raw >= 511
      "nav_status":  0,                    // 0..15; omitted when the app holds -1
      "msgs":        148,                  // received_count
      "time":        "2026-08-14 09:41:02" // last_position.timestamp verbatim; omitted when empty
    }
  ],
  "stats": { "packets_valid": 4213 }       // AISAppView::packets_valid()
}
```

Every field above except `mmsi`, `msgs` and `stats` is optional, and **absent
stays absent — no zeros, no placeholders, no empty-string fields.** That is the
firmest rule in this document and this kind is where it bites hardest, because
every one of these absences is normal traffic rather than an error:

- `lat`/`lon` are emitted **only when the app's own validity gate passes**
  (`pos.latitude.is_valid() && pos.longitude.is_valid()`), in degrees via the
  app's own `ais::format::latlon_float` on `normalized()` values. Both are
  omitted together. A vessel heard through a message 5 (static and voyage data)
  and nothing else has no position at all — it belongs in the table and **not**
  on the chart, and it is emphatically not at 0°N 0°E. Conversely the panel does
  *not* discard a fix of exactly 0, 0: since the wire can never mean "unknown" by
  it, there it can only mean a ship in the Gulf of Guinea.
- `sog_kn` is knots, from a raw field in tenths of a knot. Raw 1023 is "not
  available" and is omitted; raw 1022 means "102.2 knots **or more**" and is sent
  as `102.2`, which is why the panel prints "≥" at exactly that value and nowhere
  else.
- `cog_deg` (course over ground, raw tenths of a degree, 3600 = not available)
  and `heading_deg` (true heading, raw >= 511 = not available) measure different
  things and are separately optional. The panel resolves them **without inventing
  either**: the hull is rotated to `heading_deg` falling back to `cog_deg`, the
  speed vector is drawn along `cog_deg` falling back to `heading_deg`, and a
  vessel that has reported neither is drawn as a **ringed dot with no
  orientation at all** rather than a hull pointing due north. A vector is drawn
  only when there is both a course and a speed.
- `nav_status` is ITU-R M.1371's 0..15. The table prints
  `ais::format::navigational_status`'s own words (the same ones the device's
  screen shows), and the target is coloured by class: under way (0, 8) green,
  anchored or moored (1, 5) amber, hampered or special (2, 3, 4, 6, 7, 11, 12,
  14) red, and neutral for the codes that carry no operational meaning (9, 10,
  13, 15) as well as for a vessel that has never reported one.

`stats.packets_valid` is the only backend statistic. Until one has arrived the
readout leaves it out entirely rather than showing a zero the backend never
claimed; once one has, a later payload that omits it keeps the last figure
(the counter only climbs, so the last one read is a true lower bound, and
blanking it on a malformed tick would lose information rather than add it).
The **"N with position" figure
beside it is counted in the browser** from which vessels arrived with
coordinates — the backend sends no such number — and it is labelled as such on
screen so it cannot be read as one.

Trails, like the ADS-B panel's, are the browser's own: the C++ side keeps no
track history, so they accumulate across polls and start over on reload.
Basemap tiles come from the same `/api/tiles/{z}/{x}/{y}.png` proxy, under the
same OSM tile usage policy obligations documented under `adsb` — attribution
visible whenever tiles are, demand-driven requests only, any non-200 treated
silently as "no basemap".

Two implementation notes that are corrections of the `adsb` panel rather than
copies of it, and should be kept if these two files are ever unified:

- **No rule styles the panel root.** Kind classes are sticky (see
  `panels.css`), so `.mp-adsb`'s root rule — `gap`, `min-height` — keeps
  applying to whatever is rendered into that mount afterwards. This panel puts
  its layout on an inner `.mp-ais-root` element instead, which the next
  renderer's `innerHTML` clear removes.
- **Interaction repaints do not depend on `requestAnimationFrame`.** rAF does
  not run in a hidden tab or a non-compositing pane, which is exactly where
  these panels get verified: with an rAF-only path, toggling units left the
  scale bar reading "2 nm" over a chart redrawn in kilometres. Every repaint
  request goes through a `scheduleDraw` that coalesces on a 0 ms timeout as
  well as on the frame loop.

### `image`

The picture an image-producing RX app is building up — NOAA APT, WeFax, SSTV — at
full resolution in the browser instead of squeezed into the device's 240x320 screen.

```jsonc
{
  "app_name": "SSTV RX",            // optional
  "width": 240, "height": 202,
  "format": "rgb888",               // the only format this panel decodes
  "rev": 202,                       // increments when the image content changes
  "data_b64": "…",                  // base64 of width*height*3 bytes, rows top to bottom;
                                    // OMITTED when the client already has this rev
  "note": "no image decoded yet"    // optional, honest, never fabricated
}
```

The client tells the backend which revision it is already holding by adding
`?have_image_rev=N` to `GET /api/panel` (the portal passes the query through), and the
backend omits `data_b64` when it would only be re-sending pixels the browser has.
**An update without `data_b64` therefore keeps the picture already on screen** — that
is the steady state, not an error.

Drawn into a canvas whose backing store is exactly `width`x`height`, then CSS-scaled to
fit the pane: whole-number multiples when upscaling (so each decoded pixel becomes an
identical square block) and the fractional ratio when the pane is smaller than the
image, with `image-rendering: pixelated` throughout. Smoothing is off deliberately —
these rasters are small and interpolation would invent detail the demodulator never
produced. Re-fitting on container resize changes only the CSS size, never the backing
store, so it costs no redraw and no re-decode. **Save PNG** is `canvas.toDataURL`,
entirely client-side: nothing round-trips and nothing is written on the device.

Nothing is ever synthesized:

- **`rev: 0` with no `data_b64` means nothing has been decoded**, and renders as the
  `note`, never as a black rectangle that reads like a picture of a dark sky. That
  also holds if it arrives *after* the panel had pixels (an app reset): the old
  picture is dropped, because it is no longer of anything.
- **A payload this panel cannot trust is not drawn.** An unrecognised `format`,
  base64 that will not decode, or a byte count that is not exactly
  `width * height * 3` all render as a message saying precisely that. A partial
  image is never drawn from a short payload.
- **The meta line only ever describes pixels actually on the canvas.** If the backend
  announces a new `rev` but sends no bytes for it, the panel keeps the image it has
  and says which rev that is, rather than letting a stale picture pass as current.

Painting happens synchronously inside `render()`, never from `requestAnimationFrame`
— rAF does not fire in a non-composited browser pane, which is where this gets
verified (`panels/adsb.js`'s `drawNow` exists for the same reason).

### `geotable`

For apps whose entries *are* positions — APRS RX, EPIRB RX and Radiosonde RX:
the map above the table it came from, in one panel. AIS published this kind too
until it was given the dedicated `ais` kind above; the fixture below keeps its
ship-shaped sample data, which is still a faithful example of the shape and is
what `harness.html`'s geotable button loads.

```jsonc
{
  "app_name": "AIS Boats",
  "table": { "columns": ["MMSI", "Name/Call"],       // exactly the flat table shape above
             "rows": [["244660320", "EVER GIVEN"]] },
  "map":   { "markers": [
    { "lat": 50.7712, "lon": -1.2984, "label": "EVER GIVEN", "heading_deg": 96, "kind": "vessel" }
  ] }
}
```

`geotable.js` composes rather than draws: it owns two child elements and calls
`MayhemPanels.renderPanel("map", …)` and `renderPanel("table", …)` on them, so there is
still exactly one map renderer and one table renderer in this repo and a fix to either
lands here for free. It strips the two children's card chrome (they are each a full
`.mp-panel` in their own right) and adds the one number neither half can show alone:
**"N of M located"**.

The OpenStreetMap basemap is the worked example of "a fix to either lands here for
free": it was added to `panels/map.js` and this panel's upper half has it, with the
same offline fallback, and not a line of `geotable.js` changed.

That number is the point of the kind. **Markers exist only for entries that have a real
position fix**; an entry without one is in the table and not on the map. A station
heard through a status or telemetry packet only, or a vessel that has broadcast its
name but not its position, is emphatically not at 0°N 0°E — the backend enforces this,
and there is no path in `geotable.js` that derives a marker from a table row.

Each provider's gate is the app's own decode, never a rule invented in the portal:

| app | gate | what has no marker |
| --- | --- | --- |
| APRS RX | `AprsRecentEntry::has_position` | a station heard only through a status or telemetry packet |
| AIS Boats | `Latitude/Longitude::is_valid()` | a vessel that has sent a name but no position report, or the 91/181° sentinel |
| EPIRB RX | `epirb::Location::is_valid()` on a frame that passed its BCH checks, plus a refusal of the exact origin | a protocol that encodes no position, either "not available" sentinel (255 **and** 127 — see below), a frame that failed BCH, 0°/0° |
| Radiosonde RX | `sonde::GPS_data::is_valid()` on the fix the app accepted | a sonde whose GPS has not acquired, or whose GPS block failed its CRC |

Two of those are worth spelling out because the obvious gate is the wrong one:

- **EPIRB.** `Location::is_unknown()` — upstream's test — only checks `degrees >= 255`,
  and only *longitude* is an eight-bit field. Every protocol parses latitude out of a
  **seven**-bit field whose "not available" default is **127**, so a frame carrying it
  passes `is_unknown()` and decodes as 127°N. `is_valid()` (`src/apps/ui_epirb_rx.cpp`)
  rejects both sentinels and range-checks the result; `provider_epirb.cpp` additionally
  refuses an exact 0°/0°, because C/S encodes "unavailable" as all-ones and never as
  zero. A beacon plotted where it is not is worse than one not plotted at all.
- **Radiosonde.** The app's screen position lives in a `ui::GeoPos`, which reads
  0°/0° until a fix arrives — indistinguishable from a real position. So
  `provider_sonde.cpp` publishes `SondeView::fix()`, written only inside the app's own
  `GPS_data::is_valid()` branch (the same branch that moves the device's map marker),
  and applies that test again to the value it is about to send.

Two adaptations happen between the contract's marker and the one `map.js` draws, and
they are the only logic in the file:

- `heading_deg` → `heading`. A marker with no heading keeps none, and `map.js` then
  draws a dot rather than an arrow — never defaulted to `0`, which would read as
  "heading due north".
- An `id` is synthesized (from the label, falling back to the array index) purely so
  `map.js`'s click-to-select can tell markers apart; with `id` undefined every marker
  compares equal and one click selects all of them. It is a rendering key and is never
  displayed as data.

### `screen` (fallback)

```jsonc
{ "app_name": "Jammer", "category": "Transmit", "reason": "no structured view yet" }
```

An honest card, not an error page: names the app, shows its category if given, and
says plainly that it has no structured web view yet. This is also what
`renderPanel` falls back to for an unregistered `kind`, with a synthesized `reason`.

## Panels versus the live screen view

`server/static/screenview.js` — the live 240x320 framebuffer streamed over
`ws://…/api/screen/ws` as `MBSF` frames, with key/encoder/touch/char events posted
back — **is not a panel and has no panel kind.** The distinction is what each one is a
view *of*:

- A **panel** renders one app's *published data*: a provider on the C++ side reads
  state the app already keeps and shapes it into the JSON above. It exists only for
  the apps that have a provider, and it is a browser-native view — sortable, sized to
  the window, searchable — of information the device also draws in its own way.
- The **screen view** renders *the device itself*. Every app draws into the same
  framebuffer and every input funnels through `app::EventDispatcher::dispatch`, so
  streaming that framebuffer out and routing input back operates all 94 apps, whether
  or not any of them has a provider.

They are complements, not alternatives: the screen view is always available and always
faithful; a panel is available for fewer apps and is better where the browser genuinely
beats a 240x320 screen (full-resolution images, searchable text streams, maps). Nothing
in `panels/` talks to the screen socket, and `screenview.js` registers nothing with
`MayhemPanels`, which is why `TestEmbeddedAssets_PanelScriptsAreWiredUp` — a
`panels/*.js` ↔ `index.html` check in both directions — does not cover it; its
`<script>` tag in `index.html` sits outside the `panels/` block and is commented as
such.

Note that the `screen` **panel kind** above is a different thing entirely with an
unfortunately similar name: it is the static "this app has no structured web view yet"
card, and it is also `renderPanel`'s fallback for an unregistered kind. It has not been
repurposed.

## Fixtures

`internal/portal/testdata/*.json` has at least one fixture per kind (more where a kind
has genuinely distinct states: `spectrum` active/idle, `image` decoded/cache-hit/
nothing-decoded, `ais` populated/nothing-heard, and the C++ backend's own flat shapes
for `table` and `console`), matching the shapes above exactly. `harness.html` (see below) is driven entirely from
these files — there is no other source of demo data — so they double as the contract's
executable documentation. (`TestFixturesAreServedAsIs` in `cmd/panels-harness` pins a
named subset of them; fixtures added since, `adsb.json` included, are not in that list.)

`testdata/image.json` carries a real 240x202 RGB888 payload (an SSTV-style test
pattern, at the size `SstvImage` uses on the device) rather than a placeholder, because
a base64 image fixture with no bytes in it would exercise none of the decode, the size
check or the fit maths. `testdata/image_cached.json` is the same app at the same `rev`
with `data_b64` omitted — load `image` first, then this, to see the panel hold the
picture through a cache hit — and `testdata/image_none.json` is `rev: 0` with a note.

## Standalone harness

`cmd/panels-harness` (`go run ./cmd/panels-harness`, default `http://127.0.0.1:8091/`)
serves `internal/portal` from disk (no build step, no embedding) so `harness.html` can
load `server/static/panels/*.js` and `testdata/*.json` over real HTTP — `fetch()` of
`file://` pages is blocked by browsers, so this is the difference between "the panels
demonstrably work" and "trust me". It is a dev-only tool: apart from
`/api/tiles/{z}/{x}/{y}.png` — which it serves to the same contract as the real server,
so the `adsb` and `map` basemaps behave identically in both, and `-tiles off` is how
the offline fallback is exercised in a browser — it implements none of
`internal/portal/server`'s real endpoints (`/api/apps`, `/api/panel`, ...) and is not
meant to. Pick a kind from the sidebar to load its fixture; "Simulate live updates"
re-renders on a timer with the fixture perturbed (spectrum bins jittered, table rows
aged/added, console lines appended) to exercise the diffing/highlight/autoscroll paths
a single static render can't show.

Three of those simulations exist to reproduce a specific failure rather than to look
busy, and are worth keeping that way:

- **`console (C++ backend shape)`** appends a line per tick into a deliberately tiny
  8-line window that slides, i.e. the wrap `ui::Console` does at `max_lines`. If new
  lines stop appearing once the window first fills, the normalizer has regressed to
  deduping by array index.
- **`image`** alternates between sending `data_b64` and omitting it at the same `rev`,
  which is exactly what the `have_image_rev` negotiation produces. The picture must
  not flicker or blank on the omitted ticks.
- **`geotable`** drifts the located contacts and, every few ticks, gives one of the
  entries that has no position fix its first one, so "4 of 6 located" climbs to
  "6 of 6" while the row count stays at 6 — the absent-position rule, visible.
- **`ais`** steams every located vessel along its own COG at its own SOG (so
  trails accumulate and the six-minute vectors stay attached to real motion) and,
  every few ticks, gives one of the contacts that has never reported a position
  its first fix: the vessel count must not move while "N with position" climbs
  and the promoted contact leaves the table-only state. Vessels with no course
  are deliberately not given one — the never-fabricate-an-orientation rule is
  what these ticks are for. `ais (nothing heard)` is the honest empty state.

Panels diff against their own previous state and there is no unmount hook, so loading a
second fixture of the *same* kind is an update, not a reset (that is what makes the
`image` cache-hit button meaningful). Switching kinds clears the state; to see a kind
from scratch again, switch away and back.
