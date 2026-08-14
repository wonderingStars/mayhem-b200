// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// The "ais" panel: a live vessel-traffic view for AIS RX -- a Web Mercator
// chart with ship-shaped targets, course/speed vectors and trails, cross-
// linked to a sortable ship table. The aircraft equivalent is panels/adsb.js
// and this is written to the same standard; the payload it draws is
// documented in ../../../PANELS.md under `ais`.
//
// This is NOT a second decoder. Every field it draws was decoded by
// src/apps/ais_app.cpp (AISRecentEntry / AISPosition: MMSI, name, call sign,
// destination, position, SOG, COG, true heading, navigational status and the
// received-frame count) and formatted with that app's own ais::format::
// helpers before being published. The only state this file derives for itself
// is presentational, and it says so on screen where it matters:
//
//   - position TRAILS, accumulated client-side across polls. The C++ side
//     keeps no track history at all, so rather than widen the app's state for
//     the web view the browser remembers what it has been told. A reload
//     starts them over; nothing else depends on them.
//   - the "with position" count, which is counted here from the payload and
//     is LABELLED as counted here, because the backend does not send it.
//
// ABSENT STAYS ABSENT. This is the firmest rule in the contract and most of
// the care in this file is spent on it. A vessel that has broadcast a
// position report but no message 5 has no name; one heard through a message 5
// only has no position; SOG 1023, COG 3600 and heading 511 are ITU-R M.1371's
// "not available" sentinels and the backend omits those fields rather than
// sending them. So:
//
//   - a vessel with no position is in the TABLE and NOT on the chart. It is
//     emphatically not at 0N 0E.
//   - a vessel that has never reported an orientation is drawn as an
//     UNROTATED dot, never as a hull pointing due north.
//   - a vessel with no SOG gets no speed vector, and every table cell whose
//     field is absent is left blank rather than filled with 0 or a dash that
//     could be read as data.
//
// Why the styles are injected from here instead of living in panels.css: the
// same reason panels/adsb.js does it -- this panel is self-contained so it can
// be dropped in, and exercised by the harness, without touching the shared
// stylesheet. ensureStyle() appends one <style> to document.head exactly once.
//
// On the projection and tile code being a sibling of adsb.js's rather than a
// shared module: both files are deliberately self-contained (see adsb.js's
// header), and the slippy-map maths here is written from the same standard
// formulas rather than copied blind -- the comments explain each piece where
// AIS makes it behave differently (ships are slow, so the trail threshold is
// metres rather than tens of metres; there is no receiver position in the AIS
// contract, so there are no range rings). If the two are ever unified, they
// have to move together in one change: half a shared module is worse than
// two honest siblings.
//
// Per PANELS.md the whole contract is render(el, data); there is no mount vs
// update and no destroy hook, so all per-instance state hangs off el.__mpAis
// and the redraw loop cancels itself once el leaves the document.

