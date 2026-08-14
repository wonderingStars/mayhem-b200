// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// map_basemap_probe.js RUNS panels/map.js -- it does not read it as text.
//
// Every other JS check in this package is a source-level string match, which
// is all Go can do on its own and which cannot tell "the renderer asks the
// portal for tiles, draws them, and keeps drawing markers when it cannot get
// any" from "the file happens to contain the string /api/tiles/". The basemap's
// whole contract is behavioural: which URLs are requested, what is still drawn
// when none of them answer, when the OpenStreetMap credit appears, and whether
// a dead endpoint gets hammered forever. So this loads the real renderer into a
// stub DOM under node, drives it, and prints one JSON report; map_basemap_test.go
// asserts on that report and skips the whole thing when node is absent.
//
// The stubs are deliberately dumb -- record calls, return numbers -- because
// anything cleverer starts being a second renderer to get wrong. Timers are a
// manual queue rather than real ones so the report is deterministic: nothing
// here waits on wall-clock time.
//
// Usage: node map_basemap_probe.js <path to panels/map.js>

"use strict";

const fs = require("fs");
const vm = require("vm");

const MAP_JS = process.argv[2];
if (!MAP_JS) {
  console.error("usage: node map_basemap_probe.js <path to panels/map.js>");
  process.exit(2);
}
const SOURCE = fs.readFileSync(MAP_JS, "utf8");

const VIEW_W = 640;
const VIEW_H = 420;

// The `map` kind's payload, exactly as internal/portal/testdata/map.json spells
// it, plus a second fixture in the C++ backend's spelling (heading_deg, no id)
// to prove normalizeMarkers() still adapts what the wire actually sends.
const FIXTURE = {
  app_name: "WardriveMap",
  markers: [
    { id: "A1B2C3", lat: 51.47, lon: -0.4543, label: "UAL123", heading: 272, kind: "aircraft", detail: "FL350" },
    { id: "4A2E19", lat: 51.512, lon: -0.389, label: "BAW29X", heading: 88, kind: "aircraft" },
    { id: "3C6528", lat: 51.431, lon: -0.521, label: "DLH4YB", heading: 184, kind: "aircraft" },
    { id: "SHIP2", lat: 51.455, lon: -0.47, label: "Vessel 235887", kind: "vessel" },
  ],
};

const WIRE_FIXTURE = {
  app_name: "WardriveMap",
  markers: [
    { lat: 51.47, lon: -0.4543, label: "BT-Hub-4021", heading_deg: 272 },
    { lat: 51.512, lon: -0.389, label: "VM-9F2C" },
  ],
};

// ---------------------------------------------------------------------------
// Stub DOM
// ---------------------------------------------------------------------------

