// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200. Originally written for sdrlink (MIT) and relicensed
// here by its author/copyright holder into this GPL-2.0-or-later project.
//
// mayhem-b200 web GUI, driving any server that speaks the sdrlink wire
// protocol (see ../../PROTOCOL.md upstream, or webgui/README.md). Vanilla
// JS, no build step, no dependencies — this file is served as-is from the
// embedded asset filesystem and must work on a machine with no internet
// access at all.

(() => {
  "use strict";

  // ---------------------------------------------------------------------
  // small utilities
  // ---------------------------------------------------------------------

  function $(id) {
    return document.getElementById(id);
  }

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
    }[c]));
  }

  function toast(message, isError) {
    const container = $("toast");
    const el = document.createElement("div");
    el.className = "toast-item" + (isError ? " error" : "");
    el.textContent = message;
    container.appendChild(el);
    setTimeout(() => el.remove(), 4500);
  }

  async function api(path, opts) {
    opts = opts || {};
    const init = { method: opts.method || "GET" };
    if (opts.body !== undefined) {
      init.headers = { "Content-Type": "application/json" };
      init.body = JSON.stringify(opts.body);
    }
    const res = await fetch(path, init);
    let data = null;
    try {
      data = await res.json();
    } catch (_) {
      // no/invalid JSON body; fall through with data=null
    }
    if (!res.ok) {
      const msg = (data && data.error) || `${res.status} ${res.statusText}`;
      throw new Error(msg);
    }
    return data;
  }

  function formatUptime(seconds) {
    seconds = Math.max(0, Math.floor(seconds || 0));
    const d = Math.floor(seconds / 86400); seconds %= 86400;
    const h = Math.floor(seconds / 3600); seconds %= 3600;
    const m = Math.floor(seconds / 60);
    const s = seconds % 60;
    if (d > 0) return `${d}d ${h}h ${m}m`;
    if (h > 0) return `${h}h ${m}m ${s}s`;
    if (m > 0) return `${m}m ${s}s`;
    return `${s}s`;
  }

  function formatBytesPerSec(bps) {
    if (!bps || bps <= 0) return "0 B/s";
    const units = ["B/s", "KB/s", "MB/s", "GB/s"];
    let v = bps, i = 0;
    while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
    return `${v.toFixed(v < 10 ? 2 : 1)} ${units[i]}`;
  }

  function formatCount(n) {
    if (n === null || n === undefined) return "–";
    return n.toLocaleString();
  }

  function rangeStr(range, scale, unit) {
    if (!range || range.max <= range.min) return "n/a";
    return `${(range.min / scale).toFixed(3)}–${(range.max / scale).toFixed(3)} ${unit}`;
  }

  // ---------------------------------------------------------------------
  // bound controls
  //
  // Every control is disabled by default and only enabled once the open
  // device's Caps say it is supported (RangeControl.setRange) — per the
  // task's caps-driven-limits requirement. On commit each control shows
  // exactly what the server says the hardware accepted, which can differ
  // from what was typed (quantisation), never the raw request.
  // ---------------------------------------------------------------------

  class RangeControl {
    constructor(name, opts) {
      const root = document.querySelector(`[data-control="${name}"]`);
      this.numberEl = root.querySelector('[data-role="number"]');
      this.sliderEl = root.querySelector('[data-role="slider"]');
      this.actualEl = root.querySelector('[data-role="actual"]');
      this.endpoint = opts.endpoint;
      this.bodyKey = opts.bodyKey;
      this.scale = opts.scale || 1;
      this.decimals = opts.decimals ?? 3;
      this.unit = opts.unit || "";
      this.rawValue = null;
      this._wire();
    }

    _wire() {
      if (this.sliderEl) {
        this.sliderEl.addEventListener("input", () => {
          this.numberEl.value = parseFloat(this.sliderEl.value).toFixed(this.decimals);
        });
        this.sliderEl.addEventListener("change", () => {
          const display = parseFloat(this.sliderEl.value);
          if (Number.isNaN(display)) return;
          this.commit(display * this.scale);
        });
      }
      this.numberEl.addEventListener("change", () => {
        const display = parseFloat(this.numberEl.value);
        if (Number.isNaN(display)) return;
        if (this.sliderEl) this.sliderEl.value = display;
        this.commit(display * this.scale);
      });
    }

    // setRange configures min/max/step from a caps Range, or disables the
    // control entirely if the range is missing/degenerate (unsupported).
    setRange(range) {
      const supported = !!range && range.max > range.min;
      this.numberEl.disabled = !supported;
      if (this.sliderEl) this.sliderEl.disabled = !supported;
      if (!supported) {
        this.actualEl.textContent = "n/a";
        return;
      }
      const dmin = range.min / this.scale;
      const dmax = range.max / this.scale;
      const step = range.step > 0 ? range.step : (range.max - range.min) / 1000;
      this.numberEl.min = dmin;
      this.numberEl.max = dmax;
      this.numberEl.step = Math.max(step / this.scale, Math.pow(10, -this.decimals));
      if (this.sliderEl) {
        this.sliderEl.min = dmin;
        this.sliderEl.max = dmax;
        // A slider dragged pixel-by-pixel over a huge range (e.g. 42MHz..6GHz
        // at a 1Hz hardware step) is unusable at the true step, so give the
        // slider a coarser step; the number input still honours the real one.
        this.sliderEl.step = Math.max(step / this.scale, (dmax - dmin) / 2000);
      }
    }

    // setEnabled is for controls with no caps-reported range (lo_offset):
    // just gate on "is a device open" rather than a specific min/max.
    setEnabled(enabled) {
      this.numberEl.disabled = !enabled;
      if (this.sliderEl) this.sliderEl.disabled = !enabled;
      if (!enabled) this.actualEl.textContent = "n/a";
    }

    setValue(rawValue) {
      this.rawValue = rawValue;
      const display = rawValue / this.scale;
      this.numberEl.value = display.toFixed(this.decimals);
      if (this.sliderEl) this.sliderEl.value = display;
      this.actualEl.textContent =
        display.toLocaleString(undefined, {
          minimumFractionDigits: this.decimals,
          maximumFractionDigits: this.decimals,
        }) + (this.unit ? " " + this.unit : "");
    }

    async commit(rawValue) {
      const body = {};
      body[this.bodyKey] = rawValue;
      try {
        const res = await api(this.endpoint, { method: "POST", body });
        this.setValue(res[this.bodyKey]);
      } catch (err) {
        toast(err.message, true);
        if (this.rawValue !== null) this.setValue(this.rawValue); // revert to last known-good
      }
    }
  }

  class BoolControl {
    constructor(id, endpoint) {
      this.el = $(id);
      this.endpoint = endpoint;
      this.el.addEventListener("change", async () => {
        const desired = this.el.checked;
        try {
          const res = await api(endpoint, { method: "POST", body: { on: desired } });
          this.el.checked = res.on;
        } catch (err) {
          this.el.checked = !desired;
          toast(err.message, true);
        }
      });
    }
    setSupported(supported) { this.el.disabled = !supported; }
    setValue(v) { this.el.checked = !!v; }
  }

  class SelectControl {
    constructor(id, endpoint, actualId) {
      this.el = $(id);
      this.endpoint = endpoint;
      this.actualEl = actualId ? $(actualId) : null;
      this.el.addEventListener("change", async () => {
        try {
          const res = await api(endpoint, { method: "POST", body: { name: this.el.value } });
          this.setValue(res.name);
        } catch (err) {
          toast(err.message, true);
        }
      });
    }
    setOptions(list) {
      list = list || [];
      this.el.innerHTML = "";
      list.forEach((name) => {
        const opt = document.createElement("option");
        opt.value = name;
        opt.textContent = name;
        this.el.appendChild(opt);
      });
      this.el.disabled = list.length === 0;
      if (this.actualEl && list.length === 0) this.actualEl.textContent = "n/a";
    }
    setValue(v) {
      if (v) this.el.value = v;
      if (this.actualEl) this.actualEl.textContent = v || "–";
    }
  }

  const controls = {
    rxFreq: new RangeControl("rxFreq", { endpoint: "/api/rx/freq", bodyKey: "hz", scale: 1e6, decimals: 6, unit: "MHz" }),
    rxRate: new RangeControl("rxRate", { endpoint: "/api/rx/rate", bodyKey: "hz", scale: 1e6, decimals: 3, unit: "MHz" }),
    rxGain: new RangeControl("rxGain", { endpoint: "/api/rx/gain", bodyKey: "db", scale: 1, decimals: 1, unit: "dB" }),
    rxBandwidth: new RangeControl("rxBandwidth", { endpoint: "/api/rx/bandwidth", bodyKey: "hz", scale: 1e6, decimals: 3, unit: "MHz" }),
    loOffset: new RangeControl("loOffset", { endpoint: "/api/lo_offset", bodyKey: "hz", scale: 1e6, decimals: 6, unit: "MHz" }),
    txFreq: new RangeControl("txFreq", { endpoint: "/api/tx/freq", bodyKey: "hz", scale: 1e6, decimals: 6, unit: "MHz" }),
    txGain: new RangeControl("txGain", { endpoint: "/api/tx/gain", bodyKey: "db", scale: 1, decimals: 1, unit: "dB" }),
  };

  const rxAntennaCtl = new SelectControl("rxAntennaSelect", "/api/rx/antenna", "rxAntennaActual");
  const txAntennaCtl = new SelectControl("txAntennaSelect", "/api/tx/antenna", "txAntennaActual");
  const rxAgcCtl = new BoolControl("rxAgc", "/api/rx/agc");
  const rxDcCtl = new BoolControl("rxDcOffset", "/api/rx/dc_offset_auto");
  const rxIqCtl = new BoolControl("rxIqBalance", "/api/rx/iq_balance_auto");

  // ---------------------------------------------------------------------
  // device panel
  // ---------------------------------------------------------------------

  let devices = [];
  let deviceIsOpen = false;

  function renderDeviceOptions() {
    const sel = $("deviceSelect");
    sel.innerHTML = "";
    $("deviceEmpty").classList.toggle("hidden", devices.length > 0);
    devices.forEach((d, i) => {
      const opt = document.createElement("option");
      opt.value = String(i);
      opt.textContent = `${d.label || d.driver} (${d.serial || "no serial"})`;
      sel.appendChild(opt);
    });
    $("btnOpen").disabled = deviceIsOpen || devices.length === 0;
  }

  async function scanDevices() {
    try {
      const res = await api("/api/devices");
      devices = res.devices || [];
      renderDeviceOptions();
      if (devices.length === 0) toast("No devices found.");
    } catch (err) {
      toast(err.message, true);
    }
  }

  function applyCaps(caps) {
    $("capsBlock").classList.remove("hidden");

    const setBadge = (id, val) => {
      const el = $(id);
      el.classList.toggle("yes", !!val);
      el.classList.toggle("no", !val);
    };
    setBadge("badgeRx", caps.has_rx);
    setBadge("badgeTx", caps.has_tx);
    setBadge("badgeDuplex", caps.full_duplex);

    const grid = $("capsGrid");
    grid.innerHTML = "";
    const rows = [
      ["Driver", caps.driver || "–"],
      ["Label", caps.label || "–"],
      ["Serial", caps.serial || "–"],
      ["RX freq", rangeStr(caps.rx_freq, 1e6, "MHz")],
      ["RX gain", rangeStr(caps.rx_gain, 1, "dB")],
      ["RX rate", rangeStr(caps.rx_rate, 1e6, "MHz")],
      ["RX bandwidth", rangeStr(caps.rx_bandwidth, 1e6, "MHz")],
      ["TX freq", rangeStr(caps.tx_freq, 1e6, "MHz")],
      ["TX gain", rangeStr(caps.tx_gain, 1, "dB")],
      ["Master clock", caps.master_clock_rate ? (caps.master_clock_rate / 1e6).toFixed(3) + " MHz" : "–"],
    ];
    rows.forEach(([k, v]) => {
      const kEl = document.createElement("div"); kEl.className = "k"; kEl.textContent = k;
      const vEl = document.createElement("div"); vEl.className = "v"; vEl.textContent = v;
      grid.appendChild(kEl); grid.appendChild(vEl);
    });

    controls.rxFreq.setRange(caps.rx_freq);
    controls.rxRate.setRange(caps.rx_rate);
    controls.rxGain.setRange(caps.rx_gain);
    controls.rxBandwidth.setRange(caps.rx_bandwidth);
    controls.loOffset.setEnabled(true);
    rxAntennaCtl.setOptions(caps.rx_antennas);
    rxAgcCtl.setSupported(true);
    rxDcCtl.setSupported(true);
    rxIqCtl.setSupported(true);

    $("txUnsupportedNote").classList.toggle("hidden", !!caps.has_tx);
    controls.txFreq.setRange(caps.has_tx ? caps.tx_freq : null);
    controls.txGain.setRange(caps.has_tx ? caps.tx_gain : null);
    txAntennaCtl.setOptions(caps.has_tx ? caps.tx_antennas : []);
  }

  function clearCaps() {
    $("capsBlock").classList.add("hidden");
    Object.values(controls).forEach((c) => c.setRange(null));
    rxAntennaCtl.setOptions([]);
    txAntennaCtl.setOptions([]);
    rxAgcCtl.setSupported(false);
    rxDcCtl.setSupported(false);
    rxIqCtl.setSupported(false);
  }

  async function refreshState() {
    const st = await api("/api/state");
    controls.rxFreq.setValue(st.rx_freq_hz);
    controls.rxRate.setValue(st.rx_rate_hz);
    controls.rxGain.setValue(st.rx_gain_db);
    controls.rxBandwidth.setValue(st.rx_bandwidth_hz);
    controls.loOffset.setValue(st.lo_offset_hz);
    controls.txFreq.setValue(st.tx_freq_hz);
    controls.txGain.setValue(st.tx_gain_db);
    rxAntennaCtl.setValue(st.rx_antenna);
    txAntennaCtl.setValue(st.tx_antenna);
    rxAgcCtl.setValue(st.rx_agc);
    rxDcCtl.setValue(st.rx_dc_offset_auto);
    rxIqCtl.setValue(st.rx_iq_balance_auto);
    $("formatSelect").value = st.format || "cf32";
    setStreaming(st.streaming);
  }

  function setDeviceOpen(open) {
    deviceIsOpen = open;
    $("btnOpen").disabled = open || devices.length === 0;
    $("btnClose").disabled = !open;
    $("deviceSelect").disabled = open;
    $("deviceDot").classList.toggle("ok", open);
    $("deviceDot").classList.toggle("off", !open);
    $("btnStartRx").disabled = !open;
    if (!open) {
      $("btnStopRx").disabled = true;
      $("formatSelect").disabled = false;
      clearCaps();
      showOverlay(true, "No device open");
    }
  }

  function setStreaming(streaming) {
    $("btnStartRx").disabled = !deviceIsOpen || streaming;
    $("btnStopRx").disabled = !deviceIsOpen || !streaming;
    $("formatSelect").disabled = streaming;
  }

  // syncDeviceState checks whether a device is already open on the server
  // (e.g. this page was reloaded, or another browser tab opened it) and, if
  // so, brings this page's controls up to date instead of showing them as
  // closed/n-a until the user happens to press Open.
  async function syncDeviceState() {
    try {
      const capsRes = await api("/api/caps");
      applyCaps(capsRes.caps);
      setDeviceOpen(true);
      await refreshState();
    } catch (_) {
      // No device open (409) -- the closed/idle initial state is correct.
    }
  }

  $("btnScan").addEventListener("click", scanDevices);

  $("btnOpen").addEventListener("click", async () => {
    const idx = parseInt($("deviceSelect").value, 10);
    const dev = devices[idx];
    if (!dev) return;
    try {
      const res = await api("/api/devices/open", { method: "POST", body: { args: dev.args } });
      applyCaps(res.caps);
      setDeviceOpen(true);
      await refreshState();
      toast(`Opened ${dev.label || dev.driver}`);
    } catch (err) {
      toast(err.message, true);
    }
  });

  $("btnClose").addEventListener("click", async () => {
    try {
      await api("/api/devices/close", { method: "POST" });
      setDeviceOpen(false);
      toast("Device closed");
    } catch (err) {
      toast(err.message, true);
    }
  });

  $("btnStartRx").addEventListener("click", async () => {
    try {
      const format = $("formatSelect").value;
      const res = await api("/api/rx/start", { method: "POST", body: { format } });
      setStreaming(true);
      toast(`RX streaming started (${res.format})`);
    } catch (err) {
      toast(err.message, true);
    }
  });

  $("btnStopRx").addEventListener("click", async () => {
    try {
      await api("/api/rx/stop", { method: "POST" });
      setStreaming(false);
      toast("RX streaming stopped");
    } catch (err) {
      toast(err.message, true);
    }
  });

  // ---------------------------------------------------------------------
  // status panel — polled independently of the spectrum socket (see
  // spectrum.go doc comment for why the two are kept apart)
  // ---------------------------------------------------------------------

  function updateStatusUI(st) {
    $("uptimeValue").textContent = formatUptime(st.uptime_seconds);
    $("sessionsValue").textContent = st.sessions.length;
    $("statUptime").textContent = formatUptime(st.uptime_seconds);
    $("statSessions").textContent = st.sessions.length;
    $("deviceDot").classList.toggle("ok", st.device_open);
    $("deviceDot").classList.toggle("off", !st.device_open);

    const list = $("sessionList");
    if (!st.sessions.length) {
      list.innerHTML = '<p class="empty-note">No control sessions connected.</p>';
    } else {
      list.innerHTML = "";
      st.sessions.forEach((s) => {
        const row = document.createElement("div");
        row.className = "session-row";
        const state = s.streaming ? "streaming" : s.device_open ? "open" : "idle";
        row.innerHTML = `<span class="client">${escapeHtml(s.client || "unknown")}</span><span>${state}</span>`;
        list.appendChild(row);
      });
    }

    if (st.stats) {
      $("statThroughput").textContent = formatBytesPerSec(st.stats.bytes_per_second);
      $("statRxSamples").textContent = formatCount(st.stats.rx_samples);
      $("statOverflows").textContent = formatCount(st.stats.overflows);
      $("statOverflows").classList.toggle("warn", st.stats.overflows > 0);
      $("statDropped").textContent = formatCount(st.stats.dropped_frames);
      $("statDropped").classList.toggle("warn", st.stats.dropped_frames > 0);
    } else {
      ["statThroughput", "statRxSamples", "statOverflows", "statDropped"].forEach((id) => {
        $(id).textContent = "–";
      });
    }
  }

  async function pollStatus() {
    try {
      const st = await api("/api/status");
      updateStatusUI(st);
    } catch (_) {
      // status should basically never fail; ignore transient errors
    }
  }

  // ---------------------------------------------------------------------
  // spectrum + waterfall
  // ---------------------------------------------------------------------

  const specCanvas = $("spectrumCanvas");
  const specCtx = specCanvas.getContext("2d");
  const wfCanvas = $("waterfallCanvas");
  const wfCtx = wfCanvas.getContext("2d");

  function resizeCanvases() {
    const dpr = window.devicePixelRatio || 1;
    [specCanvas, wfCanvas].forEach((c) => {
      const rect = c.getBoundingClientRect();
      const w = Math.max(1, Math.round(rect.width * dpr));
      const h = Math.max(1, Math.round(rect.height * dpr));
      if (c.width !== w || c.height !== h) {
        c.width = w;
        c.height = h;
      }
    });
  }
  window.addEventListener("resize", resizeCanvases);
  resizeCanvases();

  // Perceptually simple black -> blue -> cyan -> yellow -> red colour ramp.
  const COLOR_STOPS = [
    [0.00, 6, 8, 14],
    [0.25, 20, 42, 120],
    [0.55, 24, 170, 190],
    [0.8, 230, 200, 40],
    [1.00, 230, 45, 40],
  ];
  function colormap(v) {
    v = v < 0 ? 0 : v > 1 ? 1 : v;
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

  function drawSpectrumLine(bins, floor, ceil) {
    const w = specCanvas.width, h = specCanvas.height;
    if (w < 2 || h < 2) return;
    const dpr = window.devicePixelRatio || 1;
    specCtx.clearRect(0, 0, w, h);

    specCtx.strokeStyle = "rgba(255,255,255,0.07)";
    specCtx.lineWidth = 1;
    specCtx.fillStyle = "rgba(136,150,168,0.85)";
    specCtx.font = `${10 * dpr}px monospace`;
    const gridLines = 4;
    const range = Math.max(1e-6, ceil - floor);
    for (let i = 0; i <= gridLines; i++) {
      const y = (h * i) / gridLines;
      specCtx.beginPath();
      specCtx.moveTo(0, y);
      specCtx.lineTo(w, y);
      specCtx.stroke();
      const db = ceil - (range * i) / gridLines;
      specCtx.fillText(`${db.toFixed(0)} dB`, 4 * dpr, y + 11 * dpr);
    }

    const n = bins.length;
    if (n === 0) return;
    specCtx.beginPath();
    for (let x = 0; x < w; x++) {
      const idx = Math.min(n - 1, Math.floor((x * n) / w));
      const norm = Math.min(1, Math.max(0, (bins[idx] - floor) / range));
      const y = h - norm * h;
      if (x === 0) specCtx.moveTo(x, y); else specCtx.lineTo(x, y);
    }
    specCtx.strokeStyle = "#34d8c3";
    specCtx.lineWidth = 1.5 * dpr;
    specCtx.stroke();
    specCtx.lineTo(w, h);
    specCtx.lineTo(0, h);
    specCtx.closePath();
    specCtx.fillStyle = "rgba(52,216,195,0.12)";
    specCtx.fill();
  }

  function drawWaterfallRow(bins, floor, ceil) {
    const w = wfCanvas.width, h = wfCanvas.height;
    if (w < 1 || h < 2) return;
    wfCtx.drawImage(wfCanvas, 0, 0, w, h - 1, 0, 1, w, h - 1);
    const row = wfCtx.createImageData(w, 1);
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
    wfCtx.putImageData(row, 0, 0);
  }

  function showOverlay(show, text) {
    $("canvasOverlay").classList.toggle("hidden", !show);
    if (text) $("canvasOverlayText").textContent = text;
  }

  const IDLE_MESSAGES = {
    "no device open": "No device open — select and open a device.",
    "rx not streaming": "RX not streaming — press Start RX.",
  };

  function handleSpectrumFrame(msg) {
    showOverlay(false);
    const auto = $("autoScale").checked;
    let floor, ceil;
    if (auto) {
      floor = msg.floor_db - 5;
      ceil = msg.ceil_db + 5;
      $("floorDb").value = floor.toFixed(0);
      $("ceilDb").value = ceil.toFixed(0);
    } else {
      floor = parseFloat($("floorDb").value);
      ceil = parseFloat($("ceilDb").value);
      if (Number.isNaN(floor)) floor = -100;
      if (Number.isNaN(ceil) || ceil <= floor) ceil = floor + 10;
    }
    drawSpectrumLine(msg.bins_db, floor, ceil);
    drawWaterfallRow(msg.bins_db, floor, ceil);
    $("centerReadout").textContent = `center: ${(msg.center_hz / 1e6).toFixed(3)} MHz`;
    $("spanReadout").textContent = `span: ${(msg.sample_rate_hz / 1e6).toFixed(3)} MHz`;
  }

  function handleIdleFrame(msg) {
    showOverlay(true, IDLE_MESSAGES[msg.reason] || msg.reason || "Idle");
  }

  let ws = null;
  let wsReconnectTimer = null;

  function setWsStatus(connected) {
    $("wsDot").classList.toggle("ok", connected);
    $("wsDot").classList.toggle("off", !connected);
  }

  function connectWS() {
    const proto = location.protocol === "https:" ? "wss" : "ws";
    ws = new WebSocket(`${proto}://${location.host}/ws/spectrum`);
    ws.addEventListener("open", () => setWsStatus(true));
    ws.addEventListener("close", () => {
      setWsStatus(false);
      clearTimeout(wsReconnectTimer);
      wsReconnectTimer = setTimeout(connectWS, 1500);
    });
    ws.addEventListener("error", () => ws.close());
    ws.addEventListener("message", (ev) => {
      let msg;
      try {
        msg = JSON.parse(ev.data);
      } catch (_) {
        return;
      }
      if (msg.type === "spectrum") handleSpectrumFrame(msg);
      else if (msg.type === "idle") handleIdleFrame(msg);
    });
  }

  // ---------------------------------------------------------------------
  // boot
  // ---------------------------------------------------------------------

  setDeviceOpen(false);
  setStreaming(false);
  scanDevices();
  syncDeviceState();
  pollStatus();
  setInterval(pollStatus, 2000);
  connectWS();
})();
