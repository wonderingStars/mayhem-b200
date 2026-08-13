// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// The "adsb" panel: a live situational-awareness view for ADS-B RX, in the
// spirit of globe.adsbexchange.com / tar1090 -- an altitude-coloured traffic
// map with trails, cross-linked to a sortable aircraft table.
//
// This is NOT a second decoder. Every field it draws is already computed by
// src/apps/ui_adsb_rx.cpp (AircraftTracker / AircraftRecentEntry: ICAO,
// callsign, CPR-decoded lat/lon, altitude, ground speed, track, squawk, hit
// count, age and frame amplitude) and merely published as JSON. The only
// state this file derives for itself is presentational:
//
//   - position TRAILS, accumulated client-side from successive polls. The C++
//     side keeps a trail only for the aircraft whose detail page is open
//     (AdsbRxDetailsView::pos_history_, private), so rather than widen the
//     app's state for the web view, the browser remembers what it has been
//     told. A reload starts trails over; nothing else depends on them.
//   - messages/sec, derived from the frames_accepted delta between polls.
//   - max range seen, derived from home position + reported positions.
//
// The plot is true Web Mercator (EPSG:3857) because there is an OpenStreetMap
// basemap under it and OSM tiles ARE Web Mercator: an equirectangular plot
// beneath Mercator tiles misplaces every target vertically, by kilometres over
// a one-degree span at UK latitudes. Tiles come from the portal's own
// /api/tiles/{z}/{x}/{y}.png, and any failure at all is treated as "no
// basemap" -- a B200 on a bench with no internet is a normal operating case,
// not an error to report. See the tile section below for the two obligations
// the OSM tile usage policy places on this side (attribution, demand-driven
// requests only).
//
// Why the styles are injected from here instead of living in panels.css:
// this panel is self-contained on purpose, so it can be dropped in (and the
// harness can exercise it) without touching the shared stylesheet or the
// host page's script tags. ensureStyle() appends one <style> to document.head
// exactly once per document.
//
// Per PANELS.md the whole contract is render(el, data); there is no mount vs
// update and no destroy hook, so all per-instance state hangs off el.__mpAdsb
// and the redraw loop cancels itself once el leaves the document.