function makeEnv() {
  const timers = [];
  const images = [];
  const created = [];

  function makeCtx() {
    const calls = [];
    const ctx = { calls };
    const rec = (name) => (...args) => { calls.push({ name, args }); };
    for (const m of ["setTransform", "clearRect", "fillRect", "beginPath", "moveTo",
      "lineTo", "stroke", "arc", "fill", "closePath", "save", "restore",
      "translate", "rotate", "fillText", "strokeText", "scale", "clip", "rect"]) {
      ctx[m] = rec(m);
    }
    // drawImage's first argument is an Image stub; record its URL only, so the
    // report stays JSON-serialisable.
    ctx.drawImage = (img, ...rest) => calls.push({ name: "drawImage", args: [img && img.src, ...rest] });
    ctx.measureText = (t) => ({ width: String(t).length * 6 });
    return ctx;
  }

  function makeElement(tag) {
    const el = {
      tagName: String(tag).toUpperCase(),
      className: "",
      textContent: "",
      innerHTML: "",
      children: [],
      style: {},
      dataset: {},
      isConnected: true,
      listeners: {},
      _w: VIEW_W,
      _h: VIEW_H,
      classList: {
        set: new Set(),
        add(...names) { names.forEach((n) => this.set.add(n)); },
        remove(...names) { names.forEach((n) => this.set.delete(n)); },
        contains(n) { return this.set.has(n); },
      },
      appendChild(child) { el.children.push(child); return child; },
      removeChild(child) {
        const i = el.children.indexOf(child);
        if (i >= 0) el.children.splice(i, 1);
        return child;
      },
      addEventListener(type, fn) { (el.listeners[type] || (el.listeners[type] = [])).push(fn); },
      removeEventListener() {},
      getBoundingClientRect() {
        return { left: 0, top: 0, right: el._w, bottom: el._h, width: el._w, height: el._h };
      },
      setPointerCapture() {},
      releasePointerCapture() {},
      dispatch(type, ev) { (el.listeners[type] || []).forEach((fn) => fn(ev)); },
    };
    Object.defineProperty(el, "clientWidth", { get: () => el._w });
    Object.defineProperty(el, "clientHeight", { get: () => el._h });
    if (String(tag).toLowerCase() === "canvas") {
      el.width = 0;
      el.height = 0;
      const ctx = makeCtx();
      el.getContext = () => ctx;
    }
    created.push(el);
    return el;
  }

  class ImageStub {
    constructor() {
      this.src = "";
      this.listeners = {};
      this.fired = false;
      images.push(this);
    }
    addEventListener(type, fn) { (this.listeners[type] || (this.listeners[type] = [])).push(fn); }
    fire(type) {
      if (this.fired) return false;
      this.fired = true;
      (this.listeners[type] || []).forEach((fn) => fn());
      return true;
    }
  }

  const document = {
    createElement: makeElement,
    getElementById() { return null; },
    head: makeElement("head"),
    body: makeElement("body"),
  };

  const registered = {};
  const sandbox = {
    console,
    Math,
    Map,
    Set,
    Array,
    Number,
    String,
    Object,
    JSON,
    Image: ImageStub,
    document,
    setTimeout(fn) { timers.push(fn); return timers.length; },
    clearTimeout() {},
  };
  sandbox.window = sandbox;
  sandbox.devicePixelRatio = 1;
  sandbox.ResizeObserver = class { observe() {} disconnect() {} };
  sandbox.addEventListener = () => {};
  sandbox.MayhemPanels = {
    util: {
      clamp: (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v),
      escapeHtml: (s) => String(s),
    },
    registerPanel(kind, fn) { registered[kind] = fn; },
  };

  const ctx = vm.createContext(sandbox);
  vm.runInContext(SOURCE, ctx, { filename: MAP_JS });

  return {
    window: sandbox,
    document,
    render: registered.map,
    images,
    created,
    makeElement,
    // flushTimers drains the queue, including timers a flushed callback queues,
    // so "a tile arrived -> repaint -> more tiles requested" settles.
    flushTimers(rounds = 8) {
      for (let i = 0; i < rounds && timers.length > 0; i++) {
        const batch = timers.splice(0, timers.length);
        batch.forEach((fn) => fn());
      }
    },
    // Answer every request that has not been answered yet. Returns how many.
    settleImages(type) {
      let n = 0;
      images.forEach((img) => { if (img.src && img.fire(type)) n++; });
      return n;
    },
    requestedURLs() { return images.filter((i) => i.src).map((i) => i.src); },
    findByClass(cls) { return created.find((el) => el.className === cls) || null; },
  };
}

// mount builds a host element and renders the fixture into it once.
function mount(env, fixture) {
  const host = env.makeElement("div");
  env.render(host, fixture);
  return host;
}

// Calls recorded since a mark, as a flat list.
function since(ctx, mark) { return ctx.calls.slice(mark); }

function labelsIn(calls, labels) {
  const want = new Set(labels);
  return calls
    .filter((c) => c.name === "fillText" && want.has(c.args[0]))
    .map((c) => c.args[0]);
}

function drawImageCount(calls) { return calls.filter((c) => c.name === "drawImage").length; }

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------

const MARKER_LABELS = FIXTURE.markers.map((m) => m.label);

// pending: tiles requested, none has answered yet. The markers must already be
// on the canvas and nothing may claim OpenStreetMap supplied a map.
function scenarioPending() {
  const env = makeEnv();
  const host = mount(env, FIXTURE);
  const ctx = host.__mpMap.ctx;
  return {
    tileRequests: env.requestedURLs(),
    labelsDrawn: labelsIn(ctx.calls, MARKER_LABELS),
    drawImages: drawImageCount(ctx.calls),
    attribDisplay: env.findByClass("mp-map-attrib").style.display,
    offlineDisplay: env.findByClass("mp-map-offline").style.display,
  };
}

