// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// The "map" panel: markers with lat/lon/label/heading on a pannable/
// zoomable offline canvas map with a lat/lon graticule -- no tile service,
// so it works with no network access at all. Used by ADS-B/APRS today; the
// shape is generic to "things with a position" (see ../../../PANELS.md).
//
// Projection is a simple equirectangular one with a latitude-cosine x
// correction (accurate enough at the local scales these apps operate at;
// this is not a general-purpose mapping library). All drawing happens in
// CSS-pixel space (ctx is reset to a single dpr scale transform per frame),
// so hit-testing against pointer events never has to convert coordinate
// spaces.

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

    const tooltip = document.createElement("div");
    tooltip.className = "mp-map-tooltip";
    tooltip.style.display = "none";
    wrap.appendChild(tooltip);

    el.appendChild(wrap);

    const st = {
      titleEl: title,
      countEl: count,
      wrap,
      canvas,
      ctx: canvas.getContext("2d"),
      legend,
      tooltip,
      centerLat: 0,
      centerLon: 0,
      pxPerDeg: 3000,
      hasFitted: false,
      markers: [],
      dragging: false,
      dragMoved: false,
      lastX: 0,
      lastY: 0,
      selectedId: null,
      legendKindsSig: "",
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
      const cosRef = Math.cos((st.centerLat * Math.PI) / 180) || 1e-6;
      st.centerLon -= dx / (cosRef * st.pxPerDeg);
      st.centerLat += dy / st.pxPerDeg;
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

  // project/unproject work entirely in CSS-pixel space; draw() resets the
  // canvas transform to the device-pixel-ratio scale once per frame so every
  // other drawing/hit-testing call can stay in that one coordinate space.
  function project(st, lat, lon) {
    const cosRef = Math.cos((st.centerLat * Math.PI) / 180) || 1e-6;
    const cx = st.canvas.clientWidth / 2;
    const cy = st.canvas.clientHeight / 2;
    return {
      x: cx + (lon - st.centerLon) * cosRef * st.pxPerDeg,
      y: cy - (lat - st.centerLat) * st.pxPerDeg,
    };
  }

  function unproject(st, x, y) {
    const cosRef = Math.cos((st.centerLat * Math.PI) / 180) || 1e-6;
    const cx = st.canvas.clientWidth / 2;
    const cy = st.canvas.clientHeight / 2;
    return {
      lon: st.centerLon + (x - cx) / (cosRef * st.pxPerDeg),
      lat: st.centerLat - (y - cy) / st.pxPerDeg,
    };
  }

  function zoomAt(st, x, y, factor) {
    const before = unproject(st, x, y);
    st.pxPerDeg = clamp(st.pxPerDeg * factor, 20, 5_000_000);
    const cosRef = Math.cos((st.centerLat * Math.PI) / 180) || 1e-6;
    const cx = st.canvas.clientWidth / 2;
    const cy = st.canvas.clientHeight / 2;
    st.centerLon = before.lon - (x - cx) / (cosRef * st.pxPerDeg);
    st.centerLat = before.lat + (y - cy) / st.pxPerDeg;
    draw(st);
  }

  function fitToMarkers(st) {
    const markers = st.markers.filter((m) => Number.isFinite(m.lat) && Number.isFinite(m.lon));
    if (markers.length === 0) return;
    let minLat = Infinity, maxLat = -Infinity, minLon = Infinity, maxLon = -Infinity;
    markers.forEach((m) => {
      minLat = Math.min(minLat, m.lat); maxLat = Math.max(maxLat, m.lat);
      minLon = Math.min(minLon, m.lon); maxLon = Math.max(maxLon, m.lon);
    });
    st.centerLat = (minLat + maxLat) / 2;
    st.centerLon = (minLon + maxLon) / 2;
    const cosRef = Math.cos((st.centerLat * Math.PI) / 180) || 1e-6;
    const latSpan = Math.max(maxLat - minLat, 0.01);
    const lonSpan = Math.max(maxLon - minLon, 0.01);
    const w = Math.max(100, st.canvas.clientWidth) * 0.8;
    const h = Math.max(100, st.canvas.clientHeight) * 0.8;
    const pxFromWidth = w / (lonSpan * cosRef);
    const pxFromHeight = h / latSpan;
    st.pxPerDeg = clamp(Math.min(pxFromWidth, pxFromHeight), 20, 5_000_000);
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

  function drawGraticule(st, ctx, w, h) {
    const cosRef = Math.cos((st.centerLat * Math.PI) / 180) || 1e-6;
    const step = niceStep(Math.min(st.pxPerDeg, st.pxPerDeg * cosRef) || st.pxPerDeg);
    const decimals = decimalsFor(step);

    const halfLonSpan = (w / 2) / (cosRef * st.pxPerDeg);
    const halfLatSpan = (h / 2) / st.pxPerDeg;
    const lonStart = Math.floor((st.centerLon - halfLonSpan) / step) * step;
    const lonEnd = st.centerLon + halfLonSpan;
    const latStart = Math.floor((st.centerLat - halfLatSpan) / step) * step;
    const latEnd = st.centerLat + halfLatSpan;

    ctx.strokeStyle = "rgba(255,255,255,0.08)";
    ctx.fillStyle = "rgba(136,150,168,0.85)";
    ctx.font = "10px monospace";
    ctx.lineWidth = 1;

    for (let lon = lonStart; lon <= lonEnd; lon += step) {
      const { x } = project(st, st.centerLat, lon);
      ctx.beginPath();
      ctx.moveTo(x, 0);
      ctx.lineTo(x, h);
      ctx.stroke();
      const label = `${Math.abs(lon).toFixed(decimals)}°${lon >= 0 ? "E" : "W"}`;
      ctx.fillText(label, x + 3, 12);
    }
    for (let lat = latStart; lat <= latEnd; lat += step) {
      const { y } = project(st, lat, st.centerLon);
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(w, y);
      ctx.stroke();
      const label = `${Math.abs(lat).toFixed(decimals)}°${lat >= 0 ? "N" : "S"}`;
      ctx.fillText(label, 3, y - 3 < 10 ? y + 12 : y - 3);
    }
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
    drawGraticule(st, ctx, w, h);

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

  function render(el, data) {
    data = data || {};
    const st = el.__mpMap || buildSkeleton(el);

    st.titleEl.textContent = data.app_name || "Map";
    st.markers = data.markers || [];
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
