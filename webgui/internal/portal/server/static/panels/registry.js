// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// The panel renderer registry: window.MayhemPanels. See ../../../PANELS.md for
// the full contract this implements and why it looks like this. In short:
//
//   MayhemPanels.registerPanel(kind, render)   // one file per kind calls this once
//   MayhemPanels.renderPanel(kind, el, data)   // host calls this on every update
//   MayhemPanels.renderAuto(el, panel)         // convenience: panel.panel_kind
//
// Load this file before any panels/<kind>.js file — they call registerPanel()
// at load time and expect window.MayhemPanels to already exist.

(() => {
  "use strict";

  const registry = new Map();

  function registerPanel(kind, render) {
    if (typeof kind !== "string" || !kind) {
      throw new Error("MayhemPanels.registerPanel: kind must be a non-empty string");
    }
    if (typeof render !== "function") {
      throw new Error("MayhemPanels.registerPanel: render must be a function(el, data)");
    }
    registry.set(kind, render);
  }

  // renderPanel dispatches to the renderer registered for `kind`, falling back
  // to the "screen" honest-fallback panel (see screen.js) when `kind` has no
  // renderer of its own. Clears any stashed per-panel state on `el` when `el`
  // is being reused for a different kind than last time, so a panel switch
  // never leaks a previous panel's DOM/state (see PANELS.md).
  function renderPanel(kind, el, data) {
    if (!el) return;

    if (el.dataset.mpKind !== kind) {
      for (const prop of Object.keys(el)) {
        if (prop.startsWith("__mp")) delete el[prop];
      }
      el.dataset.mpKind = kind;
    }

    const render = registry.get(kind);
    if (render) {
      render(el, data);
      return;
    }

    const fallback = registry.get("screen");
    const fallbackData = Object.assign({}, data, {
      app_name: (data && (data.app_name || data.title)) || kind || "This app",
      reason: `no renderer registered for panel kind "${kind}"`,
    });
    if (fallback) {
      fallback(el, fallbackData);
    } else {
      // screen.js itself hasn't loaded (misconfigured host) -- still say
      // something useful rather than leaving el untouched/blank.
      el.textContent = fallbackData.reason;
    }
  }

  // renderAuto is a convenience for callers holding a full Panel envelope
  // ({app_id, panel_kind, title, data}) straight off GET /api/panel: it reads
  // panel_kind itself instead of making the caller extract it.
  function renderAuto(el, panel) {
    const kind = (panel && panel.panel_kind) || "screen";
    const data = (panel && panel.data !== undefined) ? panel.data : panel;
    renderPanel(kind, el, data);
  }

  function kinds() {
    return Array.from(registry.keys());
  }

  // --- shared utilities used by more than one panel file ---------------------

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, (c) => ({
      "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;",
    }[c]));
  }

  // formatAge renders a millisecond duration as a short "Xs" / "Xm Ys" /
  // "Xh Ym" / "Xd Yh" relative-age string, matching the granularity a human
  // scanning a fast-moving decoder table actually wants (never more than two
  // units, never a fractional second).
  function formatAge(deltaMs) {
    if (!Number.isFinite(deltaMs) || deltaMs < 0) deltaMs = 0;
    const s = Math.floor(deltaMs / 1000);
    if (s < 1) return "now";
    if (s < 60) return `${s}s`;
    const m = Math.floor(s / 60);
    if (m < 60) return `${m}m ${s % 60}s`;
    const h = Math.floor(m / 60);
    if (h < 24) return `${h}h ${m % 60}m`;
    const d = Math.floor(h / 24);
    return `${d}d ${h % 24}h`;
  }

  function clamp(v, lo, hi) {
    return Math.min(hi, Math.max(lo, v));
  }

  // --- shared "live age" ticker ----------------------------------------------
  //
  // Several panels (table's Age column) need a text node that keeps counting
  // up even when no new data has arrived. Rather than every panel instance
  // running its own setInterval, everyone shares one 1s tick here; entries
  // whose node has left the document are pruned automatically, which is also
  // the whole of this contract's node-cleanup story (see PANELS.md: no
  // destroy() hook).
  const ageEntries = new Set();
  let ageTimer = null;

  function ensureAgeTimer() {
    if (ageTimer) return;
    ageTimer = setInterval(() => {
      for (const entry of ageEntries) {
        if (!entry.node.isConnected) {
          ageEntries.delete(entry);
          continue;
        }
        entry.node.textContent = formatAge(Date.now() - entry.tsMs);
      }
      if (ageEntries.size === 0) {
        clearInterval(ageTimer);
        ageTimer = null;
      }
    }, 1000);
  }

  // trackAge renders formatAge(now - tsMs) into `node` immediately and keeps
  // it current once a second. Returns a handle so the caller can update the
  // timestamp in place (e.g. a table row that changed again) or stop tracking
  // it explicitly (optional -- disconnection is detected automatically too).
  function trackAge(node, tsMs) {
    const entry = { node, tsMs };
    ageEntries.add(entry);
    node.textContent = formatAge(Date.now() - entry.tsMs);
    ensureAgeTimer();
    return {
      update(newTsMs) {
        entry.tsMs = newTsMs;
        entry.node.textContent = formatAge(Date.now() - entry.tsMs);
      },
      stop() {
        ageEntries.delete(entry);
      },
    };
  }

  window.MayhemPanels = {
    registerPanel,
    renderPanel,
    renderAuto,
    kinds,
    util: { escapeHtml, formatAge, clamp, trackAge },
  };
})();