// online: every tile answers. Tiles are painted, the credit appears, and the
// markers are still drawn on the same frame.
function scenarioOnline() {
  const env = makeEnv();
  const host = mount(env, FIXTURE);
  const ctx = host.__mpMap.ctx;

  env.settleImages("load");
  const mark = ctx.calls.length;
  env.flushTimers();
  const frame = since(ctx, mark);

  return {
    tileRequests: env.requestedURLs(),
    drawImages: drawImageCount(frame),
    labelsDrawn: labelsIn(frame, MARKER_LABELS),
    attribDisplay: env.findByClass("mp-map-attrib").style.display,
    offlineDisplay: env.findByClass("mp-map-offline").style.display,
    // Whether the marker glyphs are painted AFTER the tiles on that frame:
    // drawn underneath, they would be invisible.
    lastDrawImageBeforeFirstMarkerLabel:
      frame.map((c) => c.name).lastIndexOf("drawImage") <
      frame.findIndex((c) => c.name === "fillText" && MARKER_LABELS.includes(c.args[0])),
  };
}

// offline: every tile fails, forever -- an air-gapped portal, or one started
// with -tiles off, which answers 503. The panel must fall back to the graticule
// look, say so once, and STOP ASKING.
function scenarioOffline() {
  const env = makeEnv();
  const host = mount(env, FIXTURE);
  const st = host.__mpMap;
  const ctx = st.ctx;

  const firstFrameRequests = env.requestedURLs().length;
  for (let i = 0; i < 6; i++) {
    env.settleImages("error");
    env.flushTimers();
  }
  const requestsAfterGivingUp = env.requestedURLs().length;

  // Now keep using the map: re-render, pan and zoom. A given-up layer must not
  // issue a single further request.
  const mark = ctx.calls.length;
  env.render(host, FIXTURE);
  st.canvas.dispatch("pointerdown", { clientX: 100, clientY: 100, pointerId: 1 });
  st.canvas.dispatch("pointermove", { clientX: 220, clientY: 180, pointerId: 1 });
  st.canvas.dispatch("pointerup", { clientX: 220, clientY: 180, pointerId: 1 });
  st.canvas.dispatch("wheel", { clientX: 300, clientY: 200, deltaY: -1, preventDefault() {} });
  env.settleImages("error");
  env.flushTimers();
  const frame = since(ctx, mark);

  return {
    firstFrameRequests,
    requestsAfterGivingUp,
    requestsAfterFurtherUse: env.requestedURLs().length,
    gaveUp: st.tilesUnavailable === true,
    labelsDrawn: labelsIn(frame, MARKER_LABELS),
    // The graticule is the fallback map: gridline strokes are still issued.
    strokes: frame.filter((c) => c.name === "stroke").length,
    drawImages: drawImageCount(frame),
    attribDisplay: env.findByClass("mp-map-attrib").style.display,
    offlineDisplay: env.findByClass("mp-map-offline").style.display,
    offlineText: env.findByClass("mp-map-offline").textContent,
  };
}

// partial: some tiles answer and some never will. The failed slots show the
// fallback through them; there is no state where the panel is only a
// half-loaded checkerboard, and the credit is owed for the tiles that did land.
function scenarioPartial() {
  const env = makeEnv();
  const host = mount(env, FIXTURE);
  const ctx = host.__mpMap.ctx;

  let i = 0;
  env.images.forEach((img) => {
    if (!img.src) return;
    img.fire(i % 2 === 0 ? "load" : "error");
    i++;
  });
  const mark = ctx.calls.length;
  env.flushTimers();
  const frame = since(ctx, mark);

  return {
    requested: env.requestedURLs().length,
    drawImages: drawImageCount(frame),
    labelsDrawn: labelsIn(frame, MARKER_LABELS),
    strokes: frame.filter((c) => c.name === "stroke").length,
    gaveUp: host.__mpMap.tilesUnavailable === true,
    attribDisplay: env.findByClass("mp-map-attrib").style.display,
    offlineDisplay: env.findByClass("mp-map-offline").style.display,
  };
}