(() => {
  "use strict";

  const { escapeHtml, clamp } = window.MayhemPanels.util;

  // ---------------------------------------------------------------------------
  // Styles
  // ---------------------------------------------------------------------------

  const STYLE_ID = "mp-adsb-style";
  const CSS = `
.mp-adsb { display: flex; flex-direction: column; gap: 8px; min-height: 420px; height: 100%; }
.mp-adsb-stats { display: flex; flex-wrap: wrap; align-items: baseline; gap: 4px 14px;
  font: 12px var(--mp-sans, sans-serif); color: var(--mp-text-dim, #8896a8); }
.mp-adsb-stats b { color: var(--mp-text, #dde4ee); font-weight: 600; font-variant-numeric: tabular-nums; }
.mp-adsb-stats .mp-adsb-live { color: var(--mp-ok, #3ddc84); }
.mp-adsb-stats .mp-adsb-paused { color: var(--mp-warn, #f5a623); }

.mp-adsb-bar { display: flex; flex-wrap: wrap; gap: 6px; align-items: center; }
.mp-adsb-bar input[type="search"], .mp-adsb-bar input[type="number"], .mp-adsb-bar select {
  background: var(--mp-panel-2, #161e2a); color: var(--mp-text, #dde4ee);
  border: 1px solid var(--mp-border, #232d3b); border-radius: 6px;
  padding: 4px 7px; font: 12px var(--mp-sans, sans-serif); }
.mp-adsb-bar input[type="search"] { min-width: 130px; }
.mp-adsb-bar input[type="number"] { width: 74px; }
.mp-adsb-bar label { display: inline-flex; align-items: center; gap: 5px;
  font: 12px var(--mp-sans, sans-serif); color: var(--mp-text-dim, #8896a8); white-space: nowrap; }
.mp-adsb-btn { background: var(--mp-panel-2, #161e2a); color: var(--mp-text, #dde4ee);
  border: 1px solid var(--mp-border, #232d3b); border-radius: 6px; padding: 4px 9px;
  font: 12px var(--mp-sans, sans-serif); cursor: pointer; }
.mp-adsb-btn:hover { border-color: var(--mp-accent-dim, #1f8c7f); }
.mp-adsb-btn[aria-pressed="true"] { border-color: var(--mp-accent, #34d8c3);
  color: var(--mp-accent, #34d8c3); }

.mp-adsb-body { display: grid; grid-template-columns: 1fr; grid-template-rows: minmax(240px, 1fr) minmax(150px, auto);
  gap: 8px; flex: 1; min-height: 0; }
@media (min-width: 900px) {
  .mp-adsb-body { grid-template-columns: minmax(0, 3fr) minmax(320px, 2fr); grid-template-rows: 1fr; }
}

.mp-adsb-mapwrap { position: relative; min-height: 220px; min-width: 0;
  background: var(--mp-bg, #0a0e14); border: 1px solid var(--mp-border, #232d3b);
  border-radius: var(--mp-radius, 8px); overflow: hidden; }
.mp-adsb-mapwrap canvas { display: block; width: 100%; height: 100%; cursor: grab; touch-action: none; }
.mp-adsb-mapwrap canvas.mp-adsb-dragging { cursor: grabbing; }
.mp-adsb-ctl { position: absolute; top: 8px; right: 8px; display: flex; flex-direction: column; gap: 4px; }
.mp-adsb-ctl .mp-adsb-btn { width: 30px; height: 28px; padding: 0; text-align: center; }
.mp-adsb-ctl .mp-adsb-btn.mp-adsb-wide { width: auto; padding: 0 8px; }
.mp-adsb-scale { position: absolute; left: 8px; bottom: 8px; font: 11px var(--mp-mono, monospace);
  color: var(--mp-text-dim, #a09681); background: rgba(10,14,20,0.7); padding: 2px 6px; border-radius: 4px; }
.mp-adsb-tip { position: absolute; pointer-events: none; z-index: 3; max-width: 240px;
  background: rgba(10,14,20,0.95); border: 1px solid var(--mp-border, #232d3b);
  border-radius: 6px; padding: 6px 8px; font: 11px var(--mp-mono, monospace);
  color: var(--mp-text, #dde4ee); box-shadow: var(--mp-shadow, 0 6px 18px rgba(0,0,0,.5)); }
.mp-adsb-tip b { color: var(--mp-accent, #34d8c3); }
.mp-adsb-legend { position: absolute; left: 8px; top: 8px; display: flex; align-items: center; gap: 6px;
  background: rgba(10,14,20,0.7); padding: 3px 6px; border-radius: 4px;
  font: 10px var(--mp-mono, monospace); color: var(--mp-text-dim, #a09681); }
.mp-adsb-ramp { width: 108px; height: 8px; border-radius: 2px; }
/* Attribution sits above every other map overlay (the tooltip included) on
   purpose: the OSM tile usage policy requires it to be visible whenever the
   tiles are, so nothing this panel draws is allowed to cover it.
   It uses the full-strength text colour rather than the dim one the other
   overlays use: the attribution guidelines ask for text that is easily
   readable, and at 10px over a 78%-opaque box the dim token measured 4.4:1
   against the composited background -- under WCAG AA, and worse still over a
   pale patch of map. The bright token is ~10:1 on the same background. */
.mp-adsb-attrib { position: absolute; right: 6px; bottom: 5px; z-index: 4;
  background: rgba(10,14,20,0.78); padding: 2px 6px; border-radius: 4px;
  font: 10px var(--mp-sans, sans-serif); color: var(--mp-text, #dde4ee); }
.mp-adsb-attrib a { color: inherit; text-decoration: none; }
.mp-adsb-attrib a:hover { text-decoration: underline; }
.mp-adsb-offline { position: absolute; right: 6px; bottom: 5px; z-index: 4;
  background: rgba(10,14,20,0.78); padding: 2px 6px; border-radius: 4px;
  font: 10px var(--mp-sans, sans-serif); color: var(--mp-text-dim, #a09681); }

.mp-adsb-tablewrap { min-height: 0; overflow: auto; border: 1px solid var(--mp-border, #232d3b);
  border-radius: var(--mp-radius, 8px); background: var(--mp-panel, #121821); }
table.mp-adsb-table { border-collapse: collapse; width: 100%;
  font: 11px var(--mp-mono, monospace); color: var(--mp-text, #dde4ee); }
table.mp-adsb-table th { position: sticky; top: 0; z-index: 1; text-align: left; white-space: nowrap;
  background: var(--mp-panel-2, #161e2a); color: var(--mp-text-dim, #8896a8);
  border-bottom: 1px solid var(--mp-border, #232d3b); padding: 5px 7px; cursor: pointer;
  font-weight: 600; user-select: none; }
table.mp-adsb-table th:hover { color: var(--mp-text, #dde4ee); }
table.mp-adsb-table th[aria-sort] { color: var(--mp-accent, #34d8c3); }
table.mp-adsb-table td { padding: 3px 7px; white-space: nowrap;
  border-bottom: 1px solid var(--mp-border-soft, #1a222e); font-variant-numeric: tabular-nums; }
table.mp-adsb-table td.mp-adsb-num { text-align: right; }
table.mp-adsb-table tbody tr { cursor: pointer; }
table.mp-adsb-table tbody tr:hover { background: rgba(91,157,255,0.10); }
table.mp-adsb-table tbody tr.mp-adsb-sel { background: rgba(52,216,195,0.16); }
table.mp-adsb-table tbody tr.mp-adsb-new td { animation: mp-adsb-flash 6s ease-out 1; }
@keyframes mp-adsb-flash { from { background: rgba(61,220,132,0.42); } to { background: transparent; } }
table.mp-adsb-table td.mp-adsb-flight { color: var(--mp-accent, #34d8c3); }
table.mp-adsb-table tr.mp-adsb-stale td { color: var(--mp-text-dim, #a09681); }
table.mp-adsb-table td.mp-adsb-sq-alert { color: var(--mp-danger, #ef5464); font-weight: 700; }
.mp-adsb-swatch { display: inline-block; width: 7px; height: 7px; border-radius: 50%; margin-right: 5px;
  vertical-align: baseline; }
.mp-adsb-empty { padding: 14px; text-align: center; font: 12px var(--mp-sans, sans-serif);
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
  // Geo + formatting helpers
  // ---------------------------------------------------------------------------

  // Ground distances (range rings, the distance column) are computed on the
  // WGS84 mean sphere. Note that the projection below is defined on a
  // DIFFERENT sphere -- Web Mercator's 6378137 m -- so a scale-bar length and
  // a great-circle distance disagree by 0.11%, 112 m in 100 km. Both are
  // right for their own job: the tiles are drawn on the Mercator sphere and
  // a ground distance is not, and 0.11% is a tenth of the pixel rounding on
  // the scale bar. Do not "unify" them by projecting on 6371.
  const EARTH_KM = 6371.0088;
  const D2R = Math.PI / 180;

  function haversineKm(lat1, lon1, lat2, lon2) {
    const dLat = (lat2 - lat1) * D2R;
    const dLon = (lon2 - lon1) * D2R;
    const a = Math.sin(dLat / 2) ** 2 +
      Math.cos(lat1 * D2R) * Math.cos(lat2 * D2R) * Math.sin(dLon / 2) ** 2;
    return 2 * EARTH_KM * Math.asin(Math.min(1, Math.sqrt(a)));
  }

  function bearingDeg(lat1, lon1, lat2, lon2) {
    const y = Math.sin((lon2 - lon1) * D2R) * Math.cos(lat2 * D2R);
    const x = Math.cos(lat1 * D2R) * Math.sin(lat2 * D2R) -
      Math.sin(lat1 * D2R) * Math.cos(lat2 * D2R) * Math.cos((lon2 - lon1) * D2R);
    return (Math.atan2(y, x) / D2R + 360) % 360;
  }

  // altitudeColor is the familiar tar1090/adsbexchange ramp: low traffic warm,
  // high traffic cool, so a glance at the map reads as a vertical profile. On
  // the ground is deliberately desaturated so parked/taxiing targets recede.
  function altitudeColor(ft, onGround) {
    if (onGround) return "#6b7f95";
    if (!Number.isFinite(ft)) return "#8896a8";
    const t = clamp(ft, 0, 40000) / 40000;
    const hue = 20 + t * 280; // 20 (orange) -> 300 (violet)
    return `hsl(${hue.toFixed(0)}, 85%, 58%)`;
  }

  const COMPASS = ["N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
    "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"];
  function compassOf(deg) {
    if (!Number.isFinite(deg)) return "";
    return COMPASS[Math.round(((deg % 360) + 360) % 360 / 22.5) % 16];
  }

  // Emergency/alert squawks worth colouring: hijack, radio failure, mayday.
  const ALERT_SQUAWKS = new Set([7500, 7600, 7700]);

  function fmtInt(v) {
    return Number.isFinite(v) ? Math.round(v).toLocaleString() : "";
  }

  function fmtAlt(ft, units) {
    if (!Number.isFinite(ft)) return "";
    return units.metric ? `${fmtInt(ft * 0.3048)} m` : `${fmtInt(ft)} ft`;
  }

  function fmtSpeed(kt, units) {
    if (!Number.isFinite(kt) || kt <= 0) return "";
    return units.metric ? `${fmtInt(kt * 1.852)} km/h` : `${fmtInt(kt)} kt`;
  }

  function fmtDist(km, units) {
    if (!Number.isFinite(km)) return "";
    return units.metric ? `${km.toFixed(1)} km` : `${(km / 1.852).toFixed(1)} nm`;
  }

  // Level flight is never exactly 0 fpm in practice, so treat a small rate as
  // level rather than flickering the arrow on every frame.
  const LEVEL_FPM = 192;

  function climbArrow(fpm) {
    if (!Number.isFinite(fpm) || Math.abs(fpm) < LEVEL_FPM) return "";
    return fpm > 0 ? " ↑" : " ↓";
  }

  function fmtVerticalRate(fpm, units) {
    if (!Number.isFinite(fpm)) return "";
    if (Math.abs(fpm) < LEVEL_FPM) return "level";
    return units.metric
      ? `${(fpm * 0.00508).toFixed(1)} m/s`
      : `${fmtInt(fpm)} ft/min`;
  }

  function fmtAgeShort(s) {
    if (!Number.isFinite(s)) return "";
    if (s < 60) return `${Math.round(s)}s`;
    const m = Math.floor(s / 60);
    return m < 60 ? `${m}m${Math.round(s % 60)}s` : `${Math.floor(m / 60)}h${m % 60}m`;
  }

  // ---------------------------------------------------------------------------
  // Preferences (units, home position, filters) -- kept across reloads because
  // an operator's home position and preferred units do not change per session.
  // localStorage is best-effort: a blocked/full store must not break the panel.
  // ---------------------------------------------------------------------------

  const PREFS_KEY = "mayhem.adsb.prefs.v1";
  const DEFAULT_PREFS = {
    metric: false,
    posOnly: false,
    altBand: "all",
    maxDistKm: 0,
    search: "",
    trails: true,
    rings: true,
    basemap: true,
    follow: false,
    homeLat: null,
    homeLon: null,
    sortKey: "dist",
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
  // Everything works in slippy-map world-pixel space -- the world is a
  // TILE_SIZE * 2^zoom square -- which is the same space the tile grid is
  // indexed in, so aircraft, trails, rings and the tiles beneath them cannot
  // disagree. Screen position is world position minus the centre's, so pan is
  // a translation and only zoom rescales.
  //
  // zoom is continuous. The tile layer rounds it to an integer tile z and
  // scales by 2^(zoom - z), so zooming stays smooth between tile levels rather
  // than snapping.
  //
  // All drawing and hit-testing still happens in CSS-pixel space.
  // ---------------------------------------------------------------------------

  const TILE_SIZE = 256;
  // Mercator y runs to infinity at the poles, so the slippy-map convention cuts
  // the world square off here, making it square. Latitudes must be clamped to
  // this before projecting or the arithmetic blows up near the poles.
  const MERC_MAX_LAT = 85.05112878;
  const ZOOM_MIN = 2;
  const ZOOM_MAX = 19; // the deepest z the tile endpoint accepts
  const DEFAULT_ZOOM = 9;
  // Ground resolution at the equator at zoom 0: the earth's circumference over
  // one 256 px tile. Every metres-per-pixel figure derives from this.
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

  // Ground resolution at a latitude: what the scale bar reads and what the
  // range-ring spacing is chosen from. Only true along the parallel it is
  // evaluated at -- that is Mercator.
  function metresPerPixel(lat, zoom) {
    return MPP_EQUATOR_Z0 * Math.cos(clamp(lat, -MERC_MAX_LAT, MERC_MAX_LAT) * D2R) /
      Math.pow(2, zoom);
  }

  // Panning east or west accumulates the centre longitude past ±180 (the tile
  // column index wraps, the centre does not); labels want the usual range.
  function normLon(lon) { return ((lon + 180) % 360 + 360) % 360 - 180; }

  // project takes the copy of a longitude NEAREST THE CENTRE, which is what
  // makes the overlay agree with the tile layer across the antimeridian.
  // Panning east runs st.centerLon past +180 and the tiles follow it (their
  // column index wraps, the centre does not), so with the centre reading
  // 188.3 a target reported at -179.9 is the same place as +180.1 and belongs
  // just left of the middle of the canvas -- not 368° away, 16 000 px
  // off-screen, which is what the raw difference gives.
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
    st.needsDraw = true;
  }

  // panBy moves the centre by a screen delta. Done in world pixels rather than
  // degrees because a degree of latitude is not a constant number of pixels in
  // Mercator -- dragging in degrees would slide faster than the map.
  function panBy(st, dx, dy) {
    const wx = lonToWorldX(st.centerLon, st.zoom) - dx;
    const wy = latToWorldY(st.centerLat, st.zoom) - dy;
    st.centerLon = worldXToLon(wx, st.zoom);
    st.centerLat = worldYToLat(clamp(wy, 0, worldSize(st.zoom)), st.zoom);
    st.needsDraw = true;
  }

  // zoomAt leaves the geographic point under (x, y) exactly where it is, which
  // is what makes wheel and double-click zoom feel anchored to the cursor: it
  // re-derives the centre from the anchor at the new scale instead of scaling
  // about the middle of the canvas.
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
    st.needsDraw = true;
  }

  // Smallest box fitTo will zoom to, so one aircraft (or two a few metres
  // apart) does not slam the map to maximum zoom.
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
    // Measure the box in zoom-0 world pixels, then solve for the zoom that
    // makes it fit: world pixels scale by exactly 2^zoom, so the answer is a
    // log2 of the ratio rather than a search. The centre is the middle of the
    // box in PROJECTED space, not the mean of the latitudes -- in Mercator
    // those are different points.
    const x0 = lonToWorldX(minLon, 0), x1 = lonToWorldX(maxLon, 0);
    const y0 = latToWorldY(maxLat, 0), y1 = latToWorldY(minLat, 0);
    st.centerLon = worldXToLon((x0 + x1) / 2, 0);
    st.centerLat = worldYToLat((y0 + y1) / 2, 0);
    const w = Math.max(120, st.canvas.clientWidth) * 0.82;
    const h = Math.max(120, st.canvas.clientHeight) * 0.82;
    st.zoom = clamp(Math.min(Math.log2(w / Math.max(x1 - x0, 1e-9)),
      Math.log2(h / Math.max(y1 - y0, 1e-9))), ZOOM_MIN, ZOOM_MAX);
    st.needsDraw = true;
    return true;
  }

  // destPoint solves the great-circle direct problem: the point distKm away
  // from (lat, lon) on the given bearing. Range rings are drawn by sampling
  // this, because a constant-distance locus is not a screen circle in Mercator.
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

  // ---------------------------------------------------------------------------
  // Basemap tiles
  //
  // Tiles are fetched from the portal's own endpoint, root-relative so the page
  // never names a host, and never straight from the browser to OSM: the caching,
  // revalidation and User-Agent the OSM tile usage policy requires all live
  // server-side. Two of that policy's obligations land on this side and are not
  // negotiable:
  //
  //   - "© OpenStreetMap contributors" is on screen whenever tiles are. It is
  //     not behind the basemap toggle, not under other overlays, not off-screen.
  //   - Requests are strictly DEMAND-DRIVEN: only tiles the current viewport is
  //     about to draw. No neighbour prefetch, no other zoom level, no warm-up,
  //     no "download this area" -- bulk downloading and pre-seeding are
  //     prohibited, and nothing of the sort should be added later.
  //
  // Any non-200 (503 tiles disabled/offline, 400, a block, no network at all) is
  // just "no basemap": a failed tile is remembered as failed so it is never
  // retried in a draw loop, and once several requests have failed with nothing
  // ever having loaded the layer gives up entirely rather than storming a dead
  // endpoint. The panel then looks exactly as it did before tiles existed.
  // ---------------------------------------------------------------------------

  const TILE_URL_BASE = "/api/tiles/";
  // A few hundred tiles is several screenfuls of pan history; past that the
  // oldest ones that are not on screen are dropped.
  const TILE_CACHE_MAX = 320;
  const TILE_GIVEUP_FAILURES = 4;

  function tileKey(z, x, y) { return `${z}/${x}/${y}`; }

  // scheduleTileDraw coalesces "a tile arrived" into one repaint. A timeout
  // rather than requestAnimationFrame, for the same reason drawNow exists: rAF
  // does not run in a hidden or non-compositing tab, and a tile that has landed
  // belongs on the canvas either way.
  function scheduleTileDraw(st) {
    if (st.tileDrawPending) return;
    st.tileDrawPending = window.setTimeout(() => {
      st.tileDrawPending = 0;
      if (st.el.isConnected) drawNow(st);
    }, 0);
  }

  // requestTile returns a drawable image, or null while one is in flight, has
  // failed, or the layer has been written off. It is the only place a tile is
  // ever requested, and it is only called for a tile the current frame draws.
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
      // Nothing has ever loaded and several requests have failed: the endpoint
      // is disabled or this box is offline. Stop asking; the graticule is a
      // perfectly good map.
      if (st.tileLoads === 0 && st.tileFailures >= TILE_GIVEUP_FAILURES) {
        st.tilesUnavailable = true;
      }
      scheduleTileDraw(st);
    });
    entry.img.src = TILE_URL_BASE + key + ".png";
    return null;
  }

  // drawTiles paints the basemap and reports whether anything actually landed
  // on the canvas -- which is what decides whether the attribution is shown and
  // whether the graticule switches to its over-imagery colours.
  function drawTiles(st, ctx, w, h) {
    if (!st.prefs.basemap || st.tilesUnavailable) return false;

    // Tile z follows the map zoom in CSS pixels, deliberately ignoring
    // devicePixelRatio: on a 2x display that upscales the imagery slightly
    // rather than fetching four times as many tiles for the same view. Target
    // positions are unaffected (they are projected, not sampled), and asking
    // OSM's servers for a quarter of the tiles is the friendlier default.
    const z = clamp(Math.round(st.zoom), 0, ZOOM_MAX);
    const scale = Math.pow(2, st.zoom - z); // CSS pixels per tile pixel
    const size = TILE_SIZE * scale;
    const n = Math.pow(2, z);
    // Screen position of the top-left corner of tile (0, 0).
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
      // knocks it back so the altitude colours and labels stay the foreground.
      ctx.fillStyle = "rgba(10,14,20,0.30)";
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

  // The attribution is a licence condition and the offline note is the whole of
  // the "no basemap" reporting: one quiet line, no dialog, nothing to dismiss.
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

  // The graticule is still worth having over the tiles at low contrast, and
  // with the basemap off it is the only spatial reference there is.
  //
  // In Mercator longitude is linear in x but latitude is NOT linear in y, so
  // the parallels are stepped in DEGREES and projected one at a time; spacing
  // them evenly down the canvas (as the old equirectangular code could) would
  // mislabel every line but the middle one.
  function drawGraticule(st, ctx, w, h, overTiles) {
    const pxPerDegLon = worldSize(st.zoom) / 360;
    const lonStep = niceStep(pxPerDegLon);
    // A degree of latitude is 1/cos(phi) times as TALL as a degree of
    // longitude is wide in Mercator, so the parallels need their own step:
    // choosing both from the longitude figure spaces them 1/cos(phi) too far
    // apart (121 px instead of 73 at 53N, 214 px at 70N).
    const pxPerDegLat = pxPerDegLon /
      Math.cos(clamp(st.centerLat, -MERC_MAX_LAT, MERC_MAX_LAT) * D2R);
    const latStep = niceStep(pxPerDegLat);

    const nw = unproject(st, 0, 0);
    const se = unproject(st, w, h);
    const minLat = clamp(se.lat, -MERC_MAX_LAT, MERC_MAX_LAT);
    const maxLat = clamp(nw.lat, -MERC_MAX_LAT, MERC_MAX_LAT);

    ctx.save();
    ctx.strokeStyle = overTiles ? "rgba(255,255,255,0.14)" : "rgba(255,255,255,0.06)";
    ctx.fillStyle = overTiles ? "rgba(221,228,238,0.85)" : "rgba(91,107,128,0.9)";
    ctx.font = "10px monospace";
    ctx.lineWidth = 1;

    // Over map imagery a bare label disappears into whatever is behind it, so
    // it gets the same dark halo the aircraft labels use.
    const label = (text, x, y) => {
      if (overTiles) {
        const fill = ctx.fillStyle;
        ctx.lineWidth = 3;
        ctx.strokeStyle = "rgba(10,14,20,0.85)";
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

  // Range rings answer the question a table cannot: "how far out am I actually
  // hearing anything?" Spacing follows the current zoom so there are always a
  // few visible rings rather than none or fifty.
  // Ring radii and scale-bar lengths are chosen as nice numbers IN THE UNIT
  // BEING DISPLAYED, not picked in km and then converted. Converting is what
  // makes a bar 0.50254 km long carry the label "0.3 nm" (0.5556 km): the
  // number is a rounded conversion of a metric length, so the scale an
  // operator reads off the map is wrong by up to 11%. Same ladder either way,
  // just applied to whichever unit is on screen.
  const RANGE_STEPS = [0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 400];
  const KM_PER_NM = 1.852;

  function kmPerUnit(units) { return units.metric ? 1 : KM_PER_NM; }

  // The smallest ladder step that is at least minPx wide on screen.
  function rangeStep(pxPerUnit, minPx) {
    for (const s of RANGE_STEPS) {
      if (s * pxPerUnit >= minPx) return s;
    }
    return RANGE_STEPS[RANGE_STEPS.length - 1];
  }

  // Ladder values are exact but accumulating them is not (0.1 + 0.1 + 0.1 is
  // 0.30000000000000004), and that would be printed verbatim into a label.
  function fmtRange(v) { return String(Number(v.toFixed(3))); }

  function rangeLabel(v, metric) {
    if (metric) return v < 1 ? `${Math.round(v * 1000)} m` : `${fmtRange(v)} km`;
    // Below a couple of cables, nautical miles stop being a readable number.
    return v < 0.2 ? `${Math.round(v * KM_PER_NM * 3280.84)} ft` : `${fmtRange(v)} nm`;
  }

  const RING_SAMPLES = 180;  // 2° of bearing per segment: smooth at any size
  const RING_MAX_COUNT = 12; // enough to read; a bound on the work per frame
  const RING_MAX_KM = 800;
  function drawRangeRings(st, ctx, w, h) {
    if (!st.home) return;
    const centre = project(st, st.home.lat, st.home.lon);
    const unitKm = kmPerUnit(st.units);
    const pxPerKm = 1000 / metresPerPixel(st.home.lat, st.zoom);

    // Which radii are worth drawing: sample the viewport border for the nearest
    // and furthest ground distance visible. Home is often off-screen after a
    // manual pan, and then the interesting rings are the ones crossing the view
    // rather than the first few out from a point nobody can see.
    let nearKm = Infinity;
    let farKm = 0;
    for (let i = 0; i < 8; i++) {
      const t = i / 8;
      for (const [px, py] of [[t * w, 0], [t * w, h], [0, t * h], [w, t * h]]) {
        const g = unproject(st, px, py);
        const d = haversineKm(st.home.lat, st.home.lon, g.lat, g.lon);
        if (d < nearKm) nearKm = d;
        if (d > farKm) farKm = d;
      }
    }
    if (centre.x >= 0 && centre.x <= w && centre.y >= 0 && centre.y <= h) nearKm = 0;

    // Everything from here is in display units; only destPoint sees km.
    const step = rangeStep(pxPerKm * unitKm, 55);
    const first = Math.max(step, Math.ceil(nearKm / unitKm / step) * step);
    const farU = farKm / unitKm;

    ctx.save();
    ctx.strokeStyle = "rgba(52,216,195,0.16)";
    ctx.fillStyle = "rgba(52,216,195,0.5)";
    ctx.font = "10px monospace";
    ctx.lineWidth = 1;
    for (let drawn = 0; drawn < RING_MAX_COUNT; drawn++) {
      const r = Number((first + drawn * step).toFixed(3));
      if (r > farU || r * unitKm > RING_MAX_KM) break;
      // A range ring is a circle on the globe, not on the screen. In Mercator a
      // constant-distance locus is neither round nor centred on home once you
      // are away from the map centre, so it is sampled at a constant great-
      // circle distance around the compass and every point projected.
      let north = null;
      ctx.beginPath();
      for (let i = 0; i <= RING_SAMPLES; i++) {
        const d = destPoint(st.home.lat, st.home.lon, (i / RING_SAMPLES) * 360, r * unitKm);
        const p = project(st, d.lat, d.lon);
        if (i === 0) { north = p; ctx.moveTo(p.x, p.y); } else { ctx.lineTo(p.x, p.y); }
      }
      ctx.stroke();
      ctx.fillText(rangeLabel(r, st.units.metric), north.x + 3, north.y - 3);
    }
    ctx.restore();
  }

  // The receiver's own position is drawn whenever one is known, not as part of
  // the rings: "Rings" turns off the range overlay, and an operator who has
  // turned it off has not asked to lose sight of where their aerial is.
  function drawHome(st, ctx) {
    if (!st.home) return;
    const centre = project(st, st.home.lat, st.home.lon);
    ctx.save();
    ctx.translate(centre.x, centre.y);
    ctx.strokeStyle = "#34d8c3";
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(-6, 0); ctx.lineTo(6, 0);
    ctx.moveTo(0, -6); ctx.lineTo(0, 6);
    ctx.stroke();
    ctx.beginPath();
    ctx.arc(0, 0, 3.5, 0, Math.PI * 2);
    ctx.stroke();
    ctx.restore();
  }

  function drawTrail(st, ctx, trail, color, selected) {
    if (!trail || trail.length < 2) return;
    ctx.save();
    ctx.lineWidth = selected ? 2 : 1.2;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    // Fade the tail: older segments are more transparent, so direction of
    // travel is readable without arrowheads.
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

  function drawAircraft(ctx, x, y, color, heading, selected, label) {
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
    if (Number.isFinite(heading)) ctx.rotate(heading * D2R);
    // A small plan-view airframe reads better than an arrow at a glance: nose,
    // swept wings, tailplane.
    ctx.beginPath();
    ctx.moveTo(0, -8);
    ctx.lineTo(1.8, -2.5);
    ctx.lineTo(8.5, 2.5);
    ctx.lineTo(8.5, 4.2);
    ctx.lineTo(1.8, 2.4);
    ctx.lineTo(1.5, 6);
    ctx.lineTo(4, 8);
    ctx.lineTo(4, 9);
    ctx.lineTo(0, 7.8);
    ctx.lineTo(-4, 9);
    ctx.lineTo(-4, 8);
    ctx.lineTo(-1.5, 6);
    ctx.lineTo(-1.8, 2.4);
    ctx.lineTo(-8.5, 4.2);
    ctx.lineTo(-8.5, 2.5);
    ctx.lineTo(-1.8, -2.5);
    ctx.closePath();
    ctx.fillStyle = color;
    ctx.fill();
    ctx.strokeStyle = "rgba(0,0,0,0.55)";
    ctx.lineWidth = 0.8;
    ctx.stroke();
    ctx.restore();

    if (label) {
      ctx.font = selected ? "bold 11px monospace" : "10px monospace";
      ctx.textAlign = "left";
      ctx.lineWidth = 3;
      ctx.strokeStyle = "rgba(10,14,20,0.85)";
      ctx.strokeText(label, 11, 4);
      ctx.fillStyle = selected ? "#ffffff" : "#dde4ee";
      ctx.fillText(label, 11, 4);
    }
    ctx.restore();
  }

  function drawScaleBar(st) {
    // Ground resolution at the centre latitude. Mercator's scale is only true
    // along the parallel it is measured at, and the middle of the view is the
    // one an operator is looking at. The length is a nice number of whatever
    // unit is displayed, so the bar is exactly as long as its label says.
    const pxPerUnit = kmPerUnit(st.units) * 1000 / metresPerPixel(st.centerLat, st.zoom);
    const v = rangeStep(pxPerUnit, 60);
    st.scaleEl.textContent = rangeLabel(v, st.units.metric);
    st.scaleEl.style.width = `${Math.round(v * pxPerUnit)}px`;
    // currentColor, not a named token: an inline style beats every stylesheet,
    // so naming a colour here put the ruler permanently out of the theme's
    // reach. It used to be --mp-text-faint, which measures 2.2:1 against this
    // overlay's own box over a pale tile -- under the 3:1 WCAG 1.4.11 asks of
    // a graphical object, and unfixable from panels.css even after that sheet
    // had correctly moved the scale's *text* up to --mp-text-dim. Following
    // the element's own colour keeps the bar and its label in step for good.
    st.scaleEl.style.borderBottom = "2px solid currentColor";
  }

  // Above this zoom the view is a few tens of km across and every target can
  // carry its callsign without the labels colliding. (It is the same density
  // the old equirectangular threshold of 900 px per degree gave.)
  const LABEL_ZOOM = 10.3;

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
    if (st.prefs.rings) drawRangeRings(st, ctx, w, h);
    drawHome(st, ctx);

    const visible = st.filtered.filter((a) => a.hasPos);
    st.hit = [];

    if (st.prefs.trails) {
      for (const a of visible) {
        drawTrail(st, ctx, st.trails.get(a.icao), altitudeColor(a.altitude_ft, a.on_ground),
          a.icao === st.selectedId);
      }
    }

    for (const a of visible) {
      const p = project(st, a.lat, a.lon);
      if (p.x < -60 || p.y < -60 || p.x > w + 60 || p.y > h + 60) continue;
      const selected = a.icao === st.selectedId;
      const label = st.zoom >= LABEL_ZOOM || selected ? (a.callsign || a.icao) : "";
      drawAircraft(ctx, p.x, p.y, altitudeColor(a.altitude_ft, a.on_ground),
        a.heading_deg, selected, label);
      st.hit.push({ icao: a.icao, x: p.x, y: p.y });
    }

    drawScaleBar(st);
  }

  function hitTest(st, x, y) {
    let best = null;
    let bestD = 15;
    for (const h of st.hit) {
      const d = Math.hypot(h.x - x, h.y - y);
      if (d < bestD) { bestD = d; best = h.icao; }
    }
    return best;
  }

  // ---------------------------------------------------------------------------
  // Skeleton
  // ---------------------------------------------------------------------------

  const COLUMNS = [
    { key: "callsign", label: "Flight", num: false },
    { key: "icao", label: "ICAO", num: false },
    { key: "altitude_ft", label: "Alt", num: true },
    { key: "speed_kt", label: "Spd", num: true },
    { key: "heading_deg", label: "Trk", num: true },
    { key: "squawk", label: "Sqk", num: true },
    { key: "dist", label: "Dist", num: true },
    { key: "brg", label: "Brg", num: true },
    { key: "messages", label: "Msgs", num: true },
    { key: "age_s", label: "Age", num: true },
    { key: "rssi", label: "Sig", num: true },
  ];

  function buildSkeleton(el) {
    ensureStyle();
    el.innerHTML = "";
    el.classList.add("mp-panel", "mp-adsb");

    const prefs = loadPrefs();

    const stats = document.createElement("div");
    stats.className = "mp-adsb-stats";
    el.appendChild(stats);

    const bar = document.createElement("div");
    bar.className = "mp-adsb-bar";
    bar.innerHTML = `
      <input type="search" data-f="search" placeholder="Flight / ICAO…" />
      <select data-f="altBand" title="Altitude band">
        <option value="all">All altitudes</option>
        <option value="ground">On ground</option>
        <option value="low">below 10 000 ft</option>
        <option value="mid">10 000 – 25 000 ft</option>
        <option value="high">above 25 000 ft</option>
      </select>
      <label>Max <input type="number" data-f="maxDist" min="0" step="10" placeholder="∞" /> <span data-f="distUnit">nm</span></label>
      <label><input type="checkbox" data-f="posOnly" /> Positions only</label>
      <button class="mp-adsb-btn" data-f="trails" aria-pressed="true">Trails</button>
      <button class="mp-adsb-btn" data-f="rings" aria-pressed="true">Rings</button>
      <button class="mp-adsb-btn" data-f="basemap" aria-pressed="true" title="OpenStreetMap basemap">Basemap</button>
      <button class="mp-adsb-btn" data-f="follow" aria-pressed="false">Follow</button>
      <button class="mp-adsb-btn" data-f="units">Units: imperial</button>
      <button class="mp-adsb-btn" data-f="pause" aria-pressed="false">Pause</button>
      <button class="mp-adsb-btn" data-f="home" title="Set the receiver position used for range rings, distance and bearing">Set home…</button>
    `;
    el.appendChild(bar);

    const body = document.createElement("div");
    body.className = "mp-adsb-body";

    const mapwrap = document.createElement("div");
    mapwrap.className = "mp-adsb-mapwrap";
    const canvas = document.createElement("canvas");
    mapwrap.appendChild(canvas);

    const ctl = document.createElement("div");
    ctl.className = "mp-adsb-ctl";
    ctl.innerHTML = `
      <button class="mp-adsb-btn" data-f="zin" title="Zoom in">+</button>
      <button class="mp-adsb-btn" data-f="zout" title="Zoom out">−</button>
      <button class="mp-adsb-btn mp-adsb-wide" data-f="fit" title="Fit all aircraft">Fit</button>
    `;
    mapwrap.appendChild(ctl);

    const legend = document.createElement("div");
    legend.className = "mp-adsb-legend";
    legend.innerHTML = `<span>0</span><span class="mp-adsb-ramp"></span><span>40k ft</span>`;
    legend.querySelector(".mp-adsb-ramp").style.background =
      `linear-gradient(90deg, ${[0, 0.25, 0.5, 0.75, 1]
        .map((t) => altitudeColor(t * 40000, false)).join(",")})`;
    mapwrap.appendChild(legend);

    const scale = document.createElement("div");
    scale.className = "mp-adsb-scale";
    mapwrap.appendChild(scale);

    // Attribution: required by the OSM tile usage policy whenever the tiles are
    // on screen, so it is created here rather than being anything a control can
    // switch off, and it links to the copyright page as the policy asks.
    const attrib = document.createElement("div");
    attrib.className = "mp-adsb-attrib";
    attrib.innerHTML = '<a href="https://www.openstreetmap.org/copyright" ' +
      'target="_blank" rel="noopener noreferrer">© OpenStreetMap contributors</a>';
    attrib.style.display = "none";
    mapwrap.appendChild(attrib);

    // The entire report for "no basemap": one line, no dialog, nothing to
    // acknowledge. It only appears if the operator asked for a basemap.
    const offline = document.createElement("div");
    offline.className = "mp-adsb-offline";
    offline.textContent = "Basemap unavailable — running offline";
    offline.style.display = "none";
    mapwrap.appendChild(offline);

    const tip = document.createElement("div");
    tip.className = "mp-adsb-tip";
    tip.style.display = "none";
    mapwrap.appendChild(tip);

    body.appendChild(mapwrap);

    const tablewrap = document.createElement("div");
    tablewrap.className = "mp-adsb-tablewrap";
    const table = document.createElement("table");
    table.className = "mp-adsb-table";
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
    empty.className = "mp-adsb-empty";
    empty.textContent = "No aircraft heard yet.";
    tablewrap.appendChild(empty);
    body.appendChild(tablewrap);

    el.appendChild(body);

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
      // Tile layer: cache keyed "z/x/y", counters behind the give-up rule, and
      // the coalescing handle for "a tile arrived, repaint".
      tiles: new Map(),
      tileLoads: 0,
      tileFailures: 0,
      tilesUnavailable: false,
      tileDrawPending: 0,
      hasFitted: false,
      aircraft: [],
      filtered: [],
      trails: new Map(),
      firstSeen: new Map(),
      rows: new Map(),
      hit: [],
      selectedId: null,
      home: (Number.isFinite(prefs.homeLat) && Number.isFinite(prefs.homeLon))
        ? { lat: prefs.homeLat, lon: prefs.homeLon } : null,
      homeFromServer: false,
      paused: false,
      lastAccepted: null,
      lastAcceptedAt: 0,
      msgsPerSec: 0,
      maxRangeKm: 0,
      framesSeen: 0,
      framesAccepted: 0,
      dragging: false,
      dragMoved: false,
      lastX: 0,
      lastY: 0,
      needsDraw: true,
      appName: "ADS-B RX",
    };
    el.__mpAdsb = st;

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

  function persist(st) {
    Object.assign(st.prefs, {
      metric: st.units.metric,
      homeLat: st.home ? st.home.lat : null,
      homeLon: st.home ? st.home.lon : null,
    });
    savePrefs(st.prefs);
  }

  function q(st, sel) {
    return st.bar.querySelector(`[data-f="${sel}"]`);
  }

  function syncControls(st) {
    q(st, "search").value = st.prefs.search || "";
    q(st, "altBand").value = st.prefs.altBand || "all";
    q(st, "maxDist").value = st.prefs.maxDistKm
      ? (st.units.metric ? st.prefs.maxDistKm : st.prefs.maxDistKm / 1.852).toFixed(0)
      : "";
    q(st, "distUnit").textContent = st.units.metric ? "km" : "nm";
    q(st, "posOnly").checked = !!st.prefs.posOnly;
    q(st, "trails").setAttribute("aria-pressed", String(!!st.prefs.trails));
    q(st, "rings").setAttribute("aria-pressed", String(!!st.prefs.rings));
    q(st, "basemap").setAttribute("aria-pressed", String(!!st.prefs.basemap));
    q(st, "follow").setAttribute("aria-pressed", String(!!st.prefs.follow));
    q(st, "units").textContent = st.units.metric ? "Units: metric" : "Units: imperial";
    q(st, "pause").setAttribute("aria-pressed", String(st.paused));
    q(st, "pause").textContent = st.paused ? "Resume" : "Pause";
  }

  function wireControls(st) {
    q(st, "search").addEventListener("input", (ev) => {
      st.prefs.search = ev.target.value;
      savePrefs(st.prefs);
      refreshDerived(st);
    });
    q(st, "altBand").addEventListener("change", (ev) => {
      st.prefs.altBand = ev.target.value;
      savePrefs(st.prefs);
      refreshDerived(st);
    });
    q(st, "maxDist").addEventListener("input", (ev) => {
      const v = parseFloat(ev.target.value);
      const km = Number.isFinite(v) && v > 0 ? (st.units.metric ? v : v * 1.852) : 0;
      st.prefs.maxDistKm = km;
      savePrefs(st.prefs);
      refreshDerived(st);
    });
    q(st, "posOnly").addEventListener("change", (ev) => {
      st.prefs.posOnly = ev.target.checked;
      savePrefs(st.prefs);
      refreshDerived(st);
    });

    for (const key of ["trails", "rings", "follow"]) {
      q(st, key).addEventListener("click", () => {
        st.prefs[key] = !st.prefs[key];
        savePrefs(st.prefs);
        syncControls(st);
        st.needsDraw = true;
      });
    }

    q(st, "basemap").addEventListener("click", () => {
      st.prefs.basemap = !st.prefs.basemap;
      if (st.prefs.basemap) {
        // Switching it back on is an explicit ask, and the only moment it is
        // reasonable to try an endpoint that has already been written off --
        // the operator may well have just plugged the network back in. Only the
        // failed entries are dropped; tiles already in hand are still good.
        st.tilesUnavailable = false;
        st.tileFailures = 0;
        for (const [key, entry] of Array.from(st.tiles)) {
          if (entry.failed) st.tiles.delete(key);
        }
      }
      savePrefs(st.prefs);
      syncControls(st);
      st.needsDraw = true;
    });

    q(st, "units").addEventListener("click", () => {
      st.units.metric = !st.units.metric;
      persist(st);
      syncControls(st);
      renderTable(st);
      st.needsDraw = true;
    });

    q(st, "pause").addEventListener("click", () => {
      st.paused = !st.paused;
      syncControls(st);
      updateStats(st);
    });

    // Home position: a prompt rather than a modal keeps this panel free of any
    // shell dependency. Accepts "51.5, -0.12" or two space-separated numbers;
    // an empty answer clears it back to the server-reported position (if any).
    q(st, "home").addEventListener("click", () => {
      const cur = st.home ? `${st.home.lat.toFixed(5)}, ${st.home.lon.toFixed(5)}` : "";
      const ans = window.prompt(
        "Receiver position as \"lat, lon\" in decimal degrees.\nLeave blank to clear.", cur);
      if (ans === null) return;
      const t = ans.trim();
      if (t === "") {
        st.home = null;
        st.maxRangeKm = 0;
        persist(st);
        refreshDerived(st);
        return;
      }
      const m = t.split(/[,\s]+/).map(parseFloat);
      if (m.length < 2 || !Number.isFinite(m[0]) || !Number.isFinite(m[1]) ||
        Math.abs(m[0]) > 90 || Math.abs(m[1]) > 180) {
        window.alert("Could not read that as a latitude and longitude.");
        return;
      }
      st.home = { lat: m[0], lon: m[1] };
      st.maxRangeKm = 0;
      persist(st);
      refreshDerived(st);
    });

    st.mapwrap.querySelector('[data-f="zin"]').addEventListener("click", () =>
      zoomAt(st, st.canvas.clientWidth / 2, st.canvas.clientHeight / 2, 1.5));
    st.mapwrap.querySelector('[data-f="zout"]').addEventListener("click", () =>
      zoomAt(st, st.canvas.clientWidth / 2, st.canvas.clientHeight / 2, 1 / 1.5));
    st.mapwrap.querySelector('[data-f="fit"]').addEventListener("click", () => {
      const pts = st.filtered.filter((a) => a.hasPos);
      if (st.home) pts.push(st.home);
      fitTo(st, pts);
    });
  }

  function wireCanvas(st) {
    const c = st.canvas;

    if (window.ResizeObserver) {
      new ResizeObserver(() => { sizeCanvas(st); st.needsDraw = true; }).observe(st.mapwrap);
    } else {
      window.addEventListener("resize", () => { sizeCanvas(st); st.needsDraw = true; });
    }

    c.addEventListener("pointerdown", (ev) => {
      st.dragging = true;
      st.dragMoved = false;
      st.lastX = ev.clientX;
      st.lastY = ev.clientY;
      c.setPointerCapture(ev.pointerId);
      c.classList.add("mp-adsb-dragging");
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
        st.needsDraw = true;
        return;
      }

      const icao = hitTest(st, px, py);
      showTip(st, icao, px, py);
      highlightRow(st, icao, false);
    });

    const endDrag = (ev) => {
      if (!st.dragging) return;
      st.dragging = false;
      c.classList.remove("mp-adsb-dragging");
      try { c.releasePointerCapture(ev.pointerId); } catch (_) { /* already released */ }
    };
    c.addEventListener("pointerup", (ev) => {
      const wasDrag = st.dragMoved;
      endDrag(ev);
      if (wasDrag) return;
      const rect = c.getBoundingClientRect();
      const icao = hitTest(st, ev.clientX - rect.left, ev.clientY - rect.top);
      select(st, icao);
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
      if (!tr || !tr.dataset.icao) return;
      select(st, tr.dataset.icao);
      const a = st.aircraft.find((x) => x.icao === tr.dataset.icao);
      if (a && a.hasPos) setCenter(st, a.lat, a.lon);
    });

    st.tbody.addEventListener("mouseover", (ev) => {
      const tr = ev.target.closest("tr");
      if (tr && tr.dataset.icao) showTipForRow(st, tr.dataset.icao);
    });
    st.tbody.addEventListener("mouseleave", () => { st.tipEl.style.display = "none"; });
  }

  function select(st, icao) {
    st.selectedId = (icao && icao === st.selectedId) ? null : (icao || null);
    highlightRow(st, st.selectedId, true);
    st.needsDraw = true;
  }

  function highlightRow(st, icao, selectedClass) {
    for (const [id, tr] of st.rows) {
      tr.classList.toggle("mp-adsb-sel", id === (selectedClass ? icao : st.selectedId));
    }
    if (selectedClass && icao) {
      const tr = st.rows.get(icao);
      if (tr && tr.scrollIntoView) tr.scrollIntoView({ block: "nearest" });
    }
  }

  function tipHtml(st, a) {
    const rows = [
      ["Callsign", a.callsign || "—"],
      ["ICAO", a.icao],
      ["Altitude", a.on_ground ? "on ground" : (fmtAlt(a.altitude_ft, st.units) || "—")],
      ["Speed", fmtSpeed(a.speed_kt, st.units) || "—"],
      ["Vertical", fmtVerticalRate(a.vertical_rate_fpm, st.units) || "—"],
      ["Track", Number.isFinite(a.heading_deg)
        ? `${Math.round(a.heading_deg)}° ${compassOf(a.heading_deg)}` : "—"],
      ["Squawk", a.squawk ? String(a.squawk).padStart(4, "0") : "—"],
      ["Distance", Number.isFinite(a.dist) ? fmtDist(a.dist, st.units) : "—"],
      ["Bearing", Number.isFinite(a.brg) ? `${Math.round(a.brg)}° ${compassOf(a.brg)}` : "—"],
      ["Messages", fmtInt(a.messages) || "—"],
      ["Last seen", fmtAgeShort(a.age_s) || "—"],
      ["Signal", Number.isFinite(a.rssi) ? String(Math.round(a.rssi)) : "—"],
    ];
    const body = rows.map(([k, v]) =>
      `<div>${escapeHtml(k)}: <b>${escapeHtml(v)}</b></div>`).join("");
    const info = a.info ? `<div style="opacity:.7">${escapeHtml(a.info)}</div>` : "";
    return body + info;
  }

  function showTip(st, icao, px, py) {
    if (!icao) { st.tipEl.style.display = "none"; return; }
    const a = st.aircraft.find((x) => x.icao === icao);
    if (!a) { st.tipEl.style.display = "none"; return; }
    st.tipEl.innerHTML = tipHtml(st, a);
    st.tipEl.style.display = "block";
    const w = st.mapwrap.clientWidth;
    const h = st.mapwrap.clientHeight;
    const tw = st.tipEl.offsetWidth;
    const th = st.tipEl.offsetHeight;
    st.tipEl.style.left = `${clamp(px + 14, 4, Math.max(4, w - tw - 4))}px`;
    st.tipEl.style.top = `${clamp(py + 14, 4, Math.max(4, h - th - 4))}px`;
  }

  function showTipForRow(st, icao) {
    const a = st.aircraft.find((x) => x.icao === icao);
    if (!a) return;
    st.tipEl.innerHTML = tipHtml(st, a);
    st.tipEl.style.display = "block";
    st.tipEl.style.left = "8px";
    st.tipEl.style.top = "8px";
  }

  // ---------------------------------------------------------------------------
  // Data shaping
  // ---------------------------------------------------------------------------

  function normalise(raw) {
    const lat = Number(raw.lat);
    const lon = Number(raw.lon);
    // An out-of-range latitude is a corrupt fix, and it has to be DROPPED
    // rather than drawn: the projection clamps to ±85.05 (it has to, Mercator
    // runs to infinity at the poles), so a lat of 95 would otherwise be
    // plotted confidently at the top of the world. Same reasoning as the
    // 0N 0E check next to it.
    const hasPos = raw.has_pos !== false &&
      Number.isFinite(lat) && Number.isFinite(lon) &&
      Math.abs(lat) <= 90 && Math.abs(lon) <= 180 &&
      !(lat === 0 && lon === 0);
    return {
      icao: String(raw.icao || raw.id || "").toUpperCase(),
      callsign: (raw.callsign || "").trim(),
      lat, lon, hasPos,
      altitude_ft: Number.isFinite(Number(raw.altitude_ft)) ? Number(raw.altitude_ft) : NaN,
      speed_kt: Number.isFinite(Number(raw.speed_kt)) ? Number(raw.speed_kt) : NaN,
      heading_deg: Number.isFinite(Number(raw.heading_deg)) ? Number(raw.heading_deg) : NaN,
      vertical_rate_fpm: Number.isFinite(Number(raw.vertical_rate_fpm))
        ? Number(raw.vertical_rate_fpm) : NaN,
      squawk: Number(raw.squawk) || 0,
      messages: Number(raw.messages) || 0,
      age_s: Number.isFinite(Number(raw.age_s)) ? Number(raw.age_s) : NaN,
      rssi: Number.isFinite(Number(raw.rssi)) ? Number(raw.rssi) : NaN,
      state: raw.state || "",
      on_ground: !!raw.on_ground,
      info: raw.info || "",
    };
  }

  // Shortest gap between two frame-counter samples that yields a trustworthy
  // messages/sec figure.
  const MIN_RATE_INTERVAL_S = 0.5;

  const TRAIL_MAX_POINTS = 240;
  const TRAIL_MIN_MOVE_DEG = 0.00015; // ~15 m; below this it is just CPR jitter

  function updateTrails(st, list) {
    const seen = new Set();
    for (const a of list) {
      seen.add(a.icao);
      if (!a.hasPos) continue;
      let t = st.trails.get(a.icao);
      if (!t) { t = []; st.trails.set(a.icao, t); }
      const last = t[t.length - 1];
      if (!last || Math.abs(last.lat - a.lat) > TRAIL_MIN_MOVE_DEG ||
        Math.abs(last.lon - a.lon) > TRAIL_MIN_MOVE_DEG) {
        t.push({ lat: a.lat, lon: a.lon, alt: a.altitude_ft });
        if (t.length > TRAIL_MAX_POINTS) t.shift();
      }
    }
    // Drop trails for aircraft the tracker has aged out, so a long session
    // does not accumulate the whole day's traffic in memory.
    for (const icao of Array.from(st.trails.keys())) {
      if (!seen.has(icao)) st.trails.delete(icao);
    }
    for (const icao of Array.from(st.firstSeen.keys())) {
      if (!seen.has(icao)) st.firstSeen.delete(icao);
    }
  }

  function passesFilter(st, a) {
    const p = st.prefs;
    if (p.posOnly && !a.hasPos) return false;

    const term = (p.search || "").trim().toUpperCase();
    if (term && !a.icao.includes(term) && !a.callsign.toUpperCase().includes(term)) return false;

    if (p.altBand === "ground") {
      if (!a.on_ground) return false;
    } else if (p.altBand !== "all") {
      if (!Number.isFinite(a.altitude_ft)) return false;
      if (p.altBand === "low" && !(a.altitude_ft < 10000)) return false;
      if (p.altBand === "mid" && !(a.altitude_ft >= 10000 && a.altitude_ft <= 25000)) return false;
      if (p.altBand === "high" && !(a.altitude_ft > 25000)) return false;
    }

    if (p.maxDistKm > 0) {
      if (!Number.isFinite(a.dist)) return false;
      if (a.dist > p.maxDistKm) return false;
    }
    return true;
  }

  function sortList(st, list) {
    const key = st.prefs.sortKey;
    const dir = st.prefs.sortDir;
    const isText = key === "callsign" || key === "icao";
    return list.slice().sort((x, y) => {
      let a = x[key];
      let b = y[key];
      if (isText) {
        a = (a || "").toString();
        b = (b || "").toString();
        // Blank callsigns sort last regardless of direction: they are the
        // least useful rows and pinning them to the top on every re-sort is
        // just noise.
        if (!a && b) return 1;
        if (a && !b) return -1;
        return a.localeCompare(b) * dir;
      }
      const av = Number.isFinite(a) ? a : null;
      const bv = Number.isFinite(b) ? b : null;
      if (av === null && bv === null) return 0;
      if (av === null) return 1;
      if (bv === null) return -1;
      return (av - bv) * dir;
    });
  }

  // refreshDerived recomputes distance/bearing, filtering, sorting and the
  // table from whatever aircraft snapshot is currently held. Split out from
  // render() so a control change repaints instantly without waiting for the
  // next poll -- and so it still works while paused.
  function refreshDerived(st) {
    for (const a of st.aircraft) {
      if (st.home && a.hasPos) {
        a.dist = haversineKm(st.home.lat, st.home.lon, a.lat, a.lon);
        a.brg = bearingDeg(st.home.lat, st.home.lon, a.lat, a.lon);
        if (a.dist > st.maxRangeKm) st.maxRangeKm = a.dist;
      } else {
        a.dist = NaN;
        a.brg = NaN;
      }
    }
    st.filtered = sortList(st, st.aircraft.filter((a) => passesFilter(st, a)));
    renderTable(st);
    updateStats(st);
    syncSortIndicators(st);

    if (st.prefs.follow && st.selectedId) {
      const sel = st.aircraft.find((a) => a.icao === st.selectedId);
      if (sel && sel.hasPos) setCenter(st, sel.lat, sel.lon);
    }
    st.needsDraw = true;
  }

  function syncSortIndicators(st) {
    for (const th of st.headRow.children) {
      if (th.dataset.key === st.prefs.sortKey) {
        th.setAttribute("aria-sort", st.prefs.sortDir > 0 ? "ascending" : "descending");
        const col = COLUMNS.find((c) => c.key === th.dataset.key);
        th.textContent = `${col ? col.label : th.dataset.key} ${st.prefs.sortDir > 0 ? "▲" : "▼"}`;
      } else {
        th.removeAttribute("aria-sort");
        const col = COLUMNS.find((c) => c.key === th.dataset.key);
        th.textContent = col ? col.label : th.dataset.key;
      }
    }
  }

  function cellText(st, a, key) {
    switch (key) {
      case "callsign": return a.callsign || "";
      case "icao": return a.icao;
      case "altitude_ft": {
        if (a.on_ground) return "grnd";
        const alt = fmtAlt(a.altitude_ft, st.units);
        // The arrow annotates an altitude; on its own it would say nothing.
        return alt ? alt + climbArrow(a.vertical_rate_fpm) : "";
      }
      case "speed_kt": return fmtSpeed(a.speed_kt, st.units);
      case "heading_deg": return Number.isFinite(a.heading_deg) ? `${Math.round(a.heading_deg)}°` : "";
      case "squawk": return a.squawk ? String(a.squawk).padStart(4, "0") : "";
      case "dist": return fmtDist(a.dist, st.units);
      case "brg": return Number.isFinite(a.brg) ? `${Math.round(a.brg)}°` : "";
      case "messages": return fmtInt(a.messages);
      case "age_s": return fmtAgeShort(a.age_s);
      case "rssi": return Number.isFinite(a.rssi) ? String(Math.round(a.rssi)) : "";
      default: return "";
    }
  }

  // renderTable diffs by ICAO rather than rebuilding tbody, so the selected
  // row, the browser's text selection and the scroll position all survive a
  // poll tick.
  function renderTable(st) {
    const wanted = new Set();
    let prev = null;

    for (const a of st.filtered) {
      wanted.add(a.icao);
      let tr = st.rows.get(a.icao);
      if (!tr) {
        tr = document.createElement("tr");
        tr.dataset.icao = a.icao;
        for (const c of COLUMNS) {
          const td = document.createElement("td");
          if (c.num) td.className = "mp-adsb-num";
          tr.appendChild(td);
        }
        st.rows.set(a.icao, tr);
        // Flash a genuinely new contact, but only once we have known about it
        // for less than a poll or two -- not for a row that merely re-entered
        // the current filter.
        if (!st.firstSeen.has(a.icao)) tr.classList.add("mp-adsb-new");
      }
      if (!st.firstSeen.has(a.icao)) st.firstSeen.set(a.icao, Date.now());

      const cells = tr.children;
      for (let i = 0; i < COLUMNS.length; i++) {
        const key = COLUMNS[i].key;
        const text = cellText(st, a, key);
        const td = cells[i];
        if (td.textContent !== text) td.textContent = text;
        if (key === "callsign") {
          td.className = "mp-adsb-flight";
          const dot = altitudeColor(a.altitude_ft, a.on_ground);
          td.style.borderLeft = `3px solid ${dot}`;
        } else if (key === "squawk") {
          td.className = (COLUMNS[i].num ? "mp-adsb-num" : "") +
            (ALERT_SQUAWKS.has(a.squawk) ? " mp-adsb-sq-alert" : "");
        }
      }
      tr.classList.toggle("mp-adsb-sel", a.icao === st.selectedId);
      tr.classList.toggle("mp-adsb-stale", a.state === "old" || a.state === "expired");

      // Keep DOM order matching sort order with the minimum number of moves.
      const shouldFollow = prev ? prev.nextSibling : st.tbody.firstChild;
      if (shouldFollow !== tr) st.tbody.insertBefore(tr, shouldFollow);
      prev = tr;
    }

    for (const [icao, tr] of Array.from(st.rows)) {
      if (!wanted.has(icao)) {
        tr.remove();
        st.rows.delete(icao);
      }
    }

    st.emptyEl.style.display = st.filtered.length === 0 ? "block" : "none";
    st.emptyEl.textContent = st.aircraft.length === 0
      ? "No aircraft heard yet."
      : `No aircraft match the current filters (${st.aircraft.length} tracked).`;
  }

  function updateStats(st) {
    const withPos = st.aircraft.filter((a) => a.hasPos).length;
    const parts = [
      `<span class="${st.paused ? "mp-adsb-paused" : "mp-adsb-live"}">${st.paused ? "PAUSED" : "LIVE"}</span>`,
      `<b>${st.aircraft.length}</b> tracked`,
      `<b>${withPos}</b> with position`,
      `<b>${st.filtered.length}</b> shown`,
      `<b>${st.msgsPerSec.toFixed(1)}</b> msg/s`,
      `<b>${fmtInt(st.framesAccepted)}</b> frames`,
    ];
    if (st.maxRangeKm > 0) parts.push(`max range <b>${fmtDist(st.maxRangeKm, st.units)}</b>`);
    if (!st.home) parts.push(`<span style="opacity:.75">set a home position for range and bearing</span>`);
    st.statsEl.innerHTML = parts.join(" · ");
  }

  // drawNow paints synchronously and clears the pending-draw flag. New data
  // goes through here rather than only setting needsDraw, for two reasons:
  // the first paint should not wait a frame behind the table that is already
  // on screen, and requestAnimationFrame does not run at all in a hidden or
  // non-compositing tab -- so an rAF-only paint path is both slower to first
  // pixel and untestable outside a visible window.
  function drawNow(st) {
    st.needsDraw = false;
    sizeCanvas(st);
    draw(st);
  }

  // ---------------------------------------------------------------------------
  // Redraw loop, for interaction-driven repaints (pan, zoom, selection) which
  // are coalesced to one paint per frame. Self-cancelling: PANELS.md
  // deliberately has no destroy hook, so the loop stops itself once its
  // element leaves the document.
  // ---------------------------------------------------------------------------

  function startLoop(st) {
    let lastAgeTick = 0;
    const step = (ts) => {
      if (!st.el.isConnected) return;
      if (st.needsDraw) drawNow(st);
      // Ages keep counting between polls so a stalled feed is obvious rather
      // than looking frozen-but-fresh.
      if (ts - lastAgeTick > 1000) {
        lastAgeTick = ts;
        if (!st.paused) {
          for (const a of st.aircraft) {
            if (Number.isFinite(a.age_s)) a.age_s += 1;
          }
          for (const [icao, tr] of st.rows) {
            const a = st.aircraft.find((x) => x.icao === icao);
            if (!a) continue;
            const idx = COLUMNS.findIndex((c) => c.key === "age_s");
            if (idx >= 0) tr.children[idx].textContent = fmtAgeShort(a.age_s);
          }
        }
      }
      window.requestAnimationFrame(step);
    };
    window.requestAnimationFrame(step);
  }

  // ---------------------------------------------------------------------------
  // The contract
  // ---------------------------------------------------------------------------

  function render(el, data) {
    const st = el.__mpAdsb && el.__mpAdsb.el === el ? el.__mpAdsb : buildSkeleton(el);
    const d = data || {};

    st.appName = d.app_name || st.appName;

    // A server-supplied receiver position is only adopted when the operator
    // has not set one of their own; theirs always wins.
    if (d.home && Number.isFinite(Number(d.home.lat)) && Number.isFinite(Number(d.home.lon)) &&
      (!st.home || st.homeFromServer)) {
      st.home = { lat: Number(d.home.lat), lon: Number(d.home.lon) };
      st.homeFromServer = true;
    }

    const stats = d.stats || {};
    if (Number.isFinite(Number(stats.frames_accepted))) {
      const now = Date.now();
      const accepted = Number(stats.frames_accepted);
      const dt = (now - st.lastAcceptedAt) / 1000;
      // Only derive a rate from a long enough baseline. Two renders in quick
      // succession (a control change landing in the same tick as a poll, or a
      // burst after the tab regains focus) divide a real frame delta by a
      // near-zero dt and report a wildly overstated rate; below the floor,
      // keep the previous figure and wait for a usable interval.
      if (st.lastAccepted !== null && dt >= MIN_RATE_INTERVAL_S) {
        const delta = accepted - st.lastAccepted;
        // A decreasing counter means the app restarted its tracker; treat it
        // as a fresh baseline rather than reporting a negative rate.
        if (delta >= 0) {
          st.msgsPerSec = st.msgsPerSec * 0.6 + (delta / dt) * 0.4;
        }
        st.lastAccepted = accepted;
        st.lastAcceptedAt = now;
      } else if (st.lastAccepted === null) {
        st.lastAccepted = accepted;
        st.lastAcceptedAt = now;
      }
      st.framesAccepted = accepted;
    }
    if (Number.isFinite(Number(stats.frames_seen))) st.framesSeen = Number(stats.frames_seen);

    if (!st.paused) {
      const list = Array.isArray(d.aircraft) ? d.aircraft.map(normalise).filter((a) => a.icao) : [];
      st.aircraft = list;
      updateTrails(st, list);
      if (st.selectedId && !list.some((a) => a.icao === st.selectedId)) st.selectedId = null;
    }

    refreshDerived(st);

    if (!st.hasFitted) {
      const pts = st.aircraft.filter((a) => a.hasPos);
      if (st.home) pts.push(st.home);
      sizeCanvas(st);
      if (fitTo(st, pts)) st.hasFitted = true;
    }

    drawNow(st);
  }

  window.MayhemPanels.registerPanel("adsb", render);
})();
