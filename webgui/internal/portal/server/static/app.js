/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Part of mayhem-b200. */

/*
 * mayhem-b200 app portal — browser front end.
 *
 * Talks to this server's own JSON proxy (server.go), which in turn talks to
 * mayhem-b200's C++ app-portal API. No build step, no framework: this file
 * is loaded as a plain <script>, and its only public surface is the
 * `MayhemPortal` global documented below.
 *
 * ------------------------------------------------------------------------
 * PANEL RENDERER CONTRACT
 * ------------------------------------------------------------------------
 * When an app is launched, this page polls GET /api/panel roughly every
 * 700ms and expects back:
 *
 *   { "app_id": "adsbrx", "panel_kind": "table", "title": "...", "data": <any> }
 *
 * `panel_kind` selects which renderer draws `data`. Register one with:
 *
 *   MayhemPortal.registerPanel(kind, function render(el, data) {
 *     // el is the mount <div> (already in the document, already sized).
 *     // Redraw el's contents from `data`. Called again on every poll with
 *     // the latest `data` — do your own diffing if you need to avoid
 *     // flicker; a full innerHTML rebuild is fine for anything table-sized.
 *   });
 *
 * Two renderers ship built in, matching shapes that are common to many
 * decoder apps:
 *
 *   "kv"    data = { items: [ { label: "Frequency", value: "433.920 MHz" }, ... ] }
 *           A grid of read-only key/value tiles. Good fit for a single
 *           app's live state (receiver mode/frequency/level, and the like).
 *
 *   "table" data = { columns: ["Time","ICAO","Callsign"],
 *                     rows: [ ["12:00:01","A12345","UAL123"], ... ] }
 *           A scrolling table. Good fit for anything backed by
 *           ui::RecentEntriesTable (ui_recent_entries.hpp) — which is most
 *           of the decoder apps.
 *
 * `panel_kind` missing/empty means the active app hasn't published a panel
 * at all; an unrecognized `panel_kind` means this page doesn't know how to
 * draw it yet. Both get an honest, distinct empty state — never a blank
 * screen and never fabricated data.
 *
 * To add a new kind from outside this file, define it before app.js's
 * DOMContentLoaded handler runs (a <script> after this one, or edit this
 * file directly) and call MayhemPortal.registerPanel(...).
 *
 * ------------------------------------------------------------------------
 * THE LIVE DEVICE SCREEN
 * ------------------------------------------------------------------------
 * `panel_kind` "screen" no longer means "this app has no web view". It now
 * mounts the real 240x320 framebuffer, streamed over a WebSocket, with
 * keyboard/wheel/pointer input routed back to the device — see
 * screenview.js, which owns that view and the wire contract behind it. Every
 * app works there, because what you are looking at IS the app.
 *
 * For an app that DOES publish structured data, the panel topbar grows a
 * Data/Screen toggle: the structured view is the default (it is the reason
 * this portal exists — searchable, full-resolution, not 240 pixels wide),
 * and the live screen is one click away for anything the structured view
 * cannot reach, such as the app's own menus and settings.
 *
 * ------------------------------------------------------------------------
 * PANEL POLL QUERY: data-mp-have-image-rev
 * ------------------------------------------------------------------------
 * A renderer that caches something large across polls can tell the backend
 * what it already holds by setting `data-mp-have-image-rev` on its mount
 * element; this file forwards it as GET /api/panel?have_image_rev=N and the
 * portal server passes it straight through to mayhem-b200 (contract 4, the
 * image panel). It is cleared automatically whenever panel_kind changes, so
 * a revision from a previous app can never suppress a new app's first
 * image. Nothing here ever synthesizes a value: only the browser knows what
 * the browser has, and guessing would make the backend omit an image that
 * was never delivered.
 *
 * ------------------------------------------------------------------------
 * THE APP GRID
 * ------------------------------------------------------------------------
 * Every section, chip, count and badge below is computed from GET /api/apps
 * and GET /api/apps/current. There is no app list, no category list and no
 * "these apps have panels" list in this file, on purpose: the registry is
 * the C++ side's to change, and a copy here would be wrong the first time it
 * does. What the grid claims must be checkable against those two responses.
 *
 * Badges, and where each one's truth comes from:
 *
 *   TX / TX LOCKED  the app's own `category` (Transmit, Transceiver).
 *   HW              the app's own `hardware_limited`.
 *   RUN             GET /api/apps/current's `id`, polled while the grid is
 *                   on screen so navigating on the device itself shows here.
 *   NATIVE          nativePanelKindFor() -- see the long comment on it. The
 *                   API does not carry this per app yet, so today it is
 *                   unknown and the badge is not drawn at all.
 *
 * The receive-only lockout is deviceCanTransmit(), also unknown today; same
 * rule -- a state that cannot be verified is not asserted. Each of those two
 * functions is the single place to change when the API grows the field it
 * names.
 *
 * Icons come from an optional separate module (see appIconNode); any app id
 * it does not know gets a neutral glyph, which is the expected case for most
 * of the 94.
 *
 * ------------------------------------------------------------------------
 * WHY THIS FILE CARRIES CSS
 * ------------------------------------------------------------------------
 * The grid's markup is built here, not in index.html, so the elements this
 * file invents (badges, the icon slot, the run indicator, the section index)
 * have no styling anywhere else. GRID_CSS below fills exactly that gap. It
 * is wrapped in `@layer mp-grid`, which makes every rule in it lose to
 * app.css -- an unlayered declaration always beats a layered one, whatever
 * its specificity -- so app.css remains the owner of the shell's look and
 * this is only a floor under the parts it has no reason to know about.
 */
