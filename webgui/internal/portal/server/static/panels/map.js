// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// The "map" panel: markers with lat/lon/label/heading on a pannable/zoomable
// canvas map -- over an OpenStreetMap street basemap when the portal can reach
// one, over a bare lat/lon graticule when it cannot. The shape is generic to
// "things with a position" (see ../../../PANELS.md).
//
// Two hosts reach this renderer: WardriveMap, the only backend that publishes
// the `map` kind directly, and the `geotable` panel, which mounts this as its
// upper half for AIS and APRS (radiosonde and EPIRB are queued behind the same
// kind). Both hand over the contract's marker shape and normalizeMarkers()
// reconciles it with the one drawn here -- see the comment there, and do not
// add a second copy of that adaptation in a caller. The basemap reaches every
// one of those apps the same way, through the one renderer: geotable.js
// composes this panel and needed no change to gain it.
//
// PROJECTION is true Web Mercator (EPSG:3857), and once there is a basemap
// that is a correctness requirement rather than a style choice: OSM tiles ARE
// Web Mercator. The equirectangular projection this panel used before can be
// made to agree with them at exactly one latitude and nowhere else, so every
// marker would sit off the street it is actually on -- ~310 m half a degree
// north of centre at UK latitudes, ~1.25 km a degree north, and worst zoomed
// out. Same formulas, same reasoning as panels/adsb.js.
//
// THE MARKERS NEVER WAIT FOR THE MAP. Tiles are painted first when they have
// arrived, the graticule is drawn either way, and the markers go on top of
// whatever landed. A machine with no basemap -- an air-gapped install, a bench
// B200 with no internet, a portal started with `-tiles off` -- draws exactly
// what this panel drew before tiles existed and says so in one quiet line. A
// tile that never arrives leaves the graticule showing through its slot, so
// there is no state in which this panel is a half-loaded checkerboard.
//
// All drawing happens in CSS-pixel space (ctx is reset to a single dpr scale
// transform per frame), so hit-testing against pointer events never has to
// convert coordinate spaces.

