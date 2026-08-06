// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// Bridges the panels/*.js library (window.MayhemPanels; contract documented
// in ../../../PANELS.md) into app.js's own MayhemPortal.registerPanel(kind,
// render) mechanism, so the portal's real panel-poll loop (app.js's
// pollPanel(), driven by GET /api/panel) actually draws with these
// renderers instead of app.js's minimal built-ins.
//
// Load this file LAST, after app.js and every panels/<kind>.js file: for
// every kind MayhemPanels has registered, this replaces (or adds) app.js's
// registry entry with one that forwards to MayhemPanels.renderPanel. app.js
// ships its own "kv" renderer for simple key/value app state; MayhemPanels
// has no "kv" kind, so that one is untouched. "table" is the one kind both
// sides define -- MayhemPanels' richer, sortable/selectable one (this
// change) wins, per app.js's own doc comment inviting exactly this
// ("call MayhemPortal.registerPanel(...)" from a script loaded after it).
//
// This file is the ONLY piece of integration glue between the two modules;
// nothing else in panels/*.js knows app.js/MayhemPortal exists, and this
// file does nothing (rather than erroring) if either global is missing --
// e.g. when panels/*.js is loaded standalone via harness.html, which has no
// MayhemPortal at all.
(() => {
  "use strict";
  if (!window.MayhemPortal || !window.MayhemPanels) return;
  window.MayhemPanels.kinds().forEach((kind) => {
    window.MayhemPortal.registerPanel(kind, (el, data) => {
      window.MayhemPanels.renderPanel(kind, el, data);
    });
  });
})();
