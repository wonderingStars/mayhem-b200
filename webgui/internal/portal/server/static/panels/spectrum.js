// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// The "spectrum" panel: a trace + scrolling waterfall driven by exactly the
// SpectrumFrame/idle-frame JSON shape internal/web/spectrum.go already
// produces (see ../../../PANELS.md) -- deliberately the same shape, not a
// new one. The trace/waterfall/colour-ramp drawing below is ported from
// internal/web/static/app.js's drawSpectrumLine/drawWaterfallRow/colormap
// for visual consistency with the existing radio GUI, generalized to run
// per-panel-instance (own canvases, own state) instead of against page-wide
// element ids, and extended with an MHz frequency axis strip.

(() => {
  "use strict";

  const { clamp } = window.MayhemPanels.util;

  // Same perceptual black -> blue -> cyan -> yellow -> red ramp as
  // internal/web/static/app.js, so a waterfall looks the same wherever it's
  // drawn in this product.
  const COLOR_STOPS = [
    [0.0, 6, 8, 14],
    [0.25, 20, 42, 120],
    [0.55, 24, 170, 190],
    [0.8, 230, 200, 40],
    [1.0, 230, 45, 40],
  ];

  function colormap(v) {
    v = clamp(v, 0, 1);
    for (let i = 1; i < COLOR_STOPS.length; i++) {
      const [p0, r0, g0, b0] = COLOR_STOPS[i - 1];
      const [p1, r1, g1, b1] = COLOR_STOPS[i];
      if (v <= p1) {
        const t = p1 === p0 ? 0 : (v - p0) / (p1 - p0);
        return [r0 + (r1 - r0) * t, g0 + (g1 - g0) * t, b0 + (b1 - b0) * t];
      }
    }
    const last = COLOR_STOPS[COLOR_STOPS.length - 1];
    return [last[1], last[2], last[3]];
  }

  const IDLE_MESSAGES = {
    "no device open": "No device open.",
    "rx not streaming": "RX not streaming.",
  };

  function buildSkeleton(el) {
    el.innerHTML = "";
    el.classList.add("mp-panel", "mp-spectrum-panel");

    const toolbar = document.createElement("div");
    toolbar.className = "mp-toolbar";
    const title = document.createElement("span");
    title.className = "mp-title";
    toolbar.appendChild(title);

    const autoLabel = document.createElement("label");
    autoLabel.className = "mp-field-inline";
    const autoBox = document.createElement("input");
    autoBox.type = "checkbox";
    autoBox.checked = true;
    autoLabel.appendChild(autoBox);
    autoLabel.appendChild(document.createTextNode("Auto scale"));
    toolbar.appendChild(autoLabel);

    const floorLabel = document.createElement("label");
    floorLabel.className = "mp-field-inline";
    floorLabel.appendChild(document.createTextNode("Floor"));
    const floorInput = document.createElement("input");
    floorInput.type = "number";
    floorInput.step = "1";
    floorInput.value = "-100";
    floorInput.style.width = "62px";
    floorLabel.appendChild(floorInput);
    floorLabel.appendChild(document.createTextNode("dB"));
    toolbar.appendChild(floorLabel);

    const ceilLabel = document.createElement("label");
    ceilLabel.className = "mp-field-inline";
    ceilLabel.appendChild(document.createTextNode("Ceil"));
    const ceilInput = document.createElement("input");
    ceilInput.type = "number";
    ceilInput.step = "1";
    ceilInput.value = "-10";
    ceilInput.style.width = "62px";
    ceilLabel.appendChild(ceilInput);
    ceilLabel.appendChild(document.createTextNode("dB"));
    toolbar.appendChild(ceilLabel);

    const spanReadout = document.createElement("span");
    spanReadout.className = "mp-field-inline mp-mono";
    toolbar.appendChild(spanReadout);

    el.appendChild(toolbar);

    const wrap = document.createElement("div");
    wrap.className = "mp-spectrum-canvases";
    const traceCanvas = document.createElement("canvas");
    traceCanvas.className = "mp-spectrum-trace";
    const wfCanvas = document.createElement("canvas");
    wfCanvas.className = "mp-spectrum-waterfall";
    const overlay = document.createElement("div");
    overlay.className = "mp-spectrum-overlay mp-hidden";
    const overlaySpan = document.createElement("span");
    overlay.appendChild(overlaySpan);
    wrap.appendChild(traceCanvas);
    wrap.appendChild(wfCanvas);
    wrap.appendChild(overlay);
    el.appendChild(wrap);

    const axisCanvas = document.createElement("canvas");
    axisCanvas.className = "mp-spectrum-axis";
    el.appendChild(axisCanvas);

    const st = {
      traceCanvas,
      traceCtx: traceCanvas.getContext("2d"),
      wfCanvas,
      wfCtx: wfCanvas.getContext("2d"),
      axisCanvas,
      axisCtx: axisCanvas.getContext("2d"),
      overlay,
      overlaySpan,
      autoBox,
      floorInput,
      ceilInput,
      spanReadout,
      titleEl: title,
      lastCenterHz: 0,
      lastSampleRateHz: 0,
    };
    el.__mpSpectrum = st;

    const resize = () => resizeCanvases(st);
    if (window.ResizeObserver) {
      const ro = new ResizeObserver(resize);
      ro.observe(wrap);
      ro.observe(axisCanvas);
    } else {
      window.addEventListener("resize", resize);
    }
    resize();

    return st;
  }

  function resizeCanvases(st) {
    const dpr = window.devicePixelRatio || 1;
    [st.traceCanvas, st.wfCanvas, st.axisCanvas].forEach((c) => {
      const rect = c.getBoundingClientRect();
      const w = Math.max(1, Math.round(rect.width * dpr));
      const h = Math.max(1, Math.round(rect.height * dpr));
      if (c.width !== w || c.height !== h) {
        c.width = w;
        c.height = h;
      }
    });
  }

  function drawTrace(st, bins, floor, ceil) {
    const ctx = st.traceCtx;
    const w = st.traceCanvas.width, h = st.traceCanvas.height;
    if (w < 2 || h < 2) return;
    const dpr = window.devicePixelRatio || 1;
    ctx.clearRect(0, 0, w, h);

    ctx.strokeStyle = "rgba(255,255,255,0.07)";
    ctx.lineWidth = 1;
    ctx.fillStyle = "rgba(136,150,168,0.85)";
    ctx.font = `${10 * dpr}px monospace`;
    const gridLines = 4;
    const range = Math.max(1e-6, ceil - floor);
    for (let i = 0; i <= gridLines; i++) {
      const y = (h * i) / gridLines;
      ctx.beginPath();
      ctx.moveTo(0, y);
      ctx.lineTo(w, y);
      ctx.stroke();
      const db = ceil - (range * i) / gridLines;
      ctx.fillText(`${db.toFixed(0)} dB`, 4 * dpr, y + 11 * dpr);
    }

    const n = bins.length;
    if (n === 0) return;
    ctx.beginPath();
    for (let x = 0; x < w; x++) {
      const idx = Math.min(n - 1, Math.floor((x * n) / w));
      const norm = clamp((bins[idx] - floor) / range, 0, 1);
      const y = h - norm * h;
      if (x === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
    }
    ctx.strokeStyle = "#34d8c3";
    ctx.lineWidth = 1.5 * dpr;
    ctx.stroke();
    ctx.lineTo(w, h);
    ctx.lineTo(0, h);
    ctx.closePath();
    ctx.fillStyle = "rgba(52,216,195,0.12)";
    ctx.fill();
  }

  function drawWaterfallRow(st, bins, floor, ceil) {
    const ctx = st.wfCtx;
    const w = st.wfCanvas.width, h = st.wfCanvas.height;
    if (w < 1 || h < 2) return;
    ctx.drawImage(st.wfCanvas, 0, 0, w, h - 1, 0, 1, w, h - 1);
    const row = ctx.createImageData(w, 1);
    const n = bins.length;
    if (n === 0) return;
    const range = Math.max(1e-6, ceil - floor);
    for (let x = 0; x < w; x++) {
      const idx = Math.min(n - 1, Math.floor((x * n) / w));
      const norm = (bins[idx] - floor) / range;
      const [r, g, b] = colormap(norm);
      const o = x * 4;
      row.data[o] = r; row.data[o + 1] = g; row.data[o + 2] = b; row.data[o + 3] = 255;
    }
    ctx.putImageData(row, 0, 0);
  }

  function drawAxis(st, centerHz, sampleRateHz) {
    const ctx = st.axisCtx;
    const w = st.axisCanvas.width, h = st.axisCanvas.height;
    ctx.clearRect(0, 0, w, h);
    if (w < 2 || h < 2 || !sampleRateHz) return;
    const dpr = window.devicePixelRatio || 1;
    ctx.fillStyle = "rgba(136,150,168,0.85)";
    ctx.font = `${10 * dpr}px monospace`;
    ctx.textBaseline = "top";

    const ticks = 5;
    for (let i = 0; i < ticks; i++) {
      const frac = i / (ticks - 1);
      const hz = centerHz - sampleRateHz / 2 + frac * sampleRateHz;
      const x = frac * w;
      const label = (hz / 1e6).toFixed(3);
      const textWidth = ctx.measureText(label).width;
      let tx = x - textWidth / 2;
      if (i === 0) tx = 2 * dpr;
      if (i === ticks - 1) tx = w - textWidth - 2 * dpr;
      ctx.fillText(label, tx, 2 * dpr);
    }
  }

  function showOverlay(st, show, text) {
    st.overlay.classList.toggle("mp-hidden", !show);
    if (text) st.overlaySpan.textContent = text;
  }

  function render(el, data) {
    data = data || {};
    const st = el.__mpSpectrum || buildSkeleton(el);
    st.titleEl.textContent = data.title || "Spectrum";

    if (data.type === "idle" || !data.bins_db) {
      showOverlay(st, true, IDLE_MESSAGES[data.reason] || data.reason || "Idle");
      return;
    }
    showOverlay(st, false);

    const auto = st.autoBox.checked;
    let floor, ceil;
    if (auto) {
      floor = data.floor_db - 5;
      ceil = data.ceil_db + 5;
      st.floorInput.value = floor.toFixed(0);
      st.ceilInput.value = ceil.toFixed(0);
    } else {
      floor = parseFloat(st.floorInput.value);
      ceil = parseFloat(st.ceilInput.value);
      if (Number.isNaN(floor)) floor = -100;
      if (Number.isNaN(ceil) || ceil <= floor) ceil = floor + 10;
    }

    drawTrace(st, data.bins_db, floor, ceil);
    drawWaterfallRow(st, data.bins_db, floor, ceil);
    drawAxis(st, data.center_hz || 0, data.sample_rate_hz || 0);

    st.lastCenterHz = data.center_hz || 0;
    st.lastSampleRateHz = data.sample_rate_hz || 0;
    st.spanReadout.textContent =
      `center ${(st.lastCenterHz / 1e6).toFixed(3)} MHz · span ${(st.lastSampleRateHz / 1e6).toFixed(3)} MHz`;
  }

  window.MayhemPanels.registerPanel("spectrum", render);
})();
