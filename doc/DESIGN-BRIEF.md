# Design brief — mayhem-b200 web portal

A brief for redesigning the browser interface. Written to be self-contained:
you should not need to read the C++ or ask questions to start.

---

## 1. What this is

**mayhem-b200** is a desktop port of [PortaPack Mayhem](https://github.com/portapack-mayhem/mayhem-firmware),
a well-loved open-source firmware for a handheld software-defined radio. The
original runs on a 240×320 touchscreen you hold in your hand. This port runs
the same ~94 apps on a PC driving a real SDR (an Ettus USRP B200, or any
RTL-SDR / HackRF / Airspy / LimeSDR / PlutoSDR via a companion server).

The web portal is a browser front end to that. It is **not** a marketing site
or a dashboard product — it is an instrument panel for a radio.

**Who uses it:** SDR hobbyists and radio amateurs. Technically literate,
impatient with decoration, and they will notice if a number is wrong. Many
will leave it running for hours on a second monitor while doing something else.

**Two very different things share one interface:**

1. **Native panels** — nine data-rich views built for the browser (a live
   aircraft map, spectrum, sortable decoder tables, images, logs). These are
   where the browser genuinely beats the handheld's tiny screen.
2. **The streamed screen** — a pixel-exact 240×320 mirror of the device's own
   display, with keyboard/mouse driving it. This covers every app that has no
   native panel. It is a fixed-aspect, low-resolution canvas sitting in a large
   browser window.

Reconciling those two — one polished and browser-native, one deliberately
retro and fixed-size — is the central design problem.

---

## 2. Hard constraints (please do not design around these)

These are not preferences. Breaking one means the work cannot ship.

| Constraint | Why |
|---|---|
| **No build step.** Vanilla HTML/CSS/JS only. | The frontend is embedded in a Go binary with `go:embed` and served as static files. No npm, no bundler, no framework, no TypeScript. |
| **No external network at runtime.** | It must work fully offline on an air-gapped machine. No CDN fonts, no Google Fonts, no external CSS or JS, no remote images. Map tiles are proxied through the app's own server. Everything ships in the binary. |
| **`render(el, data)` is the panel contract.** | Every panel is a function taking a container element and a payload. It is called again on every update; there is no mount/update split and no lifecycle hooks. See `webgui/internal/portal/PANELS.md`. |
| **Panel JSON payloads are a frozen wire contract** with the C++ backend. | You may redesign how a payload is *presented*; you may not change what the backend sends. Payload shapes are documented per kind in `PANELS.md`. |
| **Paint on data arrival, not only in `requestAnimationFrame`.** | rAF does not fire in a background or non-composited tab, and these panels are watched passively for long periods. |
| **Absent data must stay absent.** | If a value has not been received, show nothing or an honest note — never a zero, a placeholder, or a synthesised value. An aircraft with no position fix must not appear at 0°N 0°E. This is a firm project rule; several bugs have come from breaking it. |

---

## 3. What exists today

Files, all under `webgui/internal/portal/server/static/`:

```
index.html          the shell
app.css   (14 KB)   shell styling: app grid, top bar, panel chrome
app.js    (28 KB)   grid, search, category filter, panel mounting, polling
screenview.js (26KB) the live 240×320 canvas + input capture
panels/
  registry.js       the panel registry (render(el,data))
  table.js          sortable decoder tables
  adsb.js   (75 KB) aircraft map on OpenStreetMap tiles — the showpiece
  ais.js    (70 KB) vessel chart on the same tiles, for AIS RX
  map.js            generic marker map
  geotable.js       map + table composed
  spectrum.js       FFT / waterfall
  receiver.js       tuning, gain, squelch readout
  console.js        scrolling text logs
  image.js          decoded images (weather satellite, fax, SSTV)
  screen.js         the "no native view yet" fallback card
  panels.css (14KB) the shared palette
```

**The palette** (CSS custom properties, `--mp-*` prefixed so they cannot
collide with the shell's own):

```
--mp-bg #0a0e14   --mp-panel #121821   --mp-panel-2 #161e2a
--mp-border #232d3b   --mp-border-soft #1a222e
--mp-text #dde4ee   --mp-text-dim #8896a8   --mp-text-faint #5b6b80
--mp-accent #34d8c3   --mp-accent-dim #1f8c7f   --mp-accent-2 #5b9dff
--mp-warn #f5a623   --mp-danger #ef5464   --mp-ok #3ddc84
--mp-mono "Cascadia Mono", Consolas, monospace
--mp-sans "Segoe UI", system-ui, Inter, Roboto, sans-serif
--mp-radius 8px
```

Dark only. There is no light theme and no `prefers-color-scheme` handling
anywhere.

**Layout today:** a top status bar (device name, RX/TX indicators, version), a
searchable app grid of 94 tiles grouped into 7 category sections with filter
chips, and a panel view with a back button that replaces the grid when an app
is open.

---

## 4. What is weak, honestly

Not a list of bugs — these are the design problems worth solving.

1. **94 tiles is a wall.** The grid is search-and-filter over a flat list.
   Nothing expresses that ADS-B and a metronome are wildly different things,
   or which apps are useful with the radio currently attached.
2. **The screen view wastes the window.** A 240×320 canvas floats in a large
   browser viewport with dead space around it. That space could carry the
   context the tiny screen cannot: what the app is, what the keys do, current
   tuning.
3. **Keyboard control is undiscoverable.** Arrow keys navigate, Enter selects,
   Escape backs out, the mouse wheel is the rotary encoder, click is touch.
   Nothing on screen says so, and the canvas must be focused first.
4. **Long-running monitoring is unsupported.** The usual real session is "leave
   ADS-B running for two hours on a second monitor". There is one panel at a
   time, no compact mode, no layout persistence.
5. **The status bar is cramped** and a long device name overlaps the title.
6. **Dark only**, and at least one element has already been found below WCAG AA
   contrast. Assume a daylight-desk user exists.
7. **No responsive story.** Untested below desktop width; a phone or tablet on
   the same network is a plausible way to watch a decoder.
8. **Panel chrome is inconsistent** — each renderer grew its own toolbar
   conventions independently.

---

## 5. What "better" means here

In rough priority order:

1. **Legibility at a glance from across a room.** The dominant use is
   monitoring, not interacting. Numbers, states and alerts should read at a
   distance.
2. **Make the app suite navigable.** Help someone find the app they want among
   94, and understand what their radio can actually do (a receive-only dongle
   cannot use the ~28 transmit apps — the backend can report this, the UI
   currently does not show it).
3. **Give the streamed screen a proper home** — a frame that makes a
   fixed-size retro display look intentional rather than orphaned, and teaches
   its controls without a manual.
4. **One coherent visual system** across nine panels that were written
   separately.
5. **Respect the instrument.** This should feel like test equipment, not a
   consumer app. Restraint over decoration. No animation that competes with
   live data.

---

## 6. Deliverables that would be most useful

Ranked by value to us:

1. **A revised `app.css` and `panels.css`** — the palette, type scale, spacing
   and component styling as a coherent system. This alone would lift
   everything, and is the lowest-risk change: pure CSS, no contract touched.
2. **Layout designs** for: the app grid, a panel view, and the screen view with
   its surrounding context. Static mockups are fine; HTML/CSS is better since
   it can be adopted directly.
3. **A light theme** via `prefers-color-scheme`, using the same token names so
   nothing else has to change.
4. **A component sheet** — buttons, chips, toolbars, tables, badges, empty
   states — so the nine panels can converge.

Optional, if you want to go further: a mobile/tablet layout, and a
multi-panel or compact monitoring mode.

---

## 7. How to see it running

```bash
# from the repo root
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build
build/mayhem-b200 --portal=8090          # the app + its API

cd webgui && go run ./cmd/mayhem-portal -http 127.0.0.1:8181 -backend http://127.0.0.1:8090
# then open http://127.0.0.1:8181
```

No radio required — the app runs without one and the portal still serves the
grid and the screen view.

**Better for design work:** there is a standalone panel harness that renders
every panel against realistic fixture data with no radio and no backend:

```bash
cd webgui && go run ./cmd/panels-harness      # http://127.0.0.1:8091/harness.html
```

It has a button per panel kind, including edge cases (a decoded image, an
image with nothing decoded yet, an empty table, an active and an idle
spectrum) and a "simulate live updates" toggle that perturbs the fixture on a
timer. Fixtures live in `webgui/internal/portal/testdata/`. This is the fastest
loop for styling work.

---

## 8. Reading order if you want the detail

- `webgui/internal/portal/PANELS.md` — the panel contract and every payload shape
- `webgui/internal/portal/server/static/app.js` — the shell's own doc comment
- `webgui/internal/portal/server/static/panels/registry.js` — the registry
- `doc/REMOTE-UI.md` — the screen-streaming and input protocol

Licence: GPL-2.0-or-later, like the rest of the project.