(() => {
  "use strict";

  const { clamp } = window.MayhemPanels.util;

  const KIND_COLORS = {
    aircraft: "#5b9dff",
    vessel: "#f5a623",
  };
  const DEFAULT_COLOR = "#34d8c3";

  function colorFor(kind) {
    return KIND_COLORS[kind] || DEFAULT_COLOR;
  }

  // ---------------------------------------------------------------------------
  // Projection: Web Mercator in slippy-map world-pixel space.
  //
  // The world is a TILE_SIZE * 2^zoom square, which is the space the tile grid
  // is indexed in, so markers and the tiles beneath them cannot disagree.
  // Screen position is world position minus the centre's, so panning is a
  // translation and only zoom rescales.
  //
  // zoom is continuous. The tile layer rounds it to an integer tile z and
  // scales by 2^(zoom - z), so zooming stays smooth between tile levels.
  // ---------------------------------------------------------------------------

  const D2R = Math.PI / 180;
  const TILE_SIZE = 256;
  // Mercator y runs to infinity at the poles, so the slippy-map convention cuts
  // the world off here, making it square. Latitudes are clamped to this before
  // projecting or the arithmetic blows up near the poles.
  const MERC_MAX_LAT = 85.05112878;
  const ZOOM_MIN = 2;
  const ZOOM_MAX = 19; // the deepest z the tile endpoint accepts
  // The view before any marker has arrived. 2^12 * 256 / 360 = 2913 px per
  // degree of longitude, which is the scale the equirectangular version of this
  // panel opened at (pxPerDeg 3000).
  const DEFAULT_ZOOM = 12;

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

  // Panning east or west accumulates the centre longitude past ±180 (the tile
  // column index wraps, the centre does not); labels want the usual range.
  function normLon(lon) { return ((lon + 180) % 360 + 360) % 360 - 180; }

  // project takes the copy of a longitude NEAREST THE CENTRE, which is what
  // keeps markers agreeing with the tile layer across the antimeridian: with
  // the centre reading 188.3 after panning east, a contact reported at -179.9
  // is the same place as +180.1 and belongs just left of the middle of the
  // canvas -- not 368° away and 16 000 px off-screen.
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

  // panBy moves the centre by a screen delta, in world pixels rather than
  // degrees: a degree of latitude is not a constant number of pixels in
  // Mercator, so panning in degrees would slide faster than the map under it.
  function panBy(st, dx, dy) {
    const wx = lonToWorldX(st.centerLon, st.zoom) - dx;
    const wy = latToWorldY(st.centerLat, st.zoom) - dy;
    st.centerLon = worldXToLon(wx, st.zoom);
    st.centerLat = worldYToLat(clamp(wy, 0, worldSize(st.zoom)), st.zoom);
  }

  // zoomAt leaves the geographic point under (x, y) exactly where it is, so
  // wheel zoom stays anchored to the cursor.
  function zoomAt(st, x, y, factor) {
    const anchor = unproject(st, x, y);
    const zoom = clamp(st.zoom + Math.log2(factor), ZOOM_MIN, ZOOM_MAX);
    if (zoom === st.zoom) return;
    st.zoom = zoom;
    const cx = st.canvas.clientWidth / 2;
    const cy = st.canvas.clientHeight / 2;
    st.centerLon = worldXToLon(lonToWorldX(anchor.lon, zoom) - (x - cx), zoom);
    st.centerLat = worldYToLat(clamp(latToWorldY(anchor.lat, zoom) - (y - cy), 0, worldSize(zoom)), zoom);
    draw(st);
  }

  function buildSkeleton(el) {
    el.innerHTML = "";
    el.classList.add("mp-panel", "mp-map-panel");

    const toolbar = document.createElement("div");
    toolbar.className = "mp-toolbar";
    const title = document.createElement("span");
    title.className = "mp-title";
    const count = document.createElement("span");
    count.className = "mp-count";
    toolbar.appendChild(title);
    toolbar.appendChild(count);
    el.appendChild(toolbar);

    const wrap = document.createElement("div");
    wrap.className = "mp-map-canvas-wrap";
    const canvas = document.createElement("canvas");
    wrap.appendChild(canvas);

    const controls = document.createElement("div");
    controls.className = "mp-map-controls";
    const zoomIn = document.createElement("button");
    zoomIn.className = "mp-btn";
    zoomIn.textContent = "+";
    const zoomOut = document.createElement("button");
    zoomOut.className = "mp-btn";
    zoomOut.textContent = "−";
    const reset = document.createElement("button");
    reset.className = "mp-btn";
    reset.textContent = "Fit";
    controls.appendChild(zoomIn);
    controls.appendChild(zoomOut);
    controls.appendChild(reset);
    wrap.appendChild(controls);

    const legend = document.createElement("div");
    legend.className = "mp-map-legend";
    wrap.appendChild(legend);

    // Attribution: required by the OSM tile usage policy whenever the tiles are
    // on screen, so it is created here rather than being anything a control can
    // switch off, and it links to the copyright page as the policy asks. It is
    // hidden until a tile has actually been painted -- an offline graticule
    // owes OpenStreetMap no credit, and claiming one would be a lie about where
    // the map came from.
    const attrib = document.createElement("div");
    attrib.className = "mp-map-attrib";
    attrib.innerHTML = '<a href="https://www.openstreetmap.org/copyright" ' +
      'target="_blank" rel="noopener noreferrer">© OpenStreetMap contributors</a>';
    attrib.style.display = "none";
    wrap.appendChild(attrib);

    // The entire report for "no basemap": one line, no dialog, nothing to
    // acknowledge, and the map keeps working around it.
    const offline = document.createElement("div");
    offline.className = "mp-map-offline";
    offline.textContent = "Basemap unavailable — running offline";
    offline.style.display = "none";
    wrap.appendChild(offline);

    const tooltip = document.createElement("div");
    tooltip.className = "mp-map-tooltip";
    tooltip.style.display = "none";
    wrap.appendChild(tooltip);

    el.appendChild(wrap);

    const st = {
      el,
      titleEl: title,
      countEl: count,
      wrap,
      canvas,
      ctx: canvas.getContext("2d"),
      legend,
      tooltip,
      attribEl: attrib,
      offlineEl: offline,
      centerLat: 0,
      centerLon: 0,
      zoom: DEFAULT_ZOOM,
      hasFitted: false,
      markers: [],
      dragging: false,
      dragMoved: false,
      lastX: 0,
      lastY: 0,
      selectedId: null,
      legendKindsSig: "",
      // Tile layer: cache keyed "z/x/y", the counters behind the give-up rule,
      // and the coalescing handle for "a tile arrived, repaint".
      tiles: new Map(),
      tileLoads: 0,
      tileFailures: 0,
      tilesUnavailable: false,
      tileDrawPending: 0,
    };
    el.__mpMap = st;

    const resize = () => {
      sizeCanvas(st);
      draw(st);
    };
    if (window.ResizeObserver) {
      new ResizeObserver(resize).observe(wrap);
    } else {
      window.addEventListener("resize", resize);
    }

    canvas.addEventListener("pointerdown", (ev) => {
      st.dragging = true;
      st.dragMoved = false;
      st.lastX = ev.clientX;
      st.lastY = ev.clientY;
      canvas.setPointerCapture(ev.pointerId);
      canvas.classList.add("mp-dragging");
    });
    canvas.addEventListener("pointermove", (ev) => {
      if (!st.dragging) return;
      const dx = ev.clientX - st.lastX;
      const dy = ev.clientY - st.lastY;
      if (Math.abs(dx) > 2 || Math.abs(dy) > 2) st.dragMoved = true;
      st.lastX = ev.clientX;
      st.lastY = ev.clientY;
      panBy(st, dx, dy);
      draw(st);
    });
    const endDrag = (ev) => {
      st.dragging = false;
      canvas.classList.remove("mp-dragging");
      if (!st.dragMoved) handleClick(st, ev);
    };
    canvas.addEventListener("pointerup", endDrag);
    canvas.addEventListener("pointercancel", () => { st.dragging = false; canvas.classList.remove("mp-dragging"); });

    canvas.addEventListener("wheel", (ev) => {
      ev.preventDefault();
      const rect = canvas.getBoundingClientRect();
      zoomAt(st, ev.clientX - rect.left, ev.clientY - rect.top, ev.deltaY < 0 ? 1.15 : 1 / 1.15);
    }, { passive: false });

    zoomIn.addEventListener("click", () => zoomAt(st, st.canvas.clientWidth / 2, st.canvas.clientHeight / 2, 1.4));
    zoomOut.addEventListener("click", () => zoomAt(st, st.canvas.clientWidth / 2, st.canvas.clientHeight / 2, 1 / 1.4));
    reset.addEventListener("click", () => { fitToMarkers(st); draw(st); });

    sizeCanvas(st);
    return st;
  }

  function sizeCanvas(st) {
    const dpr = window.devicePixelRatio || 1;
    const rect = st.wrap.getBoundingClientRect();
    const w = Math.max(1, Math.round(rect.width * dpr));
    const h = Math.max(1, Math.round(rect.height * dpr));
    if (st.canvas.width !== w || st.canvas.height !== h) {
      st.canvas.width = w;
      st.canvas.height = h;
    }
  }

  // The span a lone marker is fitted to. Degrees here, converted to world
  // pixels below, so "Fit" on a single contact lands at the same scale the
  // equirectangular version of this panel chose rather than at ZOOM_MAX.
  const MIN_FIT_SPAN_DEG = 0.01;

  function fitToMarkers(st) {
    const markers = st.markers.filter((m) => Number.isFinite(m.lat) && Number.isFinite(m.lon));
    if (markers.length === 0) return;
    let minLat = Infinity, maxLat = -Infinity, minLon = Infinity, maxLon = -Infinity;
    markers.forEach((m) => {
      minLat = Math.min(minLat, m.lat); maxLat = Math.max(maxLat, m.lat);
      minLon = Math.min(minLon, m.lon); maxLon = Math.max(maxLon, m.lon);
    });

    // The fit is computed in world pixels, not degrees. In Mercator a degree of
    // latitude is 1/cos(phi) times as tall as a degree of longitude is wide, so
    // a bounding box fitted in degrees would be both mis-scaled and centred
    // north of the markers it was supposed to frame.
    const x0 = lonToWorldX(minLon, 0);
    const x1 = lonToWorldX(maxLon, 0);
    const y0 = latToWorldY(maxLat, 0); // world y grows southward
    const y1 = latToWorldY(minLat, 0);
    st.centerLon = worldXToLon((x0 + x1) / 2, 0);
    st.centerLat = worldYToLat((y0 + y1) / 2, 0);

    const minSpan = MIN_FIT_SPAN_DEG / 360 * worldSize(0);
    const spanX = Math.max(x1 - x0, minSpan);
    const spanY = Math.max(y1 - y0, minSpan);
    const w = Math.max(100, st.canvas.clientWidth) * 0.8;
    const h = Math.max(100, st.canvas.clientHeight) * 0.8;
    st.zoom = clamp(Math.log2(Math.min(w / spanX, h / spanY)), ZOOM_MIN, ZOOM_MAX);
    st.hasFitted = true;
  }

  // niceStep picks a graticule spacing (degrees) so gridlines land roughly
  // 70-160 CSS px apart at the current zoom.
  const STEP_CANDIDATES = [30, 10, 5, 2, 1, 0.5, 0.2, 0.1, 0.05, 0.02, 0.01, 0.005, 0.002, 0.001, 0.0005];
  function niceStep(pxPerDeg) {
    for (const step of STEP_CANDIDATES) {
      if (step * pxPerDeg >= 70) continue;
      return step;
    }
    return STEP_CANDIDATES[STEP_CANDIDATES.length - 1];
  }

  function decimalsFor(step) {
    if (step >= 1) return 0;
    if (step >= 0.1) return 1;
    if (step >= 0.01) return 2;
    if (step >= 0.001) return 3;
    return 4;
  }

  // ---------------------------------------------------------------------------
  // Basemap tiles
  //
  // Tiles come from the portal's own endpoint, root-relative so the page never
  // names a host, and never straight from the browser to OpenStreetMap: the
  // caching, revalidation and User-Agent the OSM tile usage policy requires all
  // live server-side (internal/portal/tiles). This is byte-for-byte the same
  // endpoint and the same rules panels/adsb.js uses; the server needs no change
  // to serve a second panel from it, and neither renderer may quietly grow its
  // own URL. Two of the policy's obligations land on this side and are not
  // negotiable:
  //
  //   - "© OpenStreetMap contributors" is on screen whenever tiles are, above
  //     every other overlay, and off screen when they are not.
  //   - Requests are strictly DEMAND-DRIVEN: only tiles the current viewport is
  //     about to draw. No neighbour prefetch, no other zoom level, no warm-up,
  //     no "download this area" -- bulk downloading and pre-seeding are
  //     prohibited, and nothing of the sort should be added later.
  //
  // Any non-200 (503 tiles disabled or offline, 400, a block, no network at
  // all) is just "no basemap": a failed tile is remembered as failed so it is
  // never retried in a draw loop, and once several requests have failed with
  // nothing ever having loaded the layer gives up entirely rather than storming
  // a dead endpoint. The panel then looks exactly as it did before tiles
  // existed. Giving up is per-panel-instance and permanent; a reload is what
  // re-tries a portal that has since found a network.
  // ---------------------------------------------------------------------------

  const TILE_URL_BASE = "/api/tiles/";
  // A few hundred tiles is several screenfuls of pan history; past that the
  // oldest ones that are not on screen are dropped.
  const TILE_CACHE_MAX = 320;
  const TILE_GIVEUP_FAILURES = 4;

  function tileKey(z, x, y) { return `${z}/${x}/${y}`; }

  // scheduleTileDraw coalesces "a tile arrived" into one repaint, and a
  // timeout rather than requestAnimationFrame because rAF does not run in a
  // hidden or non-compositing tab while a tile that has landed belongs on the
  // canvas either way.
  function scheduleTileDraw(st) {
    if (st.tileDrawPending) return;
    st.tileDrawPending = window.setTimeout(() => {
      st.tileDrawPending = 0;
      if (st.el.isConnected) draw(st);
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
    if (st.tilesUnavailable) return false;

    // Tile z follows the map zoom in CSS pixels, deliberately ignoring
    // devicePixelRatio: on a 2x display that upscales the imagery slightly
    // rather than fetching four times as many tiles for the same view.
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
      // knocks it back so the markers and their labels stay the foreground.
      ctx.fillStyle = "rgba(5,7,11,0.30)";
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
    st.offlineEl.style.display = st.tilesUnavailable ? "block" : "none";
  }

  // The graticule is still worth having over the tiles at low contrast, and
  // with no basemap it is the only spatial reference there is.
  //
  // In Mercator longitude is linear in x but latitude is NOT linear in y, so
  // the parallels are stepped in DEGREES and projected one at a time; spacing
  // them evenly down the canvas (as the old equirectangular code did) would
  // mislabel every line but the middle one.
  function drawGraticule(st, ctx, w, h, overTiles) {
    const pxPerDegLon = worldSize(st.zoom) / 360;
    const lonStep = niceStep(pxPerDegLon);
    // A degree of latitude is 1/cos(phi) times as TALL as a degree of longitude
    // is wide in Mercator, so the parallels need their own step: choosing both
    // from the longitude figure spaces them 1/cos(phi) too far apart.
    const pxPerDegLat = pxPerDegLon /
      Math.cos(clamp(st.centerLat, -MERC_MAX_LAT, MERC_MAX_LAT) * D2R);
    const latStep = niceStep(pxPerDegLat);

    const nw = unproject(st, 0, 0);
    const se = unproject(st, w, h);
    const minLat = clamp(se.lat, -MERC_MAX_LAT, MERC_MAX_LAT);
    const maxLat = clamp(nw.lat, -MERC_MAX_LAT, MERC_MAX_LAT);

    ctx.save();
    ctx.strokeStyle = overTiles ? "rgba(255,255,255,0.16)" : "rgba(255,255,255,0.08)";
    ctx.fillStyle = overTiles ? "rgba(237,232,219,0.9)" : "rgba(136,150,168,0.85)";
    ctx.font = "10px monospace";
    ctx.lineWidth = 1;

    // Over map imagery a bare label disappears into whatever is behind it, so
    // it gets the same dark halo the marker labels use.
    const label = (text, x, y) => {
      if (overTiles) {
        const fill = ctx.fillStyle;
        ctx.lineWidth = 3;
        ctx.strokeStyle = "rgba(5,7,11,0.85)";
        ctx.strokeText(text, x, y);
        ctx.lineWidth = 1;
        ctx.strokeStyle = "rgba(255,255,255,0.16)";
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
      label(`${Math.abs(dl).toFixed(lonDecimals)}°${dl >= 0 ? "E" : "W"}`, x + 3, 12);
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

  function drawMarker(ctx, x, y, color, heading, selected) {
    ctx.save();
    ctx.translate(x, y);
    if (selected) {
      ctx.beginPath();
      ctx.arc(0, 0, 11, 0, Math.PI * 2);
      ctx.strokeStyle = "#ffffff";
      ctx.lineWidth = 1.5;
      ctx.stroke();
    }
    if (Number.isFinite(heading)) {
      ctx.rotate((heading * Math.PI) / 180);
      ctx.beginPath();
      ctx.moveTo(0, -7);
      ctx.lineTo(5, 6);
      ctx.lineTo(0, 3);
      ctx.lineTo(-5, 6);
      ctx.closePath();
      ctx.fillStyle = color;
      ctx.fill();
    } else {
      ctx.beginPath();
      ctx.arc(0, 0, 4, 0, Math.PI * 2);
      ctx.fillStyle = color;
      ctx.fill();
    }
    ctx.restore();
  }

  function draw(st) {
    const dpr = window.devicePixelRatio || 1;
    const ctx = st.ctx;
    const w = st.canvas.clientWidth;
    const h = st.canvas.clientHeight;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    ctx.fillStyle = "#05070b";
    ctx.fillRect(0, 0, w, h);

    if (w < 2 || h < 2) return;

    // Order is the whole degrade story: the basemap goes down first if any of
    // it has arrived, the graticule and the markers are drawn over whatever
    // that was -- including nothing at all.
    const overTiles = drawTiles(st, ctx, w, h);
    updateBasemapChrome(st, overTiles);
    drawGraticule(st, ctx, w, h, overTiles);

    st.markers.forEach((m) => {
      if (!Number.isFinite(m.lat) || !Number.isFinite(m.lon)) return;
      const { x, y } = project(st, m.lat, m.lon);
      if (x < -20 || x > w + 20 || y < -20 || y > h + 20) return;
      const color = colorFor(m.kind);
      drawMarker(ctx, x, y, color, m.heading, st.selectedId === m.id);
      if (m.label) {
        ctx.font = "11px sans-serif";
        ctx.lineWidth = 3;
        ctx.strokeStyle = "rgba(5,7,11,0.85)";
        ctx.strokeText(m.label, x + 8, y + 4);
        ctx.fillStyle = "#dde4ee";
        ctx.fillText(m.label, x + 8, y + 4);
      }
    });

    updateTooltip(st);
  }

  function handleClick(st, ev) {
    const rect = st.canvas.getBoundingClientRect();
    const x = ev.clientX - rect.left;
    const y = ev.clientY - rect.top;
    let hit = null;
    let bestDist = 14;
    st.markers.forEach((m) => {
      if (!Number.isFinite(m.lat) || !Number.isFinite(m.lon)) return;
      const p = project(st, m.lat, m.lon);
      const d = Math.hypot(p.x - x, p.y - y);
      if (d < bestDist) { bestDist = d; hit = m; }
    });
    st.selectedId = hit ? (st.selectedId === hit.id ? null : hit.id) : null;
    draw(st);
  }

  function updateTooltip(st) {
    const m = st.markers.find((mk) => mk.id === st.selectedId);
    if (!m) {
      st.tooltip.style.display = "none";
      return;
    }
    const p = project(st, m.lat, m.lon);
    st.tooltip.style.left = `${p.x}px`;
    st.tooltip.style.top = `${p.y}px`;
    st.tooltip.style.display = "block";
    const parts = [m.label || m.id, `${m.lat.toFixed(5)}, ${m.lon.toFixed(5)}`];
    if (Number.isFinite(m.heading)) parts.push(`hdg ${Math.round(m.heading)}°`);
    if (m.detail) parts.push(m.detail);
    st.tooltip.textContent = parts.join(" · ");
  }

  function updateLegend(st) {
    const kinds = Array.from(new Set(st.markers.map((m) => m.kind).filter(Boolean)));
    const sig = kinds.sort().join(",");
    if (sig === st.legendKindsSig) return;
    st.legendKindsSig = sig;
    st.legend.innerHTML = "";
    if (kinds.length === 0) return;
    kinds.forEach((kind) => {
      const item = document.createElement("span");
      item.className = "mp-legend-item";
      const dot = document.createElement("span");
      dot.className = "mp-legend-dot";
      dot.style.background = colorFor(kind);
      const label = document.createElement("span");
      label.textContent = kind;
      item.appendChild(dot);
      item.appendChild(label);
      st.legend.appendChild(item);
    });
  }

  // normalizeMarkers reconciles the panel contract's marker shape with the
  // one this renderer draws. It is applied to every marker array that reaches
  // this panel, so a backend cannot arrive with the contract's spelling and
  // silently lose half its data.
  //
  //   - heading_deg -> heading. The contract names the field for its unit
  //     (as the adsb panel does); this file has called it `heading` since
  //     before that contract existed, and read only its own spelling -- so a
  //     marker that said which way it was pointing was drawn as a plain dot.
  //     A marker with no heading in either spelling keeps none: the dot IS
  //     the honest rendering of "we do not know". Never defaulted to 0, which
  //     reads as "due north".
  //   - id. Marker selection (click -> tooltip) is keyed on `id`; the
  //     contract has no such field. Leaving it undefined made every marker
  //     compare equal to every other, so one click highlighted all of them
  //     and the tooltip printed the FIRST marker's name and position -- a
  //     real value belonging to a different entity, which is worse than no
  //     tooltip at all. The label is the entry's own identity in every app
  //     that publishes a position (MMSI, callsign, SSID); index is the
  //     fallback for a marker with no label. This is a rendering key and is
  //     never displayed as data: the tooltip prints `label || id`, and a
  //     marker with no label has nothing else to print anyway.
  //
  // Markers without a usable position are dropped here rather than skipped at
  // every draw site, so the marker count says how many are actually on the
  // map.
  function normalizeMarkers(raw) {
    const out = [];
    (Array.isArray(raw) ? raw : []).forEach((m, i) => {
      if (!m || !Number.isFinite(m.lat) || !Number.isFinite(m.lon)) return;
      const adapted = {
        id: m.id !== undefined ? m.id : (m.label ? "l:" + m.label : "i:" + i),
        lat: m.lat,
        lon: m.lon,
        label: m.label,
        kind: m.kind,
      };
      const heading = Number.isFinite(m.heading) ? m.heading
        : (Number.isFinite(m.heading_deg) ? m.heading_deg : null);
      if (heading !== null) adapted.heading = heading;
      if (m.detail !== undefined) adapted.detail = m.detail;
      out.push(adapted);
    });
    return out;
  }

  function render(el, data) {
    data = data || {};
    const st = el.__mpMap || buildSkeleton(el);

    st.titleEl.textContent = data.app_name || "Map";
    st.markers = normalizeMarkers(data.markers);
    st.countEl.textContent = `${st.markers.length} ${st.markers.length === 1 ? "marker" : "markers"}`;
    updateLegend(st);

    if (!st.hasFitted && st.markers.length > 0) {
      sizeCanvas(st);
      fitToMarkers(st);
    }

    sizeCanvas(st);
    draw(st);
  }

  window.MayhemPanels.registerPanel("map", render);
})();