(function () {
  'use strict';

  // ---- tunables -----------------------------------------------------------

  var STATUS_POLL_MS = 2000;
  var PANEL_POLL_MS = 700;
  var BACKEND_RETRY_MS = 3000;
  var TOAST_MS = 4500;

  // ---- grid styling -------------------------------------------------------

  // See "WHY THIS FILE CARRIES CSS" in the header comment. Colours are the
  // portal's amber-phosphor palette. Every text pair below was measured in
  // the browser against the background actually painted behind it, not
  // eyeballed and not assumed from the token name:
  //
  //   grid title      #ede8db on #0d0c0a  16.0:1
  //   meta line       #a09681 on #0d0c0a   6.7:1
  //   section index   #ff9438 on #0d0c0a   8.9:1
  //   TX / HW badge   #ff9438 on #14120e   8.5:1
  //   NATIVE badge    #3ddc84 on #14120e  10.5:1
  //   RUN             #3df58c on #14120e  13.0:1
  //   clear search    #3df58c on #0d0c0a  13.6:1
  //   fallback glyph  #3ddc84 at .75       6.3:1
  //   locked tile     name 8.3:1, TX LOCKED 4.9:1 (through opacity .72)
  //
  // #6a6252, which the design uses for small print, measures 3.2:1 on
  // #0d0c0a — so it is used here for the section hairline and nothing else,
  // never for text. The design's own badge colours (#b06a2a TX, #1f8c5f
  // NATIVE) measure 4.4:1 at 8px, which is a fail at that size; they are
  // replaced by the two brighter tones above at 9px.
  var GRID_CSS = [
    '@layer mp-grid {',
    ':root{',
    '--mpg-tile:#14120e;--mpg-rule:#211e17;',
    '--mpg-text:#ede8db;--mpg-dim:#a09681;--mpg-accent:#3df58c;',
    '--mpg-accent-2:#3ddc84;--mpg-warn:#ff9438;',
    "--mpg-mono:'Fragment Mono',ui-monospace,'Cascadia Mono',Consolas,monospace;",
    "--mpg-sans:'Archivo',system-ui,-apple-system,'Segoe UI',sans-serif;",
    // The icon set's two knobs, from the design: a speed multiplier and a
    // play state. Declared here so an icon module can rely on them existing
    // even before any motion control is wired up.
    '--ics:1;--icp:running;}',
    // The design's entry animation is `mpIn`, which fades from opacity 0.
    // This one deliberately drops the opacity half and keeps only the slide.
    // An animation's clock is the rendering timeline: in a tab the browser
    // is not painting it stays frozen on its first keyframe, and measuring
    // that here found all 94 tiles sitting at opacity 0 with zero height —
    // an invisible grid, waiting for a frame that may not come for hours on
    // a second monitor. Frozen on this one, a section is 8px low and fully
    // readable. mpIn itself is left to app.css, which owns the library.
    '@keyframes mpGridIn{from{transform:translateY(8px)}to{transform:none}}',
    // A second definition of mpLed, and app.css has one too. This copy is the
    // FLOOR, not the winner: @keyframes participate in the cascade layers, and
    // an unlayered definition beats a layered one whatever the document order,
    // so app.css's copy wins whenever that sheet is loaded. Measured in the
    // live portal — injecting a third mpLed inside @layer mp-grid moved
    // nothing, injecting one unlayered moved the resolved opacity immediately.
    // Which means app.css:@keyframes mpLed is the one to edit; this exists
    // only so .app-run-dot still blinks on a page that has no app.css.
    '@keyframes mpLed{0%,100%{opacity:.4;box-shadow:0 0 3px 0 currentColor}',
    '50%{opacity:1;box-shadow:0 0 10px 2px currentColor}}',

    '.grid-heading{display:flex;align-items:baseline;gap:14px;flex-wrap:wrap}',
    '.grid-heading-title{margin:0;font-family:var(--mpg-sans);font-size:20px;font-weight:800;',
    'letter-spacing:-.3px;color:var(--mpg-text)}',
    '.grid-heading-meta{font-family:var(--mpg-mono);font-size:11.5px;color:var(--mpg-dim)}',

    '.chip{text-transform:uppercase;letter-spacing:.06em}',

    // No fill-mode either: `both` would back-fill the 0% keyframe before the
    // animation even starts, which is the same trap one step earlier.
    '.category-section{animation:mpGridIn .35s ease}',
    '.category-index{font-family:var(--mpg-mono);font-size:11px;color:var(--mpg-warn);flex:none}',
    '.category-rule{flex:1;height:1px;background:var(--mpg-rule)}',

    // Everything app.css already owns (the tile shell, the grid template, the
    // chip and heading chrome, the focus ring) is deliberately not restated
    // here: those declarations would be inert anyway, and a dead rule reads
    // like a live one.
    '.app-card:hover,.app-card:focus-visible{--ics:.45}',

    '.app-icon{width:30px;height:30px;flex:none;display:flex;align-items:center;',
    'justify-content:center;color:var(--mpg-accent-2)}',
    '.app-icon svg{width:100%;height:100%;display:block}',
    // .75, not the .5 this started at: at .5 the letter measured 3.4:1 on the
    // tile, and while it is aria-hidden and repeats the name below it, a
    // glyph nobody can read is not much of a fallback.
    '.app-icon-fallback{width:26px;height:26px;display:flex;align-items:center;',
    'justify-content:center;border:1px solid currentColor;border-radius:5px;opacity:.75;',
    'font-family:var(--mpg-mono);font-size:12px;line-height:1}',

    '.app-badge{position:absolute;display:inline-flex;align-items:center;gap:4px;',
    'font-family:var(--mpg-mono);font-size:9px;letter-spacing:.1em;line-height:1;',
    'padding:2px 4px;border-radius:3px;border:1px solid currentColor;background:var(--mpg-tile)}',
    '.app-badge-native{top:6px;right:6px;color:var(--mpg-accent-2)}',
    '.app-badge-tx{top:6px;left:6px;color:var(--mpg-warn)}',
    '.app-badge-hw{bottom:6px;left:6px;color:var(--mpg-warn)}',
    '.app-run{position:absolute;bottom:6px;right:6px;display:none;align-items:center;gap:4px;',
    'font-family:var(--mpg-mono);font-size:9px;letter-spacing:.1em;color:var(--mpg-accent)}',
    '.app-run-dot{width:5px;height:5px;border-radius:50%;background:currentColor;',
    'color:inherit;animation:mpLed 1.6s ease-in-out infinite;animation-play-state:var(--icp)}',
    '.app-card.is-running .app-run{display:inline-flex}',

    // Locked tiles stay readable on purpose: at .72 the name is still ~8:1,
    // where the design's .38 would put it near 3:1. The state is carried by
    // the TX LOCKED badge, not by making the tile hard to read.
    '.app-card.is-locked{opacity:.72;cursor:not-allowed}',
    '.app-card.is-locked .app-icon{opacity:.5}',

    '.grid-empty-hint{margin-top:14px;font-family:var(--mpg-mono);font-size:11px;',
    'letter-spacing:.08em;text-transform:uppercase;color:var(--mpg-accent);background:none;',
    'border:1px solid rgba(61,245,140,.4);border-radius:5px;padding:8px 14px;cursor:pointer}',
    '.grid-empty-hint:hover{border-color:var(--mpg-warn);color:var(--mpg-warn)}',

    // The animation library is decoration; the state it decorates is not.
    // Under reduced motion the LED stops fully lit rather than mid-blink and
    // the section entry is skipped, so every badge still reads.
    //
    // app.css owns the portal-wide motion switch (--mp-icon-speed /
    // --mp-icon-play, a data-motion attribute, and a `*{animation:none}`
    // under this same query) and its values win over anything set here, as
    // they should. These two rules are the floor for the elements this file
    // invents, for the case where that sheet is not the one loaded.
    '@media (prefers-reduced-motion: reduce){',
    ':root{--ics:2.6;--icp:paused}',
    '.category-section{animation:none}',
    '.app-run-dot{animation:none;opacity:1}}',
    '}',
  ].join('');

  function installGridStyles() {
    if (document.getElementById('mpGridStyles')) return;
    var style = document.createElement('style');
    style.id = 'mpGridStyles';
    style.textContent = GRID_CSS;
    (document.head || document.documentElement).appendChild(style);
  }

  installGridStyles();

  // ---- panel registry -------------------------------------------------------

  var panelRegistry = Object.create(null);

  function registerPanel(kind, renderer) {
    if (typeof kind !== 'string' || !kind) {
      throw new Error('registerPanel: kind must be a non-empty string');
    }
    if (typeof renderer !== 'function') {
      throw new Error('registerPanel: renderer must be a function(el, data)');
    }
    panelRegistry[kind] = renderer;
  }

  // -- built-in renderer: "kv" --
  registerPanel('kv', function renderKv(el, data) {
    var items = (data && Array.isArray(data.items)) ? data.items : [];
    if (items.length === 0) {
      el.innerHTML = '';
      el.appendChild(emptyPanelNode('No data yet', 'This app has not reported any values yet.'));
      return;
    }
    var grid = document.createElement('div');
    grid.className = 'kv-grid';
    items.forEach(function (item) {
      var cell = document.createElement('div');
      cell.className = 'kv-cell';
      var label = document.createElement('div');
      label.className = 'kv-label';
      label.textContent = String(item && item.label != null ? item.label : '');
      var value = document.createElement('div');
      value.className = 'kv-value';
      value.textContent = String(item && item.value != null ? item.value : '—');
      cell.appendChild(label);
      cell.appendChild(value);
      grid.appendChild(cell);
    });
    el.innerHTML = '';
    el.appendChild(grid);
  });

  // -- built-in renderer: "table" --
  registerPanel('table', function renderTable(el, data) {
    var columns = (data && Array.isArray(data.columns)) ? data.columns : [];
    var rows = (data && Array.isArray(data.rows)) ? data.rows : [];

    var wrap = document.createElement('div');
    wrap.className = 'panel-table-wrap';

    if (rows.length === 0) {
      var empty = document.createElement('div');
      empty.className = 'panel-table-empty';
      empty.textContent = 'No entries yet.';
      wrap.appendChild(empty);
      el.innerHTML = '';
      el.appendChild(wrap);
      return;
    }

    var scroll = document.createElement('div');
    scroll.className = 'panel-table-scroll';
    var table = document.createElement('table');
    table.className = 'panel-table';

    if (columns.length > 0) {
      var thead = document.createElement('thead');
      var htr = document.createElement('tr');
      columns.forEach(function (c) {
        var th = document.createElement('th');
        th.textContent = String(c);
        htr.appendChild(th);
      });
      thead.appendChild(htr);
      table.appendChild(thead);
    }

    var tbody = document.createElement('tbody');
    rows.forEach(function (row) {
      var tr = document.createElement('tr');
      (Array.isArray(row) ? row : [row]).forEach(function (cell) {
        var td = document.createElement('td');
        td.textContent = cell == null ? '' : String(cell);
        tr.appendChild(td);
      });
      tbody.appendChild(tr);
    });
    table.appendChild(tbody);

    scroll.appendChild(table);
    wrap.appendChild(scroll);
    el.innerHTML = '';
    el.appendChild(wrap);
  });

  function emptyPanelNode(title, body) {
    var wrap = document.createElement('div');
    wrap.className = 'panel-empty';
    var t = document.createElement('div');
    t.className = 'panel-empty-title';
    t.textContent = title;
    var b = document.createElement('div');
    b.className = 'panel-empty-body';
    b.textContent = body;
    wrap.appendChild(t);
    wrap.appendChild(b);
    return wrap;
  }

  window.MayhemPortal = {
    registerPanel: registerPanel,
  };

  // ---- tiny API helper --------------------------------------------------

  // Every call resolves to { ok, status, body, code, message }. Never
  // rejects and never throws — network failures land here exactly like
  // application errors so callers don't need two branches. `code` mirrors
  // server.go's errorResponse.Code ("backend_unavailable" is the one this
  // file branches on specifically).
  function api(method, path) {
    return fetch(path, { method: method, headers: { Accept: 'application/json' } })
      .then(function (res) {
        return res.text().then(function (text) {
          var body = null;
          if (text) {
            try {
              body = JSON.parse(text);
            } catch (e) {
              body = null;
            }
          }
          if (res.ok) {
            return { ok: true, status: res.status, body: body };
          }
          var message = (body && body.error) || ('request failed (status ' + res.status + ')');
          var code = body && body.code;
          return { ok: false, status: res.status, body: body, code: code, message: message };
        });
      })
      .catch(function (err) {
        return { ok: false, status: 0, body: null, code: 'network_error', message: String(err && err.message || err) };
      });
  }

  // ---- DOM refs (bound after DOMContentLoaded) ---------------------------

  var els = {};

  function bindEls() {
    els.stConnDot = document.getElementById('stConnDot');
    els.stDeviceText = document.getElementById('stDeviceText');
    els.stRxChip = document.getElementById('stRxChip');
    els.stTxChip = document.getElementById('stTxChip');
    els.stVersion = document.getElementById('stVersion');
    els.backendError = document.getElementById('backendError');
    els.retryBtn = document.getElementById('retryBtn');
    els.gridView = document.getElementById('gridView');
    els.panelView = document.getElementById('panelView');
    els.searchInput = document.getElementById('searchInput');
    els.categoryChips = document.getElementById('categoryChips');
    els.categorySections = document.getElementById('categorySections');
    els.gridEmpty = document.getElementById('gridEmpty');
    els.backBtn = document.getElementById('backBtn');
    els.panelTitle = document.getElementById('panelTitle');
    els.panelKindLabel = document.getElementById('panelKindLabel');
    els.panelHwBadge = document.getElementById('panelHwBadge');
    els.panelMount = document.getElementById('panelMount');
    // Every renderer adds its own class to the shared mount and none removes
    // it, so remembering what the element looked like before anything drew in
    // it is the only reliable way back. See resetPanelMount().
    els.panelMountBaseClass = els.panelMount ? els.panelMount.className : 'panel-mount';
    els.toast = document.getElementById('toast');
    buildScreenToggle();
    buildGridHeading();
    bindEmptyState();
  }

  // buildGridHeading adds the grid's title and its live count line. Built
  // here for the same reason as the screen toggle: index.html is owned
  // elsewhere and ships only the containers. If that file grows a heading of
  // its own, this defers to it — matching on the ids it would use.
  function buildGridHeading() {
    els.gridMeta = document.getElementById('gridMeta');
    if (els.gridMeta || document.getElementById('gridHeading')) return;

    var bar = document.querySelector('#gridView .grid-toolbar');
    if (!bar) return;

    var heading = document.createElement('div');
    heading.className = 'grid-heading';
    heading.id = 'gridHeading';

    var title = document.createElement('h1');
    title.className = 'grid-heading-title';
    title.textContent = 'Applications';
    heading.appendChild(title);

    var meta = document.createElement('div');
    meta.className = 'grid-heading-meta';
    meta.id = 'gridMeta';
    // Deliberately blank until /api/apps answers: a count typed in here
    // would be a number the backend never sent.
    heading.appendChild(meta);

    bar.insertBefore(heading, bar.firstChild);
    els.gridMeta = meta;
  }

  function bindEmptyState() {
    if (!els.gridEmpty) return;
    els.gridEmptyBody = els.gridEmpty.querySelector('.empty-state-body');
    els.gridEmptyClear = document.getElementById('gridEmptyClear');
    if (els.gridEmptyClear) return;
    var clear = document.createElement('button');
    clear.type = 'button';
    clear.id = 'gridEmptyClear';
    clear.className = 'grid-empty-hint hidden';
    clear.textContent = 'Clear search';
    clear.addEventListener('click', clearSearch);
    els.gridEmpty.appendChild(clear);
    els.gridEmptyClear = clear;
  }

  // buildScreenToggle inserts the Data/Screen control into the panel topbar.
  // It is built here rather than living in index.html because that file is
  // owned elsewhere; the markup it needs is two chips, and the chip styling
  // already exists in app.css.
  function buildScreenToggle() {
    var bar = document.querySelector('#panelView .panel-topbar');
    if (!bar) return;

    var wrap = document.createElement('div');
    wrap.className = 'chip-row hidden';
    wrap.id = 'screenToggle';
    // role="group" with aria-pressed buttons, for the same reason as the
    // category chips: no roving tabindex, no aria-controls, no tabpanel.
    wrap.setAttribute('role', 'group');
    wrap.setAttribute('aria-label', 'Panel view');
    wrap.style.marginLeft = 'auto';

    function makeChip(label, mode, title) {
      var chip = document.createElement('button');
      chip.type = 'button';
      chip.className = 'chip';
      chip.textContent = label;
      chip.title = title;
      chip.addEventListener('click', function () { setScreenMode(mode); });
      wrap.appendChild(chip);
      return chip;
    }

    els.screenChipData = makeChip('Data', 'data', 'The structured browser view of this app');
    els.screenChipScreen = makeChip('Screen', 'screen', "Mirror the device's own 240x320 screen, and drive it from here");
    els.screenToggle = wrap;

    if (els.panelHwBadge && els.panelHwBadge.parentNode === bar) {
      bar.insertBefore(wrap, els.panelHwBadge);
    } else {
      bar.appendChild(wrap);
    }
  }

  // ---- state --------------------------------------------------------------

  var state = {
    groups: [],       // [{category, apps:[...]}]
    appsById: Object.create(null),
    tiles: [],        // [{app, el, category, hay, visible}] — built once per app list
    sections: [],     // [{category, el, countEl, total, tiles}]
    tileById: Object.create(null),
    appsSignature: null, // everything the tiles are drawn from, to notice a real change
    runningAppId: '',    // from /api/apps/current; drives the RUN marker
    search: '',
    category: 'all',
    view: 'grid',      // 'grid' | 'panel'
    currentApp: null,  // client.CurrentApp-shaped: {id, title, can_go_back}
    status: null,      // last GET /api/status body, for deviceCanTransmit()
    backendUp: null,   // null = unknown yet, true/false after first probe
    appsLoaded: false, // true once /api/apps has succeeded at least once
    statusTimer: null,
    panelTimer: null,
    launching: false,
    screenMode: 'data',   // 'data' | 'screen' — which view the toggle selects
    screenMounted: false, // whether panelMount currently holds the live screen
    panelKind: null,      // last panel_kind seen, to notice a change
  };

  // ---- toast --------------------------------------------------------------

  function toast(message, isError) {
    var item = document.createElement('div');
    item.className = 'toast-item' + (isError ? ' error' : '');
    item.textContent = message;
    els.toast.appendChild(item);
    setTimeout(function () {
      if (item.parentNode) item.parentNode.removeChild(item);
    }, TOAST_MS);
  }

  // ---- header / status ------------------------------------------------

  function renderStatus(status) {
    state.status = status;
    // Transmit capability arrives with the device, so the lockout has to be
    // re-evaluated here rather than only when the app list loads.
    relockTiles();
    els.stDeviceText.textContent = status.device || 'No device';
    els.stConnDot.className = 'dot ' + (status.device ? 'ok' : 'off');
    els.stRxChip.className = 'rxtx-chip' + (status.receiving ? ' active rx' : '');
    els.stTxChip.className = 'rxtx-chip' + (status.transmitting ? ' active tx' : '');
    els.stVersion.textContent = status.version ? ('v' + status.version) : '';
  }

  function setBackendDown(down) {
    if (state.backendUp === !down) return; // no change
    state.backendUp = !down;
    els.backendError.classList.toggle('hidden', !down);
    if (down) {
      stopPanelPoll();
      els.stConnDot.className = 'dot off';
      els.stDeviceText.textContent = 'not running';
    }
  }

  function pollStatus() {
    api('GET', '/api/status').then(function (res) {
      if (!res.ok) {
        setBackendDown(res.code === 'backend_unavailable' || res.status === 0);
        scheduleStatusPoll();
        return;
      }
      // Recovering from a down state (or never having loaded successfully
      // at all) means the grid is stale/empty and the panel poll may have
      // been stopped by setBackendDown(true) — bring both back without
      // waiting for the user to notice and hit "Retry now".
      var wasDown = state.backendUp === false;
      setBackendDown(false);
      renderStatus(res.body || {});
      if (wasDown || !state.appsLoaded) {
        loadApps();
        if (state.view === 'panel') startPanelPoll();
      }
      // Only while the grid is visible: in panel view the panel poll already
      // tracks the open app, and this would be a request per two seconds for
      // a marker nobody can see.
      if (state.view === 'grid') refreshRunningApp();
      scheduleStatusPoll();
    });
  }

  function scheduleStatusPoll() {
    if (state.statusTimer) clearTimeout(state.statusTimer);
    state.statusTimer = setTimeout(pollStatus, state.backendUp === false ? BACKEND_RETRY_MS : STATUS_POLL_MS);
  }

  // ---- app grid: what each badge is allowed to claim -----------------------

  // Categories whose apps key the transmitter. Read from the app's own
  // `category`, which is app::category_name() on the C++ side — not from a
  // list of app ids, which would rot the moment the registry changes.
  var TX_CATEGORIES = { Transmit: true, Transceiver: true };

  // Transceiver apps are deliberately NOT here. They both receive and
  // transmit, so a receive-only radio still runs them usefully; locking them
  // out would take away function the hardware genuinely has. Only the
  // pure-transmit category is blocked.
  var LOCKABLE_CATEGORIES = { Transmit: true };

  function isTransmitApp(app) {
    return !!(app && TX_CATEGORIES[app.category]);
  }

  // nativePanelKindFor answers "does this app have a real browser panel?" —
  // the single source for the NATIVE badge.
  //
  // The honest answer today is "unknown". GET /api/apps carries id,
  // display_name, category, hardware_limited and icon (AppSummary in
  // src/remote/app_data.cpp) and says nothing about panels; a panel kind
  // exists only for the app that is *currently open*, on GET /api/panel.
  // Deriving it from the id, or shipping "the apps that have panels" as a
  // list here, would put a badge on the grid that goes stale the first time
  // the C++ side registers or drops a panel provider — and a badge that
  // lies about which apps have a real view is worse than no badge. So this
  // returns null and the badge is simply not drawn on any tile.
  //
  // TODO(api): ONE FIELD MAKES IT REAL — `panel_kind` on each entry of
  // GET /api/apps.
  // That means AppSummary in src/remote/app_data.{hpp,cpp} publishing it,
  // and client.App in internal/portal/client/types.go carrying it — the
  // portal re-encodes that struct, so a field the Go type does not know
  // about is dropped before the browser ever sees it. This function already
  // reads `panel_kind` off the app object, so nothing else here changes.
  function nativePanelKindFor(app) {
    var kind = (app && typeof app.panel_kind === 'string') ? app.panel_kind : '';
    // "screen" is the framebuffer mirror every app has, not a native panel.
    if (!kind || kind === 'screen') return null;
    return kind;
  }

  // deviceCanTransmit reports whether the attached radio can transmit at
  // all: true, false, or null for "the API has not said".
  //
  // Null today. The value exists on the C++ side as DeviceCaps.has_tx
  // (src/radio/radio_device.hpp — "An RTL-SDR is receive-only"), but no hop
  // puts it on the wire: GET /api/status sends device, device_ok, receiving,
  // transmitting, version and levels.
  //
  // TODO(api): make it real with a `can_transmit` boolean on GET /api/status
  // — set from caps().has_tx in app_bridge.cpp's status object, and mirrored
  // on client.Status in internal/portal/client/types.go, for the same
  // re-encoding reason as above. `receiving`/`transmitting` are
  // NOT that field — they say what the radio is doing this second, not what
  // it is able to do, and treating them as capability would lock every
  // transmit app whenever the radio happened to be idle.
  function deviceCanTransmit() {
    var v = state.status ? state.status.can_transmit : undefined;
    return typeof v === 'boolean' ? v : null;
  }

  function appIsLocked(app) {
    return deviceCanTransmit() === false && !!(app && LOCKABLE_CATEGORIES[app.category]);
  }

  // ---- app grid: icons -----------------------------------------------------

  // The animated icon set is a separate module owned elsewhere. This is the
  // only place that touches it, so a module that is missing, renamed or
  // still loading costs a neutral glyph per tile and nothing else.
  //
  // Contract, in the order it is tried:
  //
  //   window.MayhemIcons.render(id, category) -> Element | SVG string | null
  //   window.MayhemIcons.create(id, category) -> same
  //   window.MayhemIcons.icon(id, category)   -> same
  //   window.MayhemIcons.get(id, category)    -> same
  //
  // `render` is what the icon set that ships in this binary exports; the
  // other three are kept as aliases so a differently-named drop-in still
  // works. It is deliberately first: an earlier revision listed only the
  // aliases, so the real module resolved to no provider at all and all 94
  // tiles silently rendered the fallback letter.
  //
  // `id` is the app's `icon` hint when it has one, else its `id` (the C++
  // side currently sets icon = id, as a stable slug). `category` is the
  // app's live category, which lets the module fall back to a per-category
  // default glyph for an app id it has never seen — a backend that gains an
  // app tomorrow gets a sensible icon rather than a blank slot. A provider
  // that only takes one argument simply ignores the second.
  //
  // An unknown id must return null/undefined rather than throw, but a throw
  // is caught here too: one broken icon may not take the grid down with it.
  //
  // A module that finishes loading after the grid is built (a dynamic
  // import, say) can dispatch `mayhem-icons-ready` on document and every
  // tile's icon slot is refilled.
  function iconProvider() {
    var m = window.MayhemIcons;
    if (!m) return null;
    var fn = m.render || m.create || m.icon || m.get;
    return typeof fn === 'function' ? fn.bind(m) : null;
  }

  function appIconNode(app) {
    var slot = document.createElement('span');
    slot.className = 'app-icon';
    slot.setAttribute('aria-hidden', 'true');
    slot.dataset.appIcon = (app && (app.icon || app.id)) || '';
    fillIconSlot(slot, app);
    return slot;
  }

  function fillIconSlot(slot, app) {
    var provide = iconProvider();
    var made = null;
    if (provide) {
      try {
        made = provide(slot.dataset.appIcon, (app && app.category) || '');
      } catch (e) {
        made = null; // a broken icon is a missing icon, not a broken grid
      }
    }
    slot.innerHTML = '';
    if (made && made.nodeType === 1) {
      slot.appendChild(made);
      slot.dataset.iconSource = 'set';
      return;
    }
    if (typeof made === 'string' && made.trim()) {
      // Trusted: same-origin module shipped in this binary, like every other
      // script on the page.
      slot.innerHTML = made;
      slot.dataset.iconSource = 'set';
      return;
    }
    slot.appendChild(neutralGlyphNode(app));
    slot.dataset.iconSource = 'fallback';
  }

  function neutralGlyphNode(app) {
    var glyph = document.createElement('span');
    glyph.className = 'app-icon-fallback';
    var s = (app && (app.display_name || app.id)) || '?';
    glyph.textContent = s.trim().charAt(0).toUpperCase() || '?';
    return glyph;
  }

  // Re-run the icon lookup over tiles that are still showing the fallback.
  // Cheap and idempotent: tiles that already got a real icon are skipped.
  function refreshIcons() {
    state.tiles.forEach(function (t) {
      var slot = t.el.querySelector('.app-icon');
      if (slot && slot.dataset.iconSource !== 'set') fillIconSlot(slot, t.app);
    });
  }

  // ---- app grid: build -----------------------------------------------------

  function categoryLabel(cat) {
    return cat || 'Uncategorized';
  }

  // The grid is built once per app list and then filtered by toggling
  // visibility, rather than rebuilt on every keystroke: at 94 tiles a
  // rebuild is affordable but it would also restart every icon animation
  // and throw away the icon module's nodes on each character typed.
  function buildGrid() {
    els.categorySections.innerHTML = '';
    state.tiles = [];
    state.sections = [];
    state.tileById = Object.create(null);

    state.groups.forEach(function (g, i) {
      var apps = Array.isArray(g.apps) ? g.apps : [];
      var section = document.createElement('section');
      section.className = 'category-section';
      section.dataset.category = g.category || '';

      var heading = document.createElement('div');
      heading.className = 'category-heading';

      var index = document.createElement('span');
      index.className = 'category-index';
      // Position in the live category order, not a fixed numbering: a
      // category the backend stops publishing must not leave a gap.
      index.textContent = (i + 1 < 10 ? '0' : '') + (i + 1);
      // Decorative ordering; the three parts of this heading sit next to
      // each other with no separating text, so read aloud it would come out
      // as "01Receive34".
      index.setAttribute('aria-hidden', 'true');
      heading.appendChild(index);

      var h2 = document.createElement('h2');
      h2.textContent = categoryLabel(g.category);
      heading.appendChild(h2);

      var count = document.createElement('span');
      count.className = 'count';
      heading.appendChild(count);

      var rule = document.createElement('span');
      rule.className = 'category-rule';
      heading.appendChild(rule);

      section.appendChild(heading);

      var grid = document.createElement('div');
      grid.className = 'app-grid';
      var tiles = apps.map(function (app) {
        var tile = tileFor(app);
        grid.appendChild(tile.el);
        state.tiles.push(tile);
        state.tileById[app.id] = tile;
        return tile;
      });
      section.appendChild(grid);
      els.categorySections.appendChild(section);

      state.sections.push({
        category: g.category,
        el: section,
        countEl: count,
        total: apps.length,
        tiles: tiles,
      });
    });
  }

  function tileFor(app) {
    var card = document.createElement('button');
    card.type = 'button';
    card.className = 'app-card';
    card.dataset.id = app.id;
    card.dataset.category = app.category || '';

    card.appendChild(appIconNode(app));

    var name = document.createElement('span');
    name.className = 'app-card-name';
    name.textContent = app.display_name || app.id;
    card.appendChild(name);

    var locked = appIsLocked(app);

    // NATIVE: drawn only when the panel kind is known and is a real panel.
    // See nativePanelKindFor — today that is never, by design.
    var kind = nativePanelKindFor(app);
    if (kind) {
      card.appendChild(badgeNode('native', 'NATIVE',
        'Has a native browser panel (' + kind + '), not just the device screen.'));
    }

    if (isTransmitApp(app)) {
      card.appendChild(badgeNode('tx', locked ? 'TX LOCKED' : 'TX',
        locked
          ? 'The attached radio cannot transmit, so this app cannot run here.'
          : 'Transmits: this app keys the radio.'));
    }

    if (app.hardware_limited) {
      card.appendChild(badgeNode('hw', 'HW',
        'Needs hardware this setup does not have; may show an explanation instead of working.'));
    }

    var run = document.createElement('span');
    run.className = 'app-run';
    var dot = document.createElement('span');
    dot.className = 'app-run-dot';
    run.appendChild(dot);
    run.appendChild(document.createTextNode('RUN'));
    card.appendChild(run);

    // The id is the operator's handle on an app (it is what /api/apps and
    // the launch route speak) but it is not worth a line of its own on the
    // tile; keep it reachable and searchable instead.
    card.title = app.id + (app.category ? ' · ' + app.category : '');

    applyLockState(card, app, locked);

    card.addEventListener('click', function () {
      if (appIsLocked(app)) {
        toast((app.display_name || app.id) + ' needs a transmit-capable radio.', true);
        return;
      }
      launchApp(app.id);
    });

    return {
      app: app,
      el: card,
      category: app.category || '',
      // Precomputed so filtering is a substring test, not a lowercase() per
      // tile per keystroke.
      hay: ((app.display_name || '') + ' ' + (app.id || '')).toLowerCase(),
      visible: true,
    };
  }

  function badgeNode(kindClass, text, title) {
    var badge = document.createElement('span');
    badge.className = 'app-badge app-badge-' + kindClass;
    badge.textContent = text;
    badge.title = title;
    return badge;
  }

  function applyLockState(card, app, locked) {
    card.classList.toggle('is-locked', locked);
    // aria-disabled, not disabled: the tile stays focusable so a screen
    // reader user can find out *why* it is unavailable, and the click
    // handler still explains itself.
    if (locked) {
      card.setAttribute('aria-disabled', 'true');
    } else {
      card.removeAttribute('aria-disabled');
    }
  }

  // relockTiles re-evaluates the lockout for every tile. Called when status
  // changes, since transmit capability arrives with the device, not with the
  // app list.
  function relockTiles() {
    state.tiles.forEach(function (t) {
      var locked = appIsLocked(t.app);
      if (locked === t.el.classList.contains('is-locked')) return;
      applyLockState(t.el, t.app, locked);
      var badge = t.el.querySelector('.app-badge-tx');
      if (badge) badge.textContent = locked ? 'TX LOCKED' : 'TX';
    });
  }

  // ---- app grid: chips, filtering, counts ---------------------------------

  function renderChips() {
    var total = 0;
    state.groups.forEach(function (g) { total += g.apps.length; });

    els.categoryChips.innerHTML = '';
    var entries = [{ key: 'all', label: 'All', count: total }];
    state.groups.forEach(function (g) {
      entries.push({ key: g.category, label: categoryLabel(g.category), count: g.apps.length });
    });

    entries.forEach(function (entry) {
      var active = state.category === entry.key;
      var chip = document.createElement('button');
      chip.type = 'button';
      chip.className = 'chip' + (active ? ' active' : '');
      // A toggle button, not a tab. The ARIA tab pattern promises a roving
      // tabindex, arrow-key navigation and an aria-controls'd tabpanel; this
      // control has none of those and owns no panel, so claiming role="tab"
      // told a screen reader to expect a keyboard contract that does not
      // exist. aria-pressed on a plain button is what these actually are.
      chip.setAttribute('aria-pressed', String(active));
      chip.appendChild(document.createTextNode(entry.label + ' '));
      var count = document.createElement('span');
      count.className = 'count';
      count.textContent = String(entry.count);
      chip.appendChild(count);
      chip.addEventListener('click', function () {
        state.category = entry.key;
        renderChips();
        applyFilter();
      });
      els.categoryChips.appendChild(chip);
    });
  }

  function applyFilter() {
    var q = state.search;
    var cat = state.category;
    var shown = 0;
    var scope = 0; // apps the category filter alone leaves in play

    state.sections.forEach(function (sec) {
      var inScope = cat === 'all' || sec.category === cat;
      var visible = 0;
      sec.tiles.forEach(function (t) {
        var ok = inScope && (!q || t.hay.indexOf(q) !== -1);
        if (ok !== t.visible) {
          t.visible = ok;
          t.el.classList.toggle('hidden', !ok);
        }
        if (ok) visible++;
      });
      if (inScope) scope += sec.total;
      sec.el.classList.toggle('hidden', visible === 0);
      sec.countEl.textContent = visible === sec.total
        ? String(sec.total)
        : visible + '/' + sec.total;
      // A bare number next to the category name is clear enough to look at
      // and meaningless to listen to.
      sec.countEl.setAttribute('aria-label', visible === sec.total
        ? sec.total + ' apps'
        : visible + ' of ' + sec.total + ' apps shown');
      shown += visible;
    });

    updateGridMeta(shown);
    updateEmptyState(shown, scope);
  }

  function updateGridMeta(shown) {
    if (!els.gridMeta) return;
    var total = state.tiles.length;
    var cats = state.sections.length;
    els.gridMeta.textContent = shown + ' / ' + total + ' apps · ' +
      cats + ' categor' + (cats === 1 ? 'y' : 'ies');
  }

  function updateEmptyState(shown, scope) {
    els.gridEmpty.classList.toggle('hidden', shown > 0 || state.tiles.length === 0);
    if (shown > 0 || !els.gridEmptyBody) return;
    var where = state.category === 'all' ? 'apps' : categoryLabel(state.category) + ' apps';
    els.gridEmptyBody.textContent = state.search
      ? '0 of ' + scope + ' ' + where + ' match “' + state.search + '”.'
      : 'No ' + where + ' are published by mayhem-b200 right now.';
    if (els.gridEmptyClear) {
      els.gridEmptyClear.classList.toggle('hidden', !state.search);
    }
  }

  function clearSearch() {
    state.search = '';
    if (els.searchInput) els.searchInput.value = '';
    applyFilter();
    if (els.searchInput) els.searchInput.focus();
  }

  // ---- app grid: the running app ------------------------------------------

  function setRunningAppId(id) {
    id = id || '';
    if (state.runningAppId === id) return;
    var prev = state.tileById[state.runningAppId];
    if (prev) prev.el.classList.remove('is-running');
    state.runningAppId = id;
    var next = state.tileById[id];
    if (next) next.el.classList.add('is-running');
  }

  // Polled while the grid is on screen so an app opened with the device's
  // own keys shows as running here too. The view is never switched from
  // under the operator — this only moves the RUN marker.
  function refreshRunningApp() {
    return api('GET', '/api/apps/current').then(function (res) {
      if (!res.ok) return;
      setRunningAppId(res.body && res.body.id);
    });
  }

  function loadApps() {
    return api('GET', '/api/apps').then(function (res) {
      if (!res.ok) {
        setBackendDown(res.code === 'backend_unavailable' || res.status === 0);
        return;
      }
      setBackendDown(false);
      state.appsLoaded = true;
      var body = res.body || {};
      state.groups = Array.isArray(body.groups) ? body.groups : [];
      state.appsById = Object.create(null);
      var signature = [];
      state.groups.forEach(function (g) {
        g.apps.forEach(function (a) {
          state.appsById[a.id] = a;
          // Everything the tiles are drawn from, not just the id: the day
          // the API starts carrying a panel kind per app, the NATIVE badges
          // appear on the next poll instead of waiting for the registry to
          // change shape.
          signature.push(g.category + '/' + a.id + '/' + (a.panel_kind || '') +
            (a.hardware_limited ? '/hw' : ''));
        });
      });

      // A category that disappeared must not leave the grid filtered to
      // nothing with no chip showing why.
      if (state.category !== 'all' && !state.groups.some(function (g) { return g.category === state.category; })) {
        state.category = 'all';
      }

      // Rebuilding tears out the icon module's nodes and restarts their
      // animations, so only do it when the registry actually changed.
      var sig = signature.join(',');
      if (sig !== state.appsSignature) {
        state.appsSignature = sig;
        buildGrid();
        var running = state.runningAppId;
        state.runningAppId = '';
        setRunningAppId(running);
      }
      renderChips();
      applyFilter();
    });
  }

  // ---- view switching -------------------------------------------------

  // resetPanelMount puts #panelMount back to exactly the state it was in at
  // page load: empty, with its original class list, and with every marker a
  // renderer may have left in its dataset gone.
  //
  // Two live bugs made this a function rather than an `innerHTML = ''`:
  //
  //  - The nine renderers only ever classList.add their kind class, and they
  //    all share this one element, so after visiting every panel the mount
  //    read "panel-mount mp-panel mp-table-panel mp-spectrum-panel
  //    mp-adsb mp-image-panel ...". A rule as ordinary as `.mp-image-panel
  //    canvas` then framed the spectrum's trace for anyone who happened to
  //    open an image app first.
  //
  //  - Panels self-cancel their animation loops by testing whether their root
  //    is still connected to the document. Leaving the panel view only added
  //    a `hidden` class to the section, so the ADS-B canvas stayed connected
  //    for the life of the page and its 60fps loop ran forever behind the
  //    grid — measured at 360 rAF callbacks over 3s while the grid was up.
  //
  // Deleting dataset.mpKind is what makes the rest safe: panels/registry.js
  // treats a changed kind as "wipe the __mp* state stashed on this element",
  // so the next render starts from scratch instead of finding cached
  // references to DOM that no longer exists.
  function resetPanelMount() {
    var el = els.panelMount;
    if (!el) return;
    el.innerHTML = '';
    el.className = els.panelMountBaseClass;
    delete el.dataset.mpKind;
    delete el.dataset.mpHaveImageRev;
  }

  function showGrid() {
    state.view = 'grid';
    els.panelView.classList.add('hidden');
    els.gridView.classList.remove('hidden');
    stopPanelPoll();
    // The panel section is only hidden, not emptied, so the screen view has
    // to be told explicitly — otherwise its socket keeps streaming frames
    // into a canvas nobody can see.
    unmountScreenView();
    // ...and the same is true of every other renderer: see resetPanelMount.
    resetPanelMount();
    updateScreenToggle('');
    // Don't wait out the status interval to find out what the device is on
    // now — the operator just came back from an app.
    refreshRunningApp();
  }

  function showPanel(cur) {
    state.view = 'panel';
    state.currentApp = cur;
    els.gridView.classList.add('hidden');
    els.panelView.classList.remove('hidden');

    // The grid is hidden from here on and is not polled for the running app
    // while it is, so record what we just opened: returning to the grid
    // shows the right tile marked immediately, before showGrid's refresh
    // lands.
    setRunningAppId(cur.id);

    els.panelTitle.textContent = cur.title || cur.id || 'App';
    var app = state.appsById[cur.id];
    els.panelHwBadge.classList.toggle('hidden', !(app && app.hardware_limited));
    els.panelKindLabel.textContent = '';
    unmountScreenView();
    resetPanelMount();
    // Each app starts on its own structured view; the toggle is per-app, not
    // a sticky global preference.
    state.screenMode = 'data';
    state.panelKind = null;
    updateScreenToggle('');

    startPanelPoll();
  }

  // ---- live device screen -------------------------------------------------

  function screenViewAvailable() {
    return !!(window.MayhemScreenView && typeof window.MayhemScreenView.mount === 'function');
  }

  function mountScreenView() {
    if (state.screenMounted) return;
    resetPanelMount();
    state.screenMounted = true;
    if (!screenViewAvailable()) {
      // index.html is owned elsewhere; if its <script src="screenview.js">
      // tag is missing this must say so, not silently show an empty panel.
      els.panelMount.appendChild(emptyPanelNode(
        'Live screen unavailable',
        'screenview.js is not loaded in this page, so the device display cannot be mirrored here.'
      ));
      return;
    }
    window.MayhemScreenView.mount(els.panelMount);
  }

  function unmountScreenView() {
    if (!state.screenMounted) return;
    if (screenViewAvailable() && typeof window.MayhemScreenView.unmount === 'function') {
      window.MayhemScreenView.unmount(els.panelMount);
    }
    resetPanelMount();
    state.screenMounted = false;
  }

  function setScreenMode(mode) {
    if (state.screenMode === mode) return;
    state.screenMode = mode;
    if (mode !== 'screen') unmountScreenView();
    updateScreenToggle(state.panelKind || '');
    // Re-render now instead of waiting out the rest of the poll interval.
    if (state.view === 'panel') startPanelPoll();
  }

  // updateScreenToggle shows the Data/Screen control only where there is a
  // real choice. panel_kind "screen" IS the live screen, so for those apps
  // the control would be a toggle between the same thing and itself.
  function updateScreenToggle(kind) {
    if (!els.screenToggle) return;
    var show = state.view === 'panel' && !!kind && kind !== 'screen';
    els.screenToggle.classList.toggle('hidden', !show);
    if (!show) return;
    var onScreen = state.screenMode === 'screen';
    els.screenChipData.classList.toggle('active', !onScreen);
    els.screenChipScreen.classList.toggle('active', onScreen);
    els.screenChipData.setAttribute('aria-pressed', String(!onScreen));
    els.screenChipScreen.setAttribute('aria-pressed', String(onScreen));
  }

  function launchApp(id) {
    if (state.launching) return;
    state.launching = true;
    api('POST', '/api/apps/' + encodeURIComponent(id) + '/launch').then(function (res) {
      state.launching = false;
      if (!res.ok) {
        if (res.code === 'backend_unavailable' || res.status === 0) {
          setBackendDown(true);
        } else {
          toast('Could not launch: ' + res.message, true);
        }
        return;
      }
      showPanel(res.body || { id: id, title: id, can_go_back: true });
    });
  }

  function goHome() {
    api('POST', '/api/apps/home').then(function (res) {
      if (!res.ok) {
        if (res.code === 'backend_unavailable' || res.status === 0) {
          setBackendDown(true);
        } else {
          toast('Could not return home: ' + res.message, true);
        }
        return;
      }
      showGrid();
      loadApps();
    });
  }

  // ---- panel polling ------------------------------------------------------

  function stopPanelPoll() {
    if (state.panelTimer) {
      clearTimeout(state.panelTimer);
      state.panelTimer = null;
    }
  }

  function startPanelPoll() {
    stopPanelPoll();
    pollPanel();
  }

  // panelPath adds contract 4's have_image_rev token when the mounted
  // renderer has published one (see the header comment).
  function panelPath() {
    var rev = els.panelMount.dataset.mpHaveImageRev;
    if (!rev) return '/api/panel';
    return '/api/panel?have_image_rev=' + encodeURIComponent(rev);
  }

  function pollPanel() {
    if (state.view !== 'panel') return;
    api('GET', panelPath()).then(function (res) {
      if (state.view !== 'panel') return; // navigated away while in flight

      if (!res.ok) {
        if (res.code === 'backend_unavailable' || res.status === 0) {
          setBackendDown(true);
          return; // status poll will retry and bring us back
        }
        // The live screen has its own socket and its own status line; a
        // failed /api/panel poll says nothing about it, so leave it alone
        // rather than tearing the canvas out from under the operator.
        if (!state.screenMounted) {
          resetPanelMount();
          els.panelMount.appendChild(emptyPanelNode(
            'Panel unavailable',
            res.message || 'mayhem-b200 returned an error for this app’s panel data.'
          ));
        }
        state.panelTimer = setTimeout(pollPanel, PANEL_POLL_MS);
        return;
      }

      var panel = res.body || {};
      var kind = panel.panel_kind || '';
      els.panelKindLabel.textContent = kind || '';

      if (kind !== state.panelKind) {
        // A different app, or the same app publishing a different kind. Start
        // the mount clean: an image revision cached for the old panel says
        // nothing about this one and would suppress the new panel's first
        // image, and the outgoing renderer's kind class would otherwise stay
        // on the element for the rest of the page's life (resetPanelMount).
        //
        // Not while the live screen is mounted: that view owns a socket and
        // has to be torn down through its own unmount(), which the branches
        // below do. Its dataset marker is cleared here either way.
        if (state.screenMounted) {
          delete els.panelMount.dataset.mpHaveImageRev;
        } else {
          resetPanelMount();
        }
        state.panelKind = kind;
      }
      updateScreenToggle(kind);

      // "screen" is the live framebuffer; every other kind can be swapped to
      // it with the toggle.
      if (kind === 'screen' || state.screenMode === 'screen') {
        if (panel.title) els.panelTitle.textContent = panel.title;
        mountScreenView();
        state.panelTimer = setTimeout(pollPanel, PANEL_POLL_MS);
        return;
      }
      unmountScreenView();

      if (!kind) {
        resetPanelMount();
        els.panelMount.appendChild(emptyPanelNode(
          'This app has no structured view yet',
          'It is still running — the device screen has its own display, this browser just has nothing to draw here.'
        ));
      } else if (!panelRegistry[kind]) {
        resetPanelMount();
        els.panelMount.appendChild(emptyPanelNode(
          'No renderer for "' + kind + '"',
          'This panel kind is not recognized by this build of the portal.'
        ));
      } else {
        if (panel.title) els.panelTitle.textContent = panel.title;
        try {
          panelRegistry[kind](els.panelMount, panel.data);
        } catch (e) {
          resetPanelMount();
          els.panelMount.appendChild(emptyPanelNode('Renderer error', String(e && e.message || e)));
        }
      }

      state.panelTimer = setTimeout(pollPanel, PANEL_POLL_MS);
    });
  }

  // ---- bootstrap ------------------------------------------------------

  function init() {
    bindEls();

    els.retryBtn.addEventListener('click', function () {
      pollStatus();
      loadApps();
    });
    els.backBtn.addEventListener('click', goHome);
    els.searchInput.addEventListener('input', function () {
      state.search = els.searchInput.value.trim().toLowerCase();
      applyFilter();
    });

    // An icon module that loads asynchronously announces itself here; tiles
    // still showing the neutral glyph pick up their real icon. No-op if the
    // event never fires, which is the case when the module is a plain
    // <script> that ran before this one.
    document.addEventListener('mayhem-icons-ready', refreshIcons);

    pollStatus();
    loadApps().then(function () {
      return api('GET', '/api/apps/current');
    }).then(function (res) {
      if (res && res.ok && res.body && res.body.id) {
        setRunningAppId(res.body.id);
        showPanel(res.body);
      }
    });
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
