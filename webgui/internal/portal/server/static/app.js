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
 */
(function () {
  'use strict';

  // ---- tunables -----------------------------------------------------------

  var STATUS_POLL_MS = 2000;
  var PANEL_POLL_MS = 700;
  var BACKEND_RETRY_MS = 3000;
  var TOAST_MS = 4500;

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
    els.toast = document.getElementById('toast');
  }

  // ---- state --------------------------------------------------------------

  var state = {
    groups: [],       // [{category, apps:[...]}]
    appsById: Object.create(null),
    search: '',
    category: 'all',
    view: 'grid',      // 'grid' | 'panel'
    currentApp: null,  // client.CurrentApp-shaped: {id, title, can_go_back}
    backendUp: null,   // null = unknown yet, true/false after first probe
    appsLoaded: false, // true once /api/apps has succeeded at least once
    statusTimer: null,
    panelTimer: null,
    launching: false,
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
      scheduleStatusPoll();
    });
  }

  function scheduleStatusPoll() {
    if (state.statusTimer) clearTimeout(state.statusTimer);
    state.statusTimer = setTimeout(pollStatus, state.backendUp === false ? BACKEND_RETRY_MS : STATUS_POLL_MS);
  }

  // ---- app grid ------------------------------------------------------------

  function categoryLabel(cat) {
    return cat || 'Uncategorized';
  }

  function iconGlyph(app) {
    var s = app.display_name || app.id || '?';
    return s.trim().charAt(0).toUpperCase() || '?';
  }

  function appMatchesFilter(app) {
    if (state.category !== 'all' && app.category !== state.category) return false;
    if (!state.search) return true;
    var q = state.search;
    return (app.display_name && app.display_name.toLowerCase().indexOf(q) !== -1) ||
      (app.id && app.id.toLowerCase().indexOf(q) !== -1);
  }

  function renderChips() {
    var counts = { all: 0 };
    state.groups.forEach(function (g) {
      counts[g.category] = g.apps.length;
      counts.all += g.apps.length;
    });

    els.categoryChips.innerHTML = '';
    var cats = ['all'].concat(state.groups.map(function (g) { return g.category; }));
    cats.forEach(function (cat) {
      var chip = document.createElement('button');
      chip.type = 'button';
      chip.className = 'chip' + (state.category === cat ? ' active' : '');
      chip.setAttribute('role', 'tab');
      chip.setAttribute('aria-selected', String(state.category === cat));
      var label = cat === 'all' ? 'All' : categoryLabel(cat);
      chip.innerHTML = label + ' <span class="count">' + counts[cat] + '</span>';
      chip.addEventListener('click', function () {
        state.category = cat;
        renderChips();
        renderGrid();
      });
      els.categoryChips.appendChild(chip);
    });
  }

  function appCardNode(app) {
    var card = document.createElement('button');
    card.type = 'button';
    card.className = 'app-card';
    card.dataset.id = app.id;

    var icon = document.createElement('div');
    icon.className = 'app-card-icon';
    icon.textContent = iconGlyph(app);
    card.appendChild(icon);

    var name = document.createElement('div');
    name.className = 'app-card-name';
    name.textContent = app.display_name || app.id;
    card.appendChild(name);

    var id = document.createElement('div');
    id.className = 'app-card-id';
    id.textContent = app.id;
    card.appendChild(id);

    if (app.hardware_limited) {
      var badge = document.createElement('span');
      badge.className = 'hw-badge';
      badge.textContent = 'HW LIMITED';
      badge.title = 'Needs hardware this B200 setup does not have; may show an explanation instead of working.';
      card.appendChild(badge);
    }

    card.addEventListener('click', function () {
      launchApp(app.id);
    });
    return card;
  }

  function renderGrid() {
    els.categorySections.innerHTML = '';
    var q = state.search;
    var anyVisible = false;

    state.groups.forEach(function (g) {
      var visibleApps = g.apps.filter(appMatchesFilter);
      if (state.category !== 'all' && g.category !== state.category) return;
      if (visibleApps.length === 0) return;
      anyVisible = true;

      var section = document.createElement('div');
      section.className = 'category-section';

      var heading = document.createElement('div');
      heading.className = 'category-heading';
      heading.innerHTML = '<h2>' + categoryLabel(g.category) + '</h2>' +
        '<span class="count">' + visibleApps.length + '</span>';
      section.appendChild(heading);

      var grid = document.createElement('div');
      grid.className = 'app-grid';
      visibleApps.forEach(function (app) {
        grid.appendChild(appCardNode(app));
      });
      section.appendChild(grid);

      els.categorySections.appendChild(section);
    });

    els.gridEmpty.classList.toggle('hidden', anyVisible);
    void q;
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
      state.groups.forEach(function (g) {
        g.apps.forEach(function (a) { state.appsById[a.id] = a; });
      });
      renderChips();
      renderGrid();
    });
  }

  // ---- view switching -------------------------------------------------

  function showGrid() {
    state.view = 'grid';
    els.panelView.classList.add('hidden');
    els.gridView.classList.remove('hidden');
    stopPanelPoll();
  }

  function showPanel(cur) {
    state.view = 'panel';
    state.currentApp = cur;
    els.gridView.classList.add('hidden');
    els.panelView.classList.remove('hidden');

    els.panelTitle.textContent = cur.title || cur.id || 'App';
    var app = state.appsById[cur.id];
    els.panelHwBadge.classList.toggle('hidden', !(app && app.hardware_limited));
    els.panelKindLabel.textContent = '';
    els.panelMount.innerHTML = '';

    startPanelPoll();
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

  function pollPanel() {
    if (state.view !== 'panel') return;
    api('GET', '/api/panel').then(function (res) {
      if (state.view !== 'panel') return; // navigated away while in flight

      if (!res.ok) {
        if (res.code === 'backend_unavailable' || res.status === 0) {
          setBackendDown(true);
          return; // status poll will retry and bring us back
        }
        els.panelMount.innerHTML = '';
        els.panelMount.appendChild(emptyPanelNode(
          'Panel unavailable',
          res.message || 'mayhem-b200 returned an error for this app’s panel data.'
        ));
        state.panelTimer = setTimeout(pollPanel, PANEL_POLL_MS);
        return;
      }

      var panel = res.body || {};
      var kind = panel.panel_kind || '';
      els.panelKindLabel.textContent = kind || '';

      if (!kind) {
        els.panelMount.innerHTML = '';
        els.panelMount.appendChild(emptyPanelNode(
          'This app has no structured view yet',
          'It is still running — the device screen has its own display, this browser just has nothing to draw here.'
        ));
      } else if (!panelRegistry[kind]) {
        els.panelMount.innerHTML = '';
        els.panelMount.appendChild(emptyPanelNode(
          'No renderer for "' + kind + '"',
          'This panel kind is not recognized by this build of the portal.'
        ));
      } else {
        if (panel.title) els.panelTitle.textContent = panel.title;
        try {
          panelRegistry[kind](els.panelMount, panel.data);
        } catch (e) {
          els.panelMount.innerHTML = '';
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
      renderGrid();
    });

    pollStatus();
    loadApps().then(function () {
      return api('GET', '/api/apps/current');
    }).then(function (res) {
      if (res && res.ok && res.body && res.body.id) {
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