(() => {
  "use strict";

  const { escapeHtml, clamp } = window.MayhemPanels.util;

  // ---------------------------------------------------------------------------
  // Styles
  // ---------------------------------------------------------------------------

  const STYLE_ID = "mp-ais-style";
  const CSS = `
/* NOTHING here styles the panel ROOT, and that is deliberate. Kind classes are
   sticky: app.js renders every panel into the one reused #panelMount and
   nothing removes the previous kind's class, so a mount that has shown this
   panel keeps .mp-ais-panel for the life of the page (panels.css documents the
   same trap). A root rule carrying gap and min-height, as the sibling adsb
   panel's does, therefore goes on distorting whatever is rendered there next.
   All of this panel's layout hangs off .mp-ais-root instead -- an element it
   builds and the next renderer's innerHTML clear removes, so it cannot linger.
   The column-flex and full height the root does need come from .mp-panel. */
.mp-ais-root { display: flex; flex-direction: column; gap: 8px;
  flex: 1; min-height: 420px; min-width: 0; }
.mp-ais-stats { display: flex; flex-wrap: wrap; align-items: baseline; gap: 4px 14px;
  font: 12px var(--mp-sans, sans-serif); color: var(--mp-text-dim, #a09681); }
.mp-ais-stats b { color: var(--mp-text, #ede8db); font-weight: 600; font-variant-numeric: tabular-nums; }
.mp-ais-stats .mp-ais-live { color: var(--mp-ok, #3ddc84); }
.mp-ais-stats .mp-ais-paused { color: var(--mp-warn, #ff9438); }
.mp-ais-stats .mp-ais-derived { opacity: .75; }

.mp-ais-bar { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; }
.mp-ais-bar input[type="search"] {
  background: var(--mp-surface, #161e2a); color: var(--mp-text, #ede8db);
  border: 1px solid var(--mp-border, #2b2820); border-radius: 6px;
  padding: 4px 7px; font: 12px var(--mp-sans, sans-serif); min-width: 150px; }
.mp-ais-bar label { display: inline-flex; align-items: center; gap: 5px;
  font: 12px var(--mp-sans, sans-serif); color: var(--mp-text-dim, #a09681); white-space: nowrap; }
.mp-ais-btn { background: transparent; color: var(--mp-text-dim, #a09681);
  border: 1px solid var(--mp-border, #2b2820); border-radius: 6px; padding: 4px 9px;
  font: 12px var(--mp-sans, sans-serif); cursor: pointer; }
.mp-ais-btn:hover { border-color: var(--mp-accent-edge, rgba(61,245,140,.6));
  color: var(--mp-accent, #3df58c); }
.mp-ais-btn[aria-pressed="true"] { border-color: var(--mp-accent-edge, rgba(61,245,140,.6));
  background: var(--mp-accent-wash, rgba(61,245,140,.12)); color: var(--mp-accent, #3df58c); }

.mp-ais-body { display: grid; grid-template-columns: 1fr;
  grid-template-rows: minmax(240px, 1fr) minmax(150px, auto); gap: 8px; flex: 1; min-height: 0; }
@media (min-width: 900px) {
  .mp-ais-body { grid-template-columns: minmax(0, 3fr) minmax(300px, 2fr); grid-template-rows: 1fr; }
}

.mp-ais-mapwrap { position: relative; min-height: 220px; min-width: 0;
  background: var(--mp-bg, #0a0e14); border: 1px solid var(--mp-border, #2b2820);
  border-radius: var(--mp-radius, 6px); overflow: hidden; }
.mp-ais-mapwrap canvas { display: block; width: 100%; height: 100%; cursor: grab; touch-action: none; }
.mp-ais-mapwrap canvas.mp-ais-dragging { cursor: grabbing; }
.mp-ais-ctl { position: absolute; top: 8px; right: 8px; display: flex; flex-direction: column; gap: 4px; }
.mp-ais-ctl .mp-ais-btn { width: 30px; height: 28px; padding: 0; text-align: center;
  background: rgba(11,10,8,0.85); }
.mp-ais-ctl .mp-ais-btn.mp-ais-wide { width: auto; padding: 0 8px; }
.mp-ais-scale { position: absolute; left: 8px; bottom: 8px; font: 11px var(--mp-mono, monospace);
  color: var(--mp-text-dim, #a09681); background: rgba(11,10,8,0.85); padding: 2px 6px; border-radius: 4px; }
.mp-ais-tip { position: absolute; pointer-events: none; z-index: 3; max-width: 250px;
  background: rgba(11,10,8,0.95); border: 1px solid var(--mp-border, #2b2820);
  border-left: 2px solid var(--mp-accent, #3df58c);
  border-radius: 6px; padding: 6px 8px; font: 11px var(--mp-mono, monospace);
  color: var(--mp-text, #ede8db); box-shadow: var(--mp-shadow, 0 6px 18px rgba(0,0,0,.5)); }
.mp-ais-tip b { color: var(--mp-accent, #3df58c); }
.mp-ais-legend { position: absolute; left: 8px; top: 8px; display: flex; align-items: center;
  flex-wrap: wrap; gap: 3px 9px; max-width: calc(100% - 60px);
  background: rgba(11,10,8,0.85); padding: 3px 6px; border-radius: 4px;
  font: 10px var(--mp-mono, monospace); color: var(--mp-text-dim, #a09681); }
.mp-ais-legend span.mp-ais-key { display: inline-flex; align-items: center; gap: 4px; }
.mp-ais-legend i { display: inline-block; width: 8px; height: 8px; border-radius: 50%; }
/* Attribution sits above every other map overlay (the tooltip included) on
   purpose: the OSM tile usage policy requires it to be visible whenever the
   tiles are, so nothing this panel draws is allowed to cover it. Full-strength
   text, not the dim token, for the same contrast reason adsb.js documents. */
.mp-ais-attrib { position: absolute; right: 6px; bottom: 5px; z-index: 4;
  background: rgba(11,10,8,0.85); padding: 2px 6px; border-radius: 4px;
  font: 10px var(--mp-sans, sans-serif); color: var(--mp-text, #ede8db); }
.mp-ais-attrib a { color: inherit; text-decoration: none; }
.mp-ais-attrib a:hover { text-decoration: underline; }
.mp-ais-offline { position: absolute; right: 6px; bottom: 5px; z-index: 4;
  background: rgba(11,10,8,0.85); padding: 2px 6px; border-radius: 4px;
  font: 10px var(--mp-sans, sans-serif); color: var(--mp-text-dim, #a09681); }

.mp-ais-tablewrap { min-height: 0; overflow: auto; border: 1px solid var(--mp-border, #2b2820);
  border-radius: var(--mp-radius, 6px); background: var(--mp-bg-sunken, #121821); }
table.mp-ais-table { border-collapse: collapse; width: 100%;
  font: 11px var(--mp-mono, monospace); color: var(--mp-text, #ede8db); }
table.mp-ais-table th { position: sticky; top: 0; z-index: 1; text-align: left; white-space: nowrap;
  background: var(--mp-bg-raised, #161e2a); color: var(--mp-text-dim, #a09681);
  border-bottom: 1px solid var(--mp-border, #2b2820); padding: 5px 7px; cursor: pointer;
  font-weight: 600; user-select: none; }
table.mp-ais-table th:hover { color: var(--mp-text, #ede8db); }
table.mp-ais-table th[aria-sort] { color: var(--mp-accent, #3df58c); }
table.mp-ais-table td { padding: 3px 7px; white-space: nowrap;
  border-bottom: 1px solid var(--mp-line-soft, #1c1913); font-variant-numeric: tabular-nums; }
table.mp-ais-table td.mp-ais-num { text-align: right; }
table.mp-ais-table tbody tr { cursor: pointer; }
table.mp-ais-table tbody tr:hover { background: var(--mp-accent-wash-soft, rgba(61,245,140,.05)); }
table.mp-ais-table tbody tr.mp-ais-sel { background: var(--mp-warn-wash, rgba(255,148,56,.10)); }
table.mp-ais-table tbody tr.mp-ais-new td { animation: mp-ais-flash 6s ease-out 1; }
@keyframes mp-ais-flash { from { background: rgba(61,245,140,0.20); } to { background: transparent; } }
table.mp-ais-table td.mp-ais-mmsi { color: var(--mp-accent, #3df58c); }
/* A vessel with no position fix is in the table and off the chart; dimming the
   row is how that reads at a glance without inventing a column for it. */
table.mp-ais-table tbody tr.mp-ais-nopos td { color: var(--mp-text-dim, #a09681); }
.mp-ais-empty { padding: 14px; text-align: center; font: 12px var(--mp-sans, sans-serif);
  color: var(--mp-text-dim, #a09681); }
`;

  function ensureStyle() {
    if (document.getElementById(STYLE_ID)) return;
    const style = document.createElement("style");
    style.id = STYLE_ID;
    style.textContent = CSS;
    document.head.appendChild(style);
  }

  // ---------------------------------------------------------------------------
  // Navigational status
  //
  // The labels are src/apps/ais_app.cpp's own ais::format::navigational_status
  // strings, verbatim, so the browser and the device's 240x320 screen say the
  // same words about the same ship. Codes 9, 10 and 13 are "reserved" and 15 is
  // "undefined" in ITU-R M.1371 -- the app prints those too, and they are
  // states rather than absences, so they are shown and coloured neutrally.
  //
  // An ABSENT nav_status (the app holding -1, i.e. nothing ever received) is a
  // different thing again and is left blank everywhere.
  // ---------------------------------------------------------------------------

  const NAV_STATUS_TEXT = {
    0: "under way w/engine",
    1: "at anchor",
    2: "not under command",
    3: "restricted maneuv",
    4: "constrained draught",
    5: "moored",
    6: "aground",
    7: "fishing",
    8: "sailing",
    9: "reserved",
    10: "reserved",
    11: "towing astern",
    12: "towing ahead/along",
    13: "reserved",
    14: "SART/MOB/EPIRB",
    15: "undefined",
  };

  // Three classes plus neutral, as the chart needs -- making way, stopped and
  // made fast, or hampered/special. Every 0..15 code is placed deliberately:
  // 0 and 8 are the two "proceeding" states (engine and sail); 1 and 5 are the
  // two "not going anywhere" states; 2, 3, 4, 6, 7, 11, 12 and 14 are all
  // "treat this vessel with care" states -- restricted in her ability to
  // manoeuvre, constrained by draught, aground, fishing, towing, or a distress
  // beacon; 9, 10, 13 and 15 carry no operational meaning and are neutral, as
  // is a vessel that has never reported one.
  const NAV_CLASS = {
    0: "underway", 8: "underway",
    1: "moored", 5: "moored",
    2: "special", 3: "special", 4: "special", 6: "special",
    7: "special", 11: "special", 12: "special", 14: "special",
    9: "unknown", 10: "unknown", 13: "unknown", 15: "unknown",
  };

  // Palette values from panels.css (--mp-accent, --mp-warn, --mp-danger,
  // --mp-text-dim). Canvas cannot use custom properties, so they are literals
  // here, as adsb.js's altitude ramp is.
  const NAV_COLOR = {
    underway: "#3df58c",
    moored: "#ff9438",
    special: "#ff6a55",
    unknown: "#a09681",
  };

  const NAV_CLASS_LABEL = {
    underway: "under way",
    moored: "anchored / moored",
    special: "restricted / special",
    unknown: "not reported",
  };

  function navClass(status) {
    return NAV_CLASS[status] || "unknown";
  }

  function navColor(v) {
    return NAV_COLOR[navClass(v.nav_status)];
  }

  function navText(status) {
    return Object.prototype.hasOwnProperty.call(NAV_STATUS_TEXT, status)
      ? NAV_STATUS_TEXT[status] : "";
  }

  // ---------------------------------------------------------------------------
  // Geo + formatting helpers
  // ---------------------------------------------------------------------------

  // Ground distances are computed on the WGS84 mean sphere; the projection
  // below is defined on Web Mercator's 6378137 m sphere. The 0.11% difference
  // is right for both jobs -- see the same note in adsb.js.
  const EARTH_KM = 6371.0088;
  const D2R = Math.PI / 180;
  const KM_PER_NM = 1.852;

  // destPoint solves the great-circle direct problem: the point distKm away on
  // the given bearing. Speed vectors are drawn through it so their length is a
  // real distance on the water rather than a number of screen pixels, which is
  // what makes them keep meaning something as the chart is zoomed.
  function destPoint(lat, lon, brgDeg, distKm) {
    const d = distKm / EARTH_KM;
    const br = brgDeg * D2R;
    const phi1 = lat * D2R;
    const sinPhi2 = clamp(Math.sin(phi1) * Math.cos(d) +
      Math.cos(phi1) * Math.sin(d) * Math.cos(br), -1, 1);
    const phi2 = Math.asin(sinPhi2);
    const lam2 = lon * D2R + Math.atan2(
      Math.sin(br) * Math.sin(d) * Math.cos(phi1),
      Math.cos(d) - Math.sin(phi1) * sinPhi2);
    return { lat: phi2 / D2R, lon: lam2 / D2R };
  }

  const COMPASS = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"];
  function compassOf(deg) {
    if (!Number.isFinite(deg)) return "";
    return COMPASS[Math.round(((deg % 360) + 360) % 360 / 22.5) % 16];
  }

  function fmtInt(v) {
    return Number.isFinite(v) ? Math.round(v).toLocaleString() : "";
  }

  // ITU-R M.1371 codes speed over ground in tenths of a knot and caps the
  // scale at raw 1022, which means "102.2 knots or more" rather than exactly
  // 102.2 (raw 1023 is "not available" and the backend omits the field for it).
  // The contract sends that cap as the number 102.2, so this is the one place
  // the panel is entitled to add a qualifier: at the cap it prints "≥", which
  // is what the value means, and at every other value it prints the number.
  const SOG_CAP_KN = 102.2;

  function fmtSpeed(kn, units) {
    if (!Number.isFinite(kn)) return "";
    const capped = kn >= SOG_CAP_KN;
    const v = units.metric ? kn * KM_PER_NM : kn;
    const unit = units.metric ? "km/h" : "kn";
    return `${capped ? "≥" : ""}${v.toFixed(1)} ${unit}`;
  }

  function fmtDeg(deg) {
    return Number.isFinite(deg) ? `${deg.toFixed(1)}°` : "";
  }

  // ---------------------------------------------------------------------------
  // Preferences. localStorage is best-effort: a blocked or full store must
  // never break the panel.
  // ---------------------------------------------------------------------------

  const PREFS_KEY = "mayhem.ais.prefs.v1";
  const DEFAULT_PREFS = {
    metric: false,
    posOnly: false,
    search: "",
    trails: true,
    vectors: true,
    basemap: true,
    follow: false,
    sortKey: "mmsi",
    sortDir: 1,
  };

  function loadPrefs() {
    try {
      const raw = window.localStorage.getItem(PREFS_KEY);
      if (!raw) return Object.assign({}, DEFAULT_PREFS);
      return Object.assign({}, DEFAULT_PREFS, JSON.parse(raw));
    } catch (_) {
      return Object.assign({}, DEFAULT_PREFS);
    }
  }

  function savePrefs(prefs) {
    try {
      window.localStorage.setItem(PREFS_KEY, JSON.stringify(prefs));
    } catch (_) {
      /* private mode / quota -- preferences simply do not persist */
    }
  }

  // ---------------------------------------------------------------------------
  // Projection: true Web Mercator (EPSG:3857), the projection OSM tiles are in.
  //
  // Same requirement as the ADS-B panel and for the same reason: there is a
  // raster basemap underneath, and an equirectangular plot beneath Mercator
  // tiles misplaces every target vertically by an error that grows with
  // latitude. For a vessel chart it would be worse than for aircraft -- a ship
  // is either in the channel or aground, and hundreds of metres of projection
  // error is the difference.
  //
  // Everything works in slippy-map world-pixel space (the world is a
  // TILE_SIZE * 2^zoom square), which is the space the tile grid is indexed in,
  // so targets, trails and tiles cannot disagree. zoom is continuous; the tile
  // layer rounds it to an integer z and scales by 2^(zoom - z).
  // ---------------------------------------------------------------------------

  const TILE_SIZE = 256;
  const MERC_MAX_LAT = 85.05112878;
  const ZOOM_MIN = 2;
  const ZOOM_MAX = 19; // the deepest z the tile endpoint accepts
  const DEFAULT_ZOOM = 10;
  const MPP_EQUATOR_Z0 = 156543.03392;

  function worldSize(zoom) { return TILE_SIZE * Math.pow(2, zoom); }

  function lonToWorldX(lon, zoom) { return (lon + 180) / 360 * worldSize(zoom); }

  function latToWorldY(lat, zoom) {
    const phi = clamp(lat, -MERC_MAX_LAT, MERC_MAX_LAT) * D2R;
    return (1 - Math.log(Math.tan(phi) + 1 / Math.cos(phi)) / Math.PI) / 2 * worldSize(zoom);
  }

  function worldXToLon(x, zoom) { return x / worldSize(zoom) * 360 - 180; }

  function worldYToLat(y, zoom) {
    return Math.atan(Math.sinh(Math.PI * (1 - 2 * y / worldSize(zoom)))) / D2R;
  }

  function metresPerPixel(lat, zoom) {
    return MPP_EQUATOR_Z0 * Math.cos(clamp(lat, -MERC_MAX_LAT, MERC_MAX_LAT) * D2R) /
      Math.pow(2, zoom);
  }

  function normLon(lon) { return ((lon + 180) % 360 + 360) % 360 - 180; }

  // project takes the copy of a longitude NEAREST THE CENTRE, which is what
  // keeps the overlay agreeing with the tile layer across the antimeridian:
  // panning east runs centerLon past +180 while the tile column index wraps, so
  // a target reported at -179.9 belongs just left of a centre reading 180.1 --
  // not 360 degrees and sixteen thousand pixels away.
  function project(st, lat, lon) {
    const cx = st.canvas.clientWidth / 2;
    const cy = st.canvas.clientHeight / 2;
    const near = lon + 360 * Math.round((st.centerLon - lon) / 360);
    return {
      x: cx + lonToWorldX(near, st.zoom) - lonToWorldX(st.centerLon, st.zoom),
      y: cy + latToWorldY(lat, st.zoom) - latToWorldY(st.centerLat, st.zoom),
    };
  }

  function unproject(st, x, y) {
    const cx = st.canvas.clientWidth / 2;
    const cy = st.canvas.clientHeight / 2;
    return {
      lon: worldXToLon(lonToWorldX(st.centerLon, st.zoom) + (x - cx), st.zoom),
      lat: worldYToLat(latToWorldY(st.centerLat, st.zoom) + (y - cy), st.zoom),
    };
  }

  function setCenter(st, lat, lon) {
    st.centerLat = clamp(lat, -MERC_MAX_LAT, MERC_MAX_LAT);
    st.centerLon = lon;
    scheduleDraw(st);
  }

  // panBy moves the centre by a screen delta, in world pixels rather than
  // degrees: a degree of latitude is not a constant number of pixels in
  // Mercator, so panning in degrees would slide faster than the chart.
  function panBy(st, dx, dy) {
    const wx = lonToWorldX(st.centerLon, st.zoom) - dx;
    const wy = latToWorldY(st.centerLat, st.zoom) - dy;
    st.centerLon = worldXToLon(wx, st.zoom);
    st.centerLat = worldYToLat(clamp(wy, 0, worldSize(st.zoom)), st.zoom);
    scheduleDraw(st);
  }

  // zoomAt leaves the geographic point under (x, y) where it is, which is what
  // makes wheel and double-click zoom feel anchored to the cursor.
  function zoomAt(st, x, y, factor) {
    const anchor = unproject(st, x, y);
    const zoom = clamp(st.zoom + Math.log2(factor), ZOOM_MIN, ZOOM_MAX);
    if (zoom === st.zoom) return;
    st.zoom = zoom;
    const cx = st.canvas.clientWidth / 2;
    const cy = st.canvas.clientHeight / 2;
    const wx = lonToWorldX(anchor.lon, zoom) - (x - cx);
    const wy = latToWorldY(anchor.lat, zoom) - (y - cy);
    st.centerLon = worldXToLon(wx, zoom);
    st.centerLat = worldYToLat(clamp(wy, 0, worldSize(zoom)), zoom);
    scheduleDraw(st);
  }

  // Smallest box fitTo will zoom to, so a single vessel (or two moored side by
  // side) does not slam the chart to maximum zoom.
  const MIN_FIT_SPAN_DEG = 0.02;

  function fitTo(st, points) {
    const pts = points.filter((p) => Number.isFinite(p.lat) && Number.isFinite(p.lon));
    if (pts.length === 0) return false;
    let minLat = Infinity, maxLat = -Infinity, minLon = Infinity, maxLon = -Infinity;
    for (const p of pts) {
      minLat = Math.min(minLat, p.lat); maxLat = Math.max(maxLat, p.lat);
      minLon = Math.min(minLon, p.lon); maxLon = Math.max(maxLon, p.lon);
    }
    if (maxLat - minLat < MIN_FIT_SPAN_DEG) {
      const c = (minLat + maxLat) / 2;
      minLat = c - MIN_FIT_SPAN_DEG / 2;
      maxLat = c + MIN_FIT_SPAN_DEG / 2;
    }
    if (maxLon - minLon < MIN_FIT_SPAN_DEG) {
      const c = (minLon + maxLon) / 2;
      minLon = c - MIN_FIT_SPAN_DEG / 2;
      maxLon = c + MIN_FIT_SPAN_DEG / 2;
    }
    // Measure the box in zoom-0 world pixels and solve for the zoom that makes
    // it fit -- world pixels scale by exactly 2^zoom, so it is a log2 rather
    // than a search. The centre is the middle of the box in PROJECTED space,
    // which in Mercator is not the mean of the latitudes.
    const x0 = lonToWorldX(minLon, 0), x1 = lonToWorldX(maxLon, 0);
    const y0 = latToWorldY(maxLat, 0), y1 = latToWorldY(minLat, 0);
    st.centerLon = worldXToLon((x0 + x1) / 2, 0);
    st.centerLat = worldYToLat((y0 + y1) / 2, 0);
    const w = Math.max(120, st.canvas.clientWidth) * 0.82;
    const h = Math.max(120, st.canvas.clientHeight) * 0.82;
    st.zoom = clamp(Math.min(Math.log2(w / Math.max(x1 - x0, 1e-9)),
      Math.log2(h / Math.max(y1 - y0, 1e-9))), ZOOM_MIN, ZOOM_MAX);
    scheduleDraw(st);
    return true;
  }

  const STEP_CANDIDATES = [30, 10, 5, 2, 1, 0.5, 0.2, 0.1, 0.05, 0.02, 0.01, 0.005, 0.002, 0.001];
  function niceStep(pxPerDeg) {
    for (const step of STEP_CANDIDATES) {
      if (step * pxPerDeg >= 80) continue;
      return step;
    }
    return STEP_CANDIDATES[STEP_CANDIDATES.length - 1];
  }

  function decimalsFor(step) {
    if (step >= 1) return 0;
    if (step >= 0.1) return 1;
    if (step >= 0.01) return 2;
    return 3;
  }

  // Scale-bar lengths are nice numbers IN THE UNIT BEING DISPLAYED rather than
  // picked in km and converted, so the bar is exactly as long as its label says
  // (converting is what makes a 0.5 km bar read "0.3 nm", wrong by 11%).
  const RANGE_STEPS = [0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 400];

  function kmPerUnit(units) { return units.metric ? 1 : KM_PER_NM; }

  function rangeStep(pxPerUnit, minPx) {
    for (const s of RANGE_STEPS) {
      if (s * pxPerUnit >= minPx) return s;
    }
    return RANGE_STEPS[RANGE_STEPS.length - 1];
  }

  function fmtRange(v) { return String(Number(v.toFixed(3))); }

  function rangeLabel(v, metric) {
    if (metric) return v < 1 ? `${Math.round(v * 1000)} m` : `${fmtRange(v)} km`;
    return v < 0.2 ? `${Math.round(v * KM_PER_NM * 1000)} m` : `${fmtRange(v)} nm`;
  }

  // ---------------------------------------------------------------------------
  // Basemap tiles
  //
  // Fetched from the portal's own endpoint, root-relative so the page never
  // names a host, and never straight from the browser to OSM: the caching,
  // revalidation and User-Agent the OSM tile usage policy requires all live
  // server-side. Two obligations land on this side and are not negotiable:
  //
  //   - "© OpenStreetMap contributors" is on screen whenever tiles are. Not
  //     behind the basemap toggle, not under other overlays, not off-screen.
  //   - Requests are strictly DEMAND-DRIVEN: only tiles the current viewport is
  //     about to draw. No neighbour prefetch, no other zoom level, no warm-up,
  //     no "download this area" -- bulk downloading and pre-seeding are
  //     prohibited, and nothing of the sort should be added later.
  //
  // Any non-200 (503 tiles disabled or offline, 400, a block, no network) is
  // just "no basemap": a failed tile is remembered as failed so it is never
  // retried in a draw loop, and once several have failed with nothing having
  // ever loaded the layer gives up rather than storming a dead endpoint. A
  // B200 on a bench with no internet is a normal operating case.
  // ---------------------------------------------------------------------------

  const TILE_URL_BASE = "/api/tiles/";
  const TILE_CACHE_MAX = 320;
  const TILE_GIVEUP_FAILURES = 4;

  function tileKey(z, x, y) { return `${z}/${x}/${y}`; }

  // A timeout rather than requestAnimationFrame, for the same reason drawNow
  // exists: rAF does not run in a hidden or non-compositing tab, and a tile
  // that has landed belongs on the canvas either way.
  function scheduleTileDraw(st) {
    if (st.tileDrawPending) return;
    st.tileDrawPending = window.setTimeout(() => {
      st.tileDrawPending = 0;
      if (st.el.isConnected) drawNow(st);
    }, 0);
  }

  function requestTile(st, z, x, y) {
    const key = tileKey(z, x, y);
    const have = st.tiles.get(key);
    if (have) return have.ok ? have.img : null;
    if (st.tilesUnavailable) return null;

    const entry = { img: new Image(), ok: false, failed: false };
    st.tiles.set(key, entry);
    entry.img.addEventListener("load", () => {
      entry.ok = true;
      st.tileLoads++;
      scheduleTileDraw(st);
    });
    entry.img.addEventListener("error", () => {
      entry.failed = true;
      st.tileFailures++;
      if (st.tileLoads === 0 && st.tileFailures >= TILE_GIVEUP_FAILURES) {
        st.tilesUnavailable = true;
      }
      scheduleTileDraw(st);
    });
    entry.img.src = TILE_URL_BASE + key + ".png";
    return null;
  }

  // drawTiles paints the basemap and reports whether anything actually landed,
  // which is what decides whether the attribution is shown and whether the
  // graticule switches to its over-imagery colours.
  function drawTiles(st, ctx, w, h) {
    if (!st.prefs.basemap || st.tilesUnavailable) return false;

    // Tile z follows the map zoom in CSS pixels, deliberately ignoring
    // devicePixelRatio: on a 2x display that upscales the imagery slightly
    // rather than asking OSM's servers for four times as many tiles.
    const z = clamp(Math.round(st.zoom), 0, ZOOM_MAX);
    const scale = Math.pow(2, st.zoom - z);
    const size = TILE_SIZE * scale;
    const n = Math.pow(2, z);
    const originX = w / 2 - lonToWorldX(st.centerLon, z) * scale;
    const originY = h / 2 - latToWorldY(st.centerLat, z) * scale;

    const firstX = Math.floor(-originX / size);
    const lastX = Math.floor((w - originX) / size);
    const firstY = Math.floor(-originY / size);
    const lastY = Math.floor((h - originY) / size);

    const used = new Set();
    let drew = 0;
    for (let ty = firstY; ty <= lastY; ty++) {
      if (ty < 0 || ty >= n) continue; // past the top or bottom of the world
      for (let tx = firstX; tx <= lastX; tx++) {
        // The world wraps east-west; the tile index does, the screen slot does
        // not, so the same tile can legitimately appear twice at low zoom.
        const wrapX = ((tx % n) + n) % n;
        used.add(tileKey(z, wrapX, ty));
        const img = requestTile(st, z, wrapX, ty);
        if (!img) continue;
        // Half a pixel of overlap: at fractional scale, adjacent tiles drawn on
        // exact boundaries leave hairline seams.
        ctx.drawImage(img, originX + tx * size, originY + ty * size, size + 0.5, size + 0.5);
        drew++;
      }
    }

    if (drew > 0) {
      // The OSM raster style is a light map and this is a dark panel: a scrim
      // knocks it back so the targets stay the foreground.
      ctx.fillStyle = "rgba(11,10,8,0.32)";
      ctx.fillRect(0, 0, w, h);
    }

    // Bounded cache. Tiles drawn this frame are never evicted, so trimming can
    // never cause the next frame to re-request what it just showed.
    if (st.tiles.size > TILE_CACHE_MAX) {
      for (const key of Array.from(st.tiles.keys())) {
        if (st.tiles.size <= TILE_CACHE_MAX) break;
        if (!used.has(key)) st.tiles.delete(key);
      }
    }
    return drew > 0;
  }

  function updateBasemapChrome(st, tilesDrawn) {
    st.attribEl.style.display = tilesDrawn ? "block" : "none";
    st.offlineEl.style.display = (st.prefs.basemap && st.tilesUnavailable) ? "block" : "none";
  }

  // ---------------------------------------------------------------------------
  // Drawing
  // ---------------------------------------------------------------------------

  function sizeCanvas(st) {
    const dpr = window.devicePixelRatio || 1;
    const rect = st.canvas.getBoundingClientRect();
    const w = Math.max(1, Math.round(rect.width * dpr));
    const h = Math.max(1, Math.round(rect.height * dpr));
    if (st.canvas.width !== w || st.canvas.height !== h) {
      st.canvas.width = w;
      st.canvas.height = h;
    }
  }

  // In Mercator longitude is linear in x but latitude is NOT linear in y, so
  // the parallels are stepped in DEGREES and projected one at a time, and they
  // need their own step: a degree of latitude is 1/cos(phi) times as tall as a
  // degree of longitude is wide.
  function drawGraticule(st, ctx, w, h, overTiles) {
    const pxPerDegLon = worldSize(st.zoom) / 360;
    const lonStep = niceStep(pxPerDegLon);
    const pxPerDegLat = pxPerDegLon /
      Math.cos(clamp(st.centerLat, -MERC_MAX_LAT, MERC_MAX_LAT) * D2R);
    const latStep = niceStep(pxPerDegLat);

    const nw = unproject(st, 0, 0);
    const se = unproject(st, w, h);
    const minLat = clamp(se.lat, -MERC_MAX_LAT, MERC_MAX_LAT);
    const maxLat = clamp(nw.lat, -MERC_MAX_LAT, MERC_MAX_LAT);

    ctx.save();
    ctx.strokeStyle = overTiles ? "rgba(255,255,255,0.14)" : "rgba(255,255,255,0.06)";
    ctx.fillStyle = overTiles ? "rgba(237,232,219,0.85)" : "rgba(160,150,129,0.9)";
    ctx.font = "10px monospace";
    ctx.lineWidth = 1;

    // Over map imagery a bare label disappears into whatever is behind it, so
    // it gets the same dark halo the vessel labels use.
    const label = (text, x, y) => {
      if (overTiles) {
        const fill = ctx.fillStyle;
        ctx.lineWidth = 3;
        ctx.strokeStyle = "rgba(11,10,8,0.85)";
        ctx.strokeText(text, x, y);
        ctx.lineWidth = 1;
        ctx.strokeStyle = "rgba(255,255,255,0.14)";
        ctx.fillStyle = fill;
      }
      ctx.fillText(text, x, y);
    };

    const lonDecimals = decimalsFor(lonStep);
    for (let lon = Math.floor(nw.lon / lonStep) * lonStep; lon <= se.lon; lon += lonStep) {
      const { x } = project(st, st.centerLat, lon);
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, h);
      ctx.stroke();
      const dl = normLon(lon);
      label(`${Math.abs(dl).toFixed(lonDecimals)}°${dl >= 0 ? "E" : "W"}`, x + 3, 11);
    }
    const latDecimals = decimalsFor(latStep);
    for (let lat = Math.floor(minLat / latStep) * latStep; lat <= maxLat; lat += latStep) {
      const { y } = project(st, lat, st.centerLon);
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(w, y);
      ctx.stroke();
      label(`${Math.abs(lat).toFixed(latDecimals)}°${lat >= 0 ? "N" : "S"}`, 3, y - 3 < 10 ? y + 12 : y - 3);
    }
    ctx.restore();
  }

  function drawTrail(st, ctx, trail, color, selected) {
    if (!trail || trail.length < 2) return;
    ctx.save();
    ctx.lineWidth = selected ? 2 : 1.2;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    // Fade the tail so direction of travel reads without arrowheads.
    for (let i = 1; i < trail.length; i++) {
      const a = project(st, trail[i - 1].lat, trail[i - 1].lon);
      const b = project(st, trail[i].lat, trail[i].lon);
      const t = i / trail.length;
      ctx.globalAlpha = (selected ? 0.35 : 0.18) + t * (selected ? 0.55 : 0.42);
      ctx.strokeStyle = color;
      ctx.beginPath();
      ctx.moveTo(a.x, a.y);
      ctx.lineTo(b.x, b.y);
      ctx.stroke();
    }
    ctx.restore();
  }

  // How far ahead a speed vector reaches: the distance the vessel would cover
  // in this many minutes at its reported SOG. Six is the conventional ARPA
  // vector on a ship's own radar, and because the far end is computed
  // geographically the line stays a true six minutes at every zoom.
  const VECTOR_MINUTES = 6;

  // drawSpeedVector draws the vessel's predicted travel. It needs BOTH a speed
  // and a direction of travel, and it uses courseDeg -- course over ground --
  // rather than the hull's heading, because a vessel making way in a tideway
  // is not going where her bow points. When only a heading is known courseDeg
  // falls back to it (see orientationOf); when neither is known, or SOG was
  // never reported, no vector is drawn at all rather than a zero-length stub
  // that would read as "stopped".
  function drawSpeedVector(st, ctx, v, color) {
    if (!Number.isFinite(v.sog_kn) || v.sog_kn <= 0) return;
    if (!Number.isFinite(v.courseDeg)) return;
    const km = v.sog_kn * KM_PER_NM * (VECTOR_MINUTES / 60);
    const end = destPoint(v.lat, v.lon, v.courseDeg, km);
    const a = project(st, v.lat, v.lon);
    const b = project(st, end.lat, end.lon);
    ctx.save();
    ctx.strokeStyle = color;
    ctx.globalAlpha = 0.75;
    ctx.lineWidth = 1.4;
    ctx.lineCap = "round";
    ctx.beginPath();
    ctx.moveTo(a.x, a.y);
    ctx.lineTo(b.x, b.y);
    ctx.stroke();
    // A cross tick at the far end, so the end of the vector is distinguishable
    // from a trail segment running the same way.
    const ang = Math.atan2(b.y - a.y, b.x - a.x) + Math.PI / 2;
    ctx.beginPath();
    ctx.moveTo(b.x + Math.cos(ang) * 3, b.y + Math.sin(ang) * 3);
    ctx.lineTo(b.x - Math.cos(ang) * 3, b.y - Math.sin(ang) * 3);
    ctx.stroke();
    ctx.restore();
  }

  // drawVessel draws a hull pointing along `orientDeg`, or -- when the vessel
  // has never reported an orientation -- a deliberately DIFFERENT glyph: a
  // ringed dot, which has no bow and cannot be misread as a direction. That
  // distinction is the whole point. Defaulting an unknown orientation to 0
  // would draw every silent vessel steaming due north.
  function drawVessel(ctx, x, y, color, orientDeg, selected, label) {
    ctx.save();
    ctx.translate(x, y);

    if (selected) {
      ctx.beginPath();
      ctx.arc(0, 0, 13, 0, Math.PI * 2);
      ctx.strokeStyle = "rgba(255,255,255,0.85)";
      ctx.lineWidth = 1.5;
      ctx.stroke();
    }

    ctx.save();
    if (Number.isFinite(orientDeg)) {
      ctx.rotate(orientDeg * D2R);
      // Plan view of a hull: pointed bow, parallel sides, transom stern.
      ctx.beginPath();
      ctx.moveTo(0, -9);
      ctx.lineTo(3.4, -3.2);
      ctx.lineTo(3.6, 5.4);
      ctx.lineTo(2.6, 7.6);
      ctx.lineTo(-2.6, 7.6);
      ctx.lineTo(-3.6, 5.4);
      ctx.lineTo(-3.4, -3.2);
      ctx.closePath();
      ctx.fillStyle = color;
      ctx.fill();
      ctx.strokeStyle = "rgba(0,0,0,0.55)";
      ctx.lineWidth = 0.8;
      ctx.stroke();
    } else {
      ctx.beginPath();
      ctx.arc(0, 0, 4, 0, Math.PI * 2);
      ctx.fillStyle = color;
      ctx.fill();
      ctx.strokeStyle = "rgba(0,0,0,0.55)";
      ctx.lineWidth = 0.8;
      ctx.stroke();
      ctx.beginPath();
      ctx.arc(0, 0, 7, 0, Math.PI * 2);
      ctx.strokeStyle = color;
      ctx.globalAlpha = 0.55;
      ctx.lineWidth = 1;
      ctx.stroke();
    }
    ctx.restore();

    if (label) {
      ctx.font = selected ? "bold 11px monospace" : "10px monospace";
      ctx.textAlign = "left";
      ctx.lineWidth = 3;
      ctx.strokeStyle = "rgba(11,10,8,0.85)";
      ctx.strokeText(label, 11, 4);
      ctx.fillStyle = selected ? "#ffffff" : "#ede8db";
      ctx.fillText(label, 11, 4);
    }
    ctx.restore();
  }

  function drawScaleBar(st) {
    // Ground resolution at the centre latitude: Mercator's scale is only true
    // along the parallel it is measured at, and the middle of the view is the
    // one being looked at.
    const pxPerUnit = kmPerUnit(st.units) * 1000 / metresPerPixel(st.centerLat, st.zoom);
    const v = rangeStep(pxPerUnit, 60);
    st.scaleEl.textContent = rangeLabel(v, st.units.metric);
    st.scaleEl.style.width = `${Math.round(v * pxPerUnit)}px`;
    // currentColor rather than a named colour: an inline style beats every
    // stylesheet, so naming one here would put the ruler permanently out of
    // the theme's reach.
    st.scaleEl.style.borderBottom = "2px solid currentColor";
  }

  // Above this zoom the view is a few miles across and every target can carry
  // its name without the labels colliding.
  const LABEL_ZOOM = 11.5;

  function draw(st) {
    const ctx = st.ctx;
    if (!ctx) return;
    const dpr = window.devicePixelRatio || 1;
    const w = st.canvas.clientWidth;
    const h = st.canvas.clientHeight;
    if (w <= 0 || h <= 0) return;

    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = "#080b10";
    ctx.fillRect(0, 0, w, h);

    const overTiles = drawTiles(st, ctx, w, h);
    updateBasemapChrome(st, overTiles);
    drawGraticule(st, ctx, w, h, overTiles);

    // Only vessels with a real fix are ever drawn. There is no path in this
    // file from a table row to a marker.
    const visible = st.filtered.filter((v) => v.hasPos);
    st.hit = [];

    if (st.prefs.trails) {
      for (const v of visible) {
        drawTrail(st, ctx, st.trails.get(v.mmsi), navColor(v), v.mmsi === st.selectedId);
      }
    }

    if (st.prefs.vectors) {
      for (const v of visible) drawSpeedVector(st, ctx, v, navColor(v));
    }

    for (const v of visible) {
      const p = project(st, v.lat, v.lon);
      if (p.x < -60 || p.y < -60 || p.x > w + 60 || p.y > h + 60) continue;
      const selected = v.mmsi === st.selectedId;
      const label = st.zoom >= LABEL_ZOOM || selected ? (v.name || v.mmsi) : "";
      drawVessel(ctx, p.x, p.y, navColor(v), v.orientDeg, selected, label);
      st.hit.push({ mmsi: v.mmsi, x: p.x, y: p.y });
    }

    drawScaleBar(st);
  }

  function hitTest(st, x, y) {
    let best = null;
    let bestD = 15;
    for (const h of st.hit) {
      const d = Math.hypot(h.x - x, h.y - y);
      if (d < bestD) { bestD = d; best = h.mmsi; }
    }
    return best;
  }

  // ---------------------------------------------------------------------------
  // Skeleton
  // ---------------------------------------------------------------------------

  const COLUMNS = [
    { key: "mmsi", label: "MMSI", num: false },
    { key: "name", label: "Name", num: false },
    { key: "sog_kn", label: "SOG", num: true },
    { key: "cog_deg", label: "COG", num: true },
    { key: "nav_status", label: "NavStatus", num: false },
    { key: "msgs", label: "Msgs", num: true },
  ];

  function buildSkeleton(el) {
    ensureStyle();
    el.innerHTML = "";
    // The kind class goes on the root for identification only -- no rule in
    // this panel's stylesheet targets it (see the CSS header).
    el.classList.add("mp-panel", "mp-ais-panel");

    const prefs = loadPrefs();

    // Everything this panel builds lives under one wrapper, so the host's
    // next innerHTML clear takes the whole thing -- layout included -- with it.
    const root = document.createElement("div");
    root.className = "mp-ais-root";
    el.appendChild(root);

    const stats = document.createElement("div");
    stats.className = "mp-ais-stats";
    root.appendChild(stats);

    const bar = document.createElement("div");
    bar.className = "mp-ais-bar";
    bar.innerHTML = `
      <input type="search" data-f="search" placeholder="MMSI / name / call sign…" />
      <label><input type="checkbox" data-f="posOnly" /> Positions only</label>
      <button class="mp-ais-btn" data-f="trails" aria-pressed="true">Trails</button>
      <button class="mp-ais-btn" data-f="vectors" aria-pressed="true" title="${VECTOR_MINUTES}-minute course/speed vectors">Vectors</button>
      <button class="mp-ais-btn" data-f="basemap" aria-pressed="true" title="OpenStreetMap basemap">Basemap</button>
      <button class="mp-ais-btn" data-f="follow" aria-pressed="false">Follow</button>
      <button class="mp-ais-btn" data-f="units">Units: knots</button>
      <button class="mp-ais-btn" data-f="pause" aria-pressed="false">Pause</button>
    `;
    root.appendChild(bar);

    const body = document.createElement("div");
    body.className = "mp-ais-body";

    const mapwrap = document.createElement("div");
    mapwrap.className = "mp-ais-mapwrap";
    const canvas = document.createElement("canvas");
    mapwrap.appendChild(canvas);

    const ctl = document.createElement("div");
    ctl.className = "mp-ais-ctl";
    ctl.innerHTML = `
      <button class="mp-ais-btn" data-f="zin" title="Zoom in">+</button>
      <button class="mp-ais-btn" data-f="zout" title="Zoom out">−</button>
      <button class="mp-ais-btn mp-ais-wide" data-f="fit" title="Fit all located vessels">Fit</button>
    `;
    mapwrap.appendChild(ctl);

    const legend = document.createElement("div");
    legend.className = "mp-ais-legend";
    legend.innerHTML = ["underway", "moored", "special", "unknown"].map((k) =>
      `<span class="mp-ais-key"><i style="background:${NAV_COLOR[k]}"></i>${escapeHtml(NAV_CLASS_LABEL[k])}</span>`
    ).join("") + `<span class="mp-ais-key">vector: ${VECTOR_MINUTES} min</span>`;
    mapwrap.appendChild(legend);

    const scale = document.createElement("div");
    scale.className = "mp-ais-scale";
    mapwrap.appendChild(scale);

    // Attribution: required by the OSM tile usage policy whenever the tiles are
    // on screen, so it is created here rather than being anything a control can
    // switch off, and it links to the copyright page as the policy asks.
    const attrib = document.createElement("div");
    attrib.className = "mp-ais-attrib";
    attrib.innerHTML = '<a href="https://www.openstreetmap.org/copyright" ' +
      'target="_blank" rel="noopener noreferrer">© OpenStreetMap contributors</a>';
    attrib.style.display = "none";
    mapwrap.appendChild(attrib);

    // The entire report for "no basemap": one line, no dialog, nothing to
    // acknowledge. It only appears if the operator asked for a basemap.
    const offline = document.createElement("div");
    offline.className = "mp-ais-offline";
    offline.textContent = "Basemap unavailable — running offline";
    offline.style.display = "none";
    mapwrap.appendChild(offline);

    const tip = document.createElement("div");
    tip.className = "mp-ais-tip";
    tip.style.display = "none";
    mapwrap.appendChild(tip);

    body.appendChild(mapwrap);

    const tablewrap = document.createElement("div");
    tablewrap.className = "mp-ais-tablewrap";
    const table = document.createElement("table");
    table.className = "mp-ais-table";
    const thead = document.createElement("thead");
    const headRow = document.createElement("tr");
    for (const c of COLUMNS) {
      const th = document.createElement("th");
      th.textContent = c.label;
      th.dataset.key = c.key;
      headRow.appendChild(th);
    }
    thead.appendChild(headRow);
    table.appendChild(thead);
    const tbody = document.createElement("tbody");
    table.appendChild(tbody);
    tablewrap.appendChild(table);
    const empty = document.createElement("div");
    empty.className = "mp-ais-empty";
    empty.textContent = "No vessels heard yet.";
    tablewrap.appendChild(empty);
    body.appendChild(tablewrap);

    root.appendChild(body);

    const st = {
      el,
      prefs,
      statsEl: stats,
      bar,
      mapwrap,
      canvas,
      ctx: canvas.getContext("2d"),
      tipEl: tip,
      scaleEl: scale,
      attribEl: attrib,
      offlineEl: offline,
      tbody,
      headRow,
      emptyEl: empty,
      units: { metric: prefs.metric },
      centerLat: 0,
      centerLon: 0,
      zoom: DEFAULT_ZOOM,
      tiles: new Map(),
      tileLoads: 0,
      tileFailures: 0,
      tilesUnavailable: false,
      tileDrawPending: 0,
      hasFitted: false,
      vessels: [],
      filtered: [],
      trails: new Map(),
      firstSeen: new Map(),
      rows: new Map(),
      hit: [],
      selectedId: null,
      paused: false,
      // packetsValid stays null until the payload carries one, so the stats
      // line can leave it out entirely rather than claiming zero.
      packetsValid: null,
      dragging: false,
      dragMoved: false,
      lastX: 0,
      lastY: 0,
      needsDraw: true,
      drawPending: 0,
    };
    el.__mpAis = st;

    wireControls(st);
    wireCanvas(st);
    wireTable(st);
    startLoop(st);
    syncControls(st);
    return st;
  }

  // ---------------------------------------------------------------------------
  // Event wiring
  // ---------------------------------------------------------------------------

  function q(st, sel) {
    return st.bar.querySelector(`[data-f="${sel}"]`);
  }

  function syncControls(st) {
    q(st, "search").value = st.prefs.search || "";
    q(st, "posOnly").checked = !!st.prefs.posOnly;
    q(st, "trails").setAttribute("aria-pressed", String(!!st.prefs.trails));
    q(st, "vectors").setAttribute("aria-pressed", String(!!st.prefs.vectors));
    q(st, "basemap").setAttribute("aria-pressed", String(!!st.prefs.basemap));
    q(st, "follow").setAttribute("aria-pressed", String(!!st.prefs.follow));
    q(st, "units").textContent = st.units.metric ? "Units: km/h" : "Units: knots";
    q(st, "pause").setAttribute("aria-pressed", String(st.paused));
    q(st, "pause").textContent = st.paused ? "Resume" : "Pause";
  }

  function wireControls(st) {
    q(st, "search").addEventListener("input", (ev) => {
      st.prefs.search = ev.target.value;
      savePrefs(st.prefs);
      refreshDerived(st);
    });
    q(st, "posOnly").addEventListener("change", (ev) => {
      st.prefs.posOnly = ev.target.checked;
      savePrefs(st.prefs);
      refreshDerived(st);
    });

    for (const key of ["trails", "vectors", "follow"]) {
      q(st, key).addEventListener("click", () => {
        st.prefs[key] = !st.prefs[key];
        savePrefs(st.prefs);
        syncControls(st);
        scheduleDraw(st);
      });
    }

    q(st, "basemap").addEventListener("click", () => {
      st.prefs.basemap = !st.prefs.basemap;
      if (st.prefs.basemap) {
        // Switching it back on is an explicit ask, and the only moment it is
        // reasonable to try an endpoint already written off -- the operator may
        // have just plugged the network back in. Only failed entries are
        // dropped; tiles already in hand are still good.
        st.tilesUnavailable = false;
        st.tileFailures = 0;
        for (const [key, entry] of Array.from(st.tiles)) {
          if (entry.failed) st.tiles.delete(key);
        }
      }
      savePrefs(st.prefs);
      syncControls(st);
      scheduleDraw(st);
    });

    q(st, "units").addEventListener("click", () => {
      st.units.metric = !st.units.metric;
      st.prefs.metric = st.units.metric;
      savePrefs(st.prefs);
      syncControls(st);
      renderTable(st);
      updateStats(st);
      scheduleDraw(st);
    });

    q(st, "pause").addEventListener("click", () => {
      st.paused = !st.paused;
      syncControls(st);
      updateStats(st);
    });

    st.mapwrap.querySelector('[data-f="zin"]').addEventListener("click", () =>
      zoomAt(st, st.canvas.clientWidth / 2, st.canvas.clientHeight / 2, 1.5));
    st.mapwrap.querySelector('[data-f="zout"]').addEventListener("click", () =>
      zoomAt(st, st.canvas.clientWidth / 2, st.canvas.clientHeight / 2, 1 / 1.5));
    st.mapwrap.querySelector('[data-f="fit"]').addEventListener("click", () => {
      fitTo(st, st.filtered.filter((v) => v.hasPos));
    });
  }

  function wireCanvas(st) {
    const c = st.canvas;

    if (window.ResizeObserver) {
      new ResizeObserver(() => { sizeCanvas(st); scheduleDraw(st); }).observe(st.mapwrap);
    } else {
      window.addEventListener("resize", () => { sizeCanvas(st); scheduleDraw(st); });
    }

    c.addEventListener("pointerdown", (ev) => {
      st.dragging = true;
      st.dragMoved = false;
      st.lastX = ev.clientX;
      st.lastY = ev.clientY;
      c.setPointerCapture(ev.pointerId);
      c.classList.add("mp-ais-dragging");
    });

    c.addEventListener("pointermove", (ev) => {
      const rect = c.getBoundingClientRect();
      const px = ev.clientX - rect.left;
      const py = ev.clientY - rect.top;

      if (st.dragging) {
        const dx = ev.clientX - st.lastX;
        const dy = ev.clientY - st.lastY;
        if (Math.abs(dx) + Math.abs(dy) > 2) st.dragMoved = true;
        st.lastX = ev.clientX;
        st.lastY = ev.clientY;
        panBy(st, dx, dy);
        // A manual pan means the operator wants to look somewhere specific;
        // silently fighting them with follow-mode re-centring is infuriating.
        if (st.prefs.follow) {
          st.prefs.follow = false;
          savePrefs(st.prefs);
          syncControls(st);
        }
        scheduleDraw(st);
        return;
      }

      const mmsi = hitTest(st, px, py);
      showTip(st, mmsi, px, py);
      highlightRow(st, mmsi, false);
    });

    const endDrag = (ev) => {
      if (!st.dragging) return;
      st.dragging = false;
      c.classList.remove("mp-ais-dragging");
      try { c.releasePointerCapture(ev.pointerId); } catch (_) { /* already released */ }
    };
    c.addEventListener("pointerup", (ev) => {
      const wasDrag = st.dragMoved;
      endDrag(ev);
      if (wasDrag) return;
      const rect = c.getBoundingClientRect();
      // Clicking a vessel on the chart selects it, which highlights and scrolls
      // to its row in the table -- the other half of the cross-link.
      select(st, hitTest(st, ev.clientX - rect.left, ev.clientY - rect.top));
    });
    c.addEventListener("pointercancel", endDrag);
    c.addEventListener("pointerleave", () => {
      st.tipEl.style.display = "none";
      highlightRow(st, null, false);
    });

    c.addEventListener("wheel", (ev) => {
      ev.preventDefault();
      const rect = c.getBoundingClientRect();
      zoomAt(st, ev.clientX - rect.left, ev.clientY - rect.top,
        ev.deltaY < 0 ? 1.22 : 1 / 1.22);
    }, { passive: false });

    c.addEventListener("dblclick", (ev) => {
      const rect = c.getBoundingClientRect();
      zoomAt(st, ev.clientX - rect.left, ev.clientY - rect.top, 2);
    });
  }

  function wireTable(st) {
    st.headRow.addEventListener("click", (ev) => {
      const th = ev.target.closest("th");
      if (!th || !th.dataset.key) return;
      if (st.prefs.sortKey === th.dataset.key) {
        st.prefs.sortDir = -st.prefs.sortDir;
      } else {
        st.prefs.sortKey = th.dataset.key;
        st.prefs.sortDir = 1;
      }
      savePrefs(st.prefs);
      refreshDerived(st);
    });

    st.tbody.addEventListener("click", (ev) => {
      const tr = ev.target.closest("tr");
      if (!tr || !tr.dataset.mmsi) return;
      select(st, tr.dataset.mmsi);
      // Clicking a row centres the chart on that vessel -- and only if it has a
      // position. A vessel with no fix stays in the table and moves nothing.
      const v = st.vessels.find((x) => x.mmsi === tr.dataset.mmsi);
      if (v && v.hasPos) setCenter(st, v.lat, v.lon);
    });

    st.tbody.addEventListener("mouseover", (ev) => {
      const tr = ev.target.closest("tr");
      if (tr && tr.dataset.mmsi) showTipForRow(st, tr.dataset.mmsi);
    });
    st.tbody.addEventListener("mouseleave", () => { st.tipEl.style.display = "none"; });
  }

  function select(st, mmsi) {
    st.selectedId = (mmsi && mmsi === st.selectedId) ? null : (mmsi || null);
    highlightRow(st, st.selectedId, true);
    scheduleDraw(st);
  }

  function highlightRow(st, mmsi, selectedClass) {
    for (const [id, tr] of st.rows) {
      tr.classList.toggle("mp-ais-sel", id === (selectedClass ? mmsi : st.selectedId));
    }
    if (selectedClass && mmsi) {
      const tr = st.rows.get(mmsi);
      if (tr && tr.scrollIntoView) tr.scrollIntoView({ block: "nearest" });
    }
  }

  // Every row of the tooltip prints an em dash for a field the vessel has not
  // broadcast. That is a display of absence, not a value: nothing here
  // substitutes a zero, and the position row says "not reported" rather than
  // showing coordinates the vessel never sent.
  function tipHtml(st, v) {
    const rows = [
      ["MMSI", v.mmsi],
      ["Name", v.name || "—"],
      ["Call sign", v.callsign || "—"],
      ["Destination", v.destination || "—"],
      ["Position", v.hasPos
        ? `${v.lat.toFixed(5)}, ${v.lon.toFixed(5)}` : "not reported"],
      ["SOG", fmtSpeed(v.sog_kn, st.units) || "—"],
      ["COG", Number.isFinite(v.cog_deg)
        ? `${fmtDeg(v.cog_deg)} ${compassOf(v.cog_deg)}` : "—"],
      ["Heading", Number.isFinite(v.heading_deg)
        ? `${Math.round(v.heading_deg)}° ${compassOf(v.heading_deg)}` : "—"],
      ["Nav status", navText(v.nav_status) || "—"],
      ["Messages", fmtInt(v.msgs) || "—"],
      ["Last", v.time || "—"],
    ];
    return rows.map(([k, val]) =>
      `<div>${escapeHtml(k)}: <b>${escapeHtml(val)}</b></div>`).join("");
  }

  function showTip(st, mmsi, px, py) {
    if (!mmsi) { st.tipEl.style.display = "none"; return; }
    const v = st.vessels.find((x) => x.mmsi === mmsi);
    if (!v) { st.tipEl.style.display = "none"; return; }
    st.tipEl.innerHTML = tipHtml(st, v);
    st.tipEl.style.display = "block";
    const w = st.mapwrap.clientWidth;
    const h = st.mapwrap.clientHeight;
    const tw = st.tipEl.offsetWidth;
    const th = st.tipEl.offsetHeight;
    st.tipEl.style.left = `${clamp(px + 14, 4, Math.max(4, w - tw - 4))}px`;
    st.tipEl.style.top = `${clamp(py + 14, 4, Math.max(4, h - th - 4))}px`;
  }

  function showTipForRow(st, mmsi) {
    const v = st.vessels.find((x) => x.mmsi === mmsi);
    if (!v) return;
    st.tipEl.innerHTML = tipHtml(st, v);
    st.tipEl.style.display = "block";
    st.tipEl.style.left = "8px";
    st.tipEl.style.top = "8px";
  }

  // ---------------------------------------------------------------------------
  // Data shaping
  //
  // The wire contract's rule is that an absent field is ABSENT -- not zero, not
  // an empty string, not 0N 0E -- so every reader below turns "not present" into
  // NaN or "" and every consumer tests for it. num() exists so that a null, an
  // empty string or a missing key can never become the number 0, which is what
  // `Number(raw.x) || 0` would do and what would put a vessel on a heading of
  // due north or a speed of stopped.
  // ---------------------------------------------------------------------------

  function num(value) {
    if (value === null || value === undefined || value === "") return NaN;
    const n = Number(value);
    return Number.isFinite(n) ? n : NaN;
  }

  function str(value) {
    return typeof value === "string" ? value.trim() : "";
  }

  // orientationOf resolves the two directional fields into the two different
  // things this panel needs, and NEVER invents either:
  //
  //   orientDeg -- which way the hull points, for rotating the glyph. True
  //     heading first (that is literally what it measures), falling back to
  //     course over ground, which for a vessel making way is a close enough
  //     stand-in to be better than drawing no orientation at all.
  //   courseDeg -- which way the vessel is travelling, for the speed vector.
  //     COG first (that is literally what it measures), falling back to
  //     heading.
  //
  // Both stay NaN when the vessel has reported neither, and the callers draw a
  // ringed dot and no vector respectively.
  function orientationOf(heading, cog) {
    return {
      orientDeg: Number.isFinite(heading) ? heading : cog,
      courseDeg: Number.isFinite(cog) ? cog : heading,
    };
  }

  function normalise(raw) {
    // The MMSI is the entry's key on the C++ side and can never be absent; a
    // row without one is a payload bug and is dropped rather than drawn under
    // a made-up identity.
    const mmsi = str(raw.mmsi);

    const lat = num(raw.lat);
    const lon = num(raw.lon);
    // The backend has already applied the app's own validity gate
    // (Latitude/Longitude::is_valid()) and omits BOTH fields when it fails, so
    // a fix is present only when both are. The range check is a guard against
    // a corrupt payload, not a second gate: the projection clamps latitude to
    // ±85.05 (Mercator runs to infinity at the poles), so a lat of 95 would
    // otherwise be plotted confidently at the top of the world.
    //
    // Deliberately NOT excluded: exactly 0, 0. The contract forbids the
    // backend from ever sending that as "unknown", so here it can only mean a
    // vessel in the Gulf of Guinea, and silently deleting a real fix would be
    // its own kind of lie.
    const hasPos = Number.isFinite(lat) && Number.isFinite(lon) &&
      Math.abs(lat) <= 90 && Math.abs(lon) <= 180;

    const heading = num(raw.heading_deg);
    const cog = num(raw.cog_deg);
    const status = num(raw.nav_status);

    const v = {
      mmsi,
      name: str(raw.name),
      callsign: str(raw.callsign),
      destination: str(raw.destination),
      lat, lon, hasPos,
      sog_kn: num(raw.sog_kn),
      cog_deg: Number.isFinite(cog) ? cog : NaN,
      heading_deg: Number.isFinite(heading) ? heading : NaN,
      // 0..15 is the whole of ITU-R M.1371's status field; anything else is a
      // payload bug and is treated as not reported rather than shown as a
      // status this panel would have to invent a name for.
      nav_status: (Number.isInteger(status) && status >= 0 && status <= 15) ? status : NaN,
      msgs: num(raw.msgs),
      time: str(raw.time),
    };
    return Object.assign(v, orientationOf(v.heading_deg, v.cog_deg));
  }

  // Ships are slow. The ADS-B panel ignores position changes under ~15 m as CPR
  // jitter, which at 12 knots is four seconds of travel and would visibly
  // stair-step a vessel's trail; 5 m is roughly one ship's length of movement
  // and still filters the 1/10000-minute quantisation of the AIS position field.
  const TRAIL_MAX_POINTS = 240;
  const TRAIL_MIN_MOVE_DEG = 0.00005;

  function updateTrails(st, list) {
    const seen = new Set();
    for (const v of list) {
      seen.add(v.mmsi);
      if (!v.hasPos) continue;
      let t = st.trails.get(v.mmsi);
      if (!t) { t = []; st.trails.set(v.mmsi, t); }
      const last = t[t.length - 1];
      if (!last || Math.abs(last.lat - v.lat) > TRAIL_MIN_MOVE_DEG ||
        Math.abs(last.lon - v.lon) > TRAIL_MIN_MOVE_DEG) {
        t.push({ lat: v.lat, lon: v.lon });
        if (t.length > TRAIL_MAX_POINTS) t.shift();
      }
    }
    // Drop trails for vessels the app has aged out of its list, so a long
    // session does not accumulate the whole day's traffic in memory.
    for (const mmsi of Array.from(st.trails.keys())) {
      if (!seen.has(mmsi)) st.trails.delete(mmsi);
    }
    for (const mmsi of Array.from(st.firstSeen.keys())) {
      if (!seen.has(mmsi)) st.firstSeen.delete(mmsi);
    }
  }

  function passesFilter(st, v) {
    const p = st.prefs;
    if (p.posOnly && !v.hasPos) return false;
    const term = (p.search || "").trim().toUpperCase();
    if (!term) return true;
    return v.mmsi.includes(term) ||
      v.name.toUpperCase().includes(term) ||
      v.callsign.toUpperCase().includes(term) ||
      v.destination.toUpperCase().includes(term);
  }

  function sortList(st, list) {
    const key = st.prefs.sortKey;
    const dir = st.prefs.sortDir;
    const isText = key === "mmsi" || key === "name";
    return list.slice().sort((x, y) => {
      const a = x[key];
      const b = y[key];
      if (isText) {
        const as = (a || "").toString();
        const bs = (b || "").toString();
        // A blank name sorts last in both directions: those rows are the least
        // useful and pinning them to the top on every re-sort is just noise.
        if (!as && bs) return 1;
        if (as && !bs) return -1;
        return as.localeCompare(bs) * dir;
      }
      // Absent numbers sort last in both directions, for the same reason. They
      // are not zero and must never sort as if they were.
      const av = Number.isFinite(a) ? a : null;
      const bv = Number.isFinite(b) ? b : null;
      if (av === null && bv === null) return 0;
      if (av === null) return 1;
      if (bv === null) return -1;
      return (av - bv) * dir;
    });
  }

  // refreshDerived recomputes filtering, sorting and the table from whatever
  // snapshot is currently held. Split out from render() so a control change
  // repaints instantly without waiting for the next poll -- and so it still
  // works while paused.
  function refreshDerived(st) {
    st.filtered = sortList(st, st.vessels.filter((v) => passesFilter(st, v)));
    renderTable(st);
    updateStats(st);
    syncSortIndicators(st);

    if (st.prefs.follow && st.selectedId) {
      const sel = st.vessels.find((v) => v.mmsi === st.selectedId);
      if (sel && sel.hasPos) setCenter(st, sel.lat, sel.lon);
    }
    scheduleDraw(st);
  }

  function syncSortIndicators(st) {
    for (const th of st.headRow.children) {
      const col = COLUMNS.find((c) => c.key === th.dataset.key);
      const label = col ? col.label : th.dataset.key;
      if (th.dataset.key === st.prefs.sortKey) {
        th.setAttribute("aria-sort", st.prefs.sortDir > 0 ? "ascending" : "descending");
        th.textContent = `${label} ${st.prefs.sortDir > 0 ? "▲" : "▼"}`;
      } else {
        th.removeAttribute("aria-sort");
        th.textContent = label;
      }
    }
  }

  // cellText returns "" for every absent field. That empty string is the
  // contract's absent-stays-absent rule reaching the screen: a blank cell, not
  // a 0, not a dash that could be mistaken for a reported value.
  function cellText(st, v, key) {
    switch (key) {
      case "mmsi": return v.mmsi;
      case "name": return v.name;
      case "sog_kn": return fmtSpeed(v.sog_kn, st.units);
      case "cog_deg": return fmtDeg(v.cog_deg);
      case "nav_status": return navText(v.nav_status);
      case "msgs": return fmtInt(v.msgs);
      default: return "";
    }
  }

  // renderTable diffs by MMSI rather than rebuilding tbody, so the selected
  // row, the browser's text selection and the scroll position all survive a
  // poll tick.
  function renderTable(st) {
    const wanted = new Set();
    let prev = null;

    for (const v of st.filtered) {
      wanted.add(v.mmsi);
      let tr = st.rows.get(v.mmsi);
      if (!tr) {
        tr = document.createElement("tr");
        tr.dataset.mmsi = v.mmsi;
        for (const c of COLUMNS) {
          const td = document.createElement("td");
          if (c.num) td.className = "mp-ais-num";
          tr.appendChild(td);
        }
        st.rows.set(v.mmsi, tr);
        // Flash a genuinely new vessel, not one that merely re-entered the
        // current filter.
        if (!st.firstSeen.has(v.mmsi)) tr.classList.add("mp-ais-new");
      }
      if (!st.firstSeen.has(v.mmsi)) st.firstSeen.set(v.mmsi, Date.now());

      const cells = tr.children;
      for (let i = 0; i < COLUMNS.length; i++) {
        const key = COLUMNS[i].key;
        const text = cellText(st, v, key);
        const td = cells[i];
        if (td.textContent !== text) td.textContent = text;
        if (key === "mmsi") {
          td.className = "mp-ais-mmsi";
          td.style.borderLeft = `3px solid ${navColor(v)}`;
        }
      }
      tr.classList.toggle("mp-ais-sel", v.mmsi === st.selectedId);
      tr.classList.toggle("mp-ais-nopos", !v.hasPos);

      // Keep DOM order matching sort order with the minimum number of moves.
      const shouldFollow = prev ? prev.nextSibling : st.tbody.firstChild;
      if (shouldFollow !== tr) st.tbody.insertBefore(tr, shouldFollow);
      prev = tr;
    }

    for (const [mmsi, tr] of Array.from(st.rows)) {
      if (!wanted.has(mmsi)) {
        tr.remove();
        st.rows.delete(mmsi);
      }
    }

    st.emptyEl.style.display = st.filtered.length === 0 ? "block" : "none";
    st.emptyEl.textContent = st.vessels.length === 0
      ? "No vessels heard yet."
      : `No vessels match the current filters (${st.vessels.length} tracked).`;
  }

  function updateStats(st) {
    const withPos = st.vessels.filter((v) => v.hasPos).length;
    const parts = [
      `<span class="${st.paused ? "mp-ais-paused" : "mp-ais-live"}">${st.paused ? "PAUSED" : "LIVE"}</span>`,
      `<b>${st.vessels.length}</b> ${st.vessels.length === 1 ? "vessel" : "vessels"}`,
      // Labelled as the browser's own count, because it is: the payload carries
      // no such figure and this is derived from which vessels came with a
      // position. Presenting it unlabelled next to a backend statistic would
      // pass it off as one.
      `<b>${withPos}</b> with position <span class="mp-ais-derived">(counted in browser)</span>`,
      `<b>${st.filtered.length}</b> shown`,
    ];
    // Absent stays absent, in the stats line as much as in a table cell: with
    // no packets_valid on the wire the panel says nothing about it rather than
    // reporting a zero the backend never claimed.
    if (Number.isFinite(st.packetsValid)) {
      parts.push(`<b>${fmtInt(st.packetsValid)}</b> valid packets`);
    }
    st.statsEl.innerHTML = parts.join(" · ");
  }

  // drawNow paints synchronously and clears the pending-draw flag. New data
  // goes through here rather than only setting needsDraw, for two reasons: the
  // first paint should not wait a frame behind the table that is already on
  // screen, and requestAnimationFrame does not run at all in a hidden or
  // non-compositing tab -- so an rAF-only paint path is both slower to first
  // pixel and untestable outside a visible window.
  function drawNow(st) {
    st.needsDraw = false;
    sizeCanvas(st);
    draw(st);
  }

  // ---------------------------------------------------------------------------
  // Repaint scheduling
  //
  // Interaction-driven repaints (pan, zoom, a control toggled, a selection)
  // are coalesced two ways at once, and it takes both:
  //
  //   - the requestAnimationFrame loop below, which is what keeps a drag or a
  //     wheel-zoom at one paint per frame while the page is visible;
  //   - a 0 ms timeout here, because rAF DOES NOT RUN AT ALL in a hidden tab
  //     or a non-compositing pane, and an rAF-only path leaves those repaints
  //     simply never happening. This was measured, not assumed: with the flag
  //     alone, switching the units control in the verification pane left the
  //     scale bar reading "2 nm" over a chart that had been redrawn in km, and
  //     toggling the speed vectors changed nothing on the canvas at all.
  //
  // Whichever fires first paints; drawNow clears the flag, so the other finds
  // nothing to do. It is the same reasoning that puts scheduleTileDraw on a
  // timeout, applied to every other repaint the panel asks for.
  // ---------------------------------------------------------------------------

  function scheduleDraw(st) {
    st.needsDraw = true;
    if (st.drawPending) return;
    st.drawPending = window.setTimeout(() => {
      st.drawPending = 0;
      if (st.needsDraw && st.canvas.isConnected) drawNow(st);
    }, 0);
  }

  // The loop tests the CANVAS rather than el: a host that clears its mount
  // with innerHTML = "" (app.js's showPanel does, in several places) leaves el
  // itself connected while every child this panel built is detached, so an
  // el-only test would leave the old loop redrawing an orphaned canvas for the
  // life of the page.
  function startLoop(st) {
    const step = () => {
      if (!st.canvas.isConnected) return;
      if (st.needsDraw) drawNow(st);
      window.requestAnimationFrame(step);
    };
    window.requestAnimationFrame(step);
  }

  // ---------------------------------------------------------------------------
  // The contract
  // ---------------------------------------------------------------------------

  // liveState returns the stashed state only if the DOM it points at is still
  // in the document. The host clears its mount with `innerHTML = ""` in
  // several places (app.js's showPanel, mountScreenView, the error paths)
  // while el.__mp* and el.dataset.mpKind survive on the element itself, and
  // registry.js only drops __mp* when the KIND changes -- so a renderer that
  // trusts its stash can end up drawing into a canvas that is no longer on the
  // page. Testing a child, not el, is what catches that (geotable.js carries
  // the same check for the same reason).
  function liveState(el) {
    const st = el.__mpAis;
    return (st && st.el === el && st.canvas.isConnected) ? st : null;
  }

  function render(el, data) {
    const st = liveState(el) || buildSkeleton(el);
    const d = data || {};

    // No app name is read: unlike the older kinds, the `ais` payload carries
    // none. The envelope's `title` is shell chrome (PANELS.md), and inventing
    // a heading for a panel that was not given one is the same class of
    // mistake as inventing a field value.
    const stats = d.stats || {};
    const valid = num(stats.packets_valid);
    if (Number.isFinite(valid)) st.packetsValid = valid;

    if (!st.paused) {
      const list = Array.isArray(d.vessels)
        ? d.vessels.map(normalise).filter((v) => v.mmsi)
        : [];
      st.vessels = list;
      updateTrails(st, list);
      if (st.selectedId && !list.some((v) => v.mmsi === st.selectedId)) st.selectedId = null;
    }

    refreshDerived(st);

    if (!st.hasFitted) {
      sizeCanvas(st);
      if (fitTo(st, st.vessels.filter((v) => v.hasPos))) st.hasFitted = true;
    }

    drawNow(st);
  }

  window.MayhemPanels.registerPanel("ais", render);
})();