// markers: the behaviour requirement 3 protects -- the wire's spelling still
// reaches the drawing code, and a click still selects ONE marker.
function scenarioMarkers() {
  const env = makeEnv();
  const host = mount(env, WIRE_FIXTURE);
  const st = host.__mpMap;
  const ctx = st.ctx;

  // heading_deg -> a rotated arrow; no heading at all -> a plain dot.
  const rotations = ctx.calls.filter((c) => c.name === "rotate").map((c) => c.args[0]);
  const arcs = ctx.calls.filter((c) => c.name === "arc").length;

  // Click the SECOND marker. Its label was drawn at (x + 8, y + 4), which is
  // how the probe learns where on the canvas it ended up without duplicating
  // the projection.
  const label = ctx.calls.find((c) => c.name === "fillText" && c.args[0] === "VM-9F2C");
  const x = label ? label.args[1] - 8 : -1;
  const y = label ? label.args[2] - 4 : -1;
  st.canvas.dispatch("pointerdown", { clientX: x, clientY: y, pointerId: 1 });
  st.canvas.dispatch("pointerup", { clientX: x, clientY: y, pointerId: 1 });

  return {
    markerCount: st.markers.length,
    ids: st.markers.map((m) => m.id),
    headings: st.markers.map((m) => (m.heading === undefined ? null : m.heading)),
    rotations,
    arcs,
    clickedAt: [x, y],
    selectedId: st.selectedId,
    tooltipText: env.findByClass("mp-map-tooltip").textContent,
    tooltipDisplay: env.findByClass("mp-map-tooltip").style.display,
    countText: st.countEl.textContent,
  };
}

// fit: "fit to markers" must frame the markers, not just centre on them. The
// probe reports the marker screen positions so the test can check they all
// landed inside the canvas.
function scenarioFit() {
  const env = makeEnv();
  const host = mount(env, FIXTURE);
  const ctx = host.__mpMap.ctx;
  const positions = ctx.calls
    .filter((c) => c.name === "fillText" && MARKER_LABELS.includes(c.args[0]))
    .map((c) => ({ label: c.args[0], x: c.args[1] - 8, y: c.args[2] - 4 }));
  return {
    zoom: host.__mpMap.zoom,
    centerLat: host.__mpMap.centerLat,
    centerLon: host.__mpMap.centerLon,
    viewport: [VIEW_W, VIEW_H],
    positions,
  };
}

// extremes: zoom to the floor, drag the world several times round and up past
// the pole, then zoom to the ceiling. Every step is a place a tile index goes
// out of range and the proxy answers 400 instead of a basemap:
//
//   - dragging west runs the centre longitude past ±180, so the screen slots
//     ask for column indices off both ends of the 2^z grid (the same tile
//     legitimately appears in several slots and the index has to wrap);
//   - dragging north puts the top edge of the canvas past row 0, so the rows
//     above it are off the top of the world entirely;
//   - zooming in runs past the deepest z the endpoint serves.
//
// The counts here are chosen to REACH those conditions from the fixture's fit
// view rather than to look thorough: 60 wheel notches out is more than the
// ~49 that span 11.8 -> 2, 150 back in is more than the ~85 that span 2 -> 19
// even if the floor were missing, and 3600 px of drag is three and a half
// worlds wide at zoom 2. A probe that stops short of the edge tests nothing,
// which is exactly what the first version of this scenario did.
function scenarioExtremes() {
  const env = makeEnv();
  const host = mount(env, FIXTURE);
  const st = host.__mpMap;
  const wheel = (deltaY) => st.canvas.dispatch("wheel", {
    clientX: VIEW_W / 2, clientY: VIEW_H / 2, deltaY, preventDefault() {},
  });

  for (let i = 0; i < 60; i++) wheel(1); // out, to the floor
  const zoomedOut = st.zoom;

  // Drag west and north, well past both edges of the world.
  st.canvas.dispatch("pointerdown", { clientX: 0, clientY: 0, pointerId: 1 });
  for (let i = 1; i <= 12; i++) {
    st.canvas.dispatch("pointermove", { clientX: i * 300, clientY: i * 300, pointerId: 1 });
  }
  st.canvas.dispatch("pointerup", { clientX: 3600, clientY: 3600, pointerId: 1 });
  const pannedCenterLon = st.centerLon;
  const pannedCenterLat = st.centerLat;
  const urlsAtMinZoom = env.requestedURLs().length;

  for (let i = 0; i < 150; i++) wheel(-1); // in, to the ceiling
  const zoomedIn = st.zoom;

  return {
    zoomedOut,
    zoomedIn,
    pannedCenterLon,
    pannedCenterLat,
    urlsAtMinZoom,
    tileRequests: env.requestedURLs(),
  };
}

const report = {
  pending: scenarioPending(),
  extremes: scenarioExtremes(),
  online: scenarioOnline(),
  offline: scenarioOffline(),
  partial: scenarioPartial(),
  markers: scenarioMarkers(),
  fit: scenarioFit(),
};

process.stdout.write(JSON.stringify(report, null, 2) + "\n");
