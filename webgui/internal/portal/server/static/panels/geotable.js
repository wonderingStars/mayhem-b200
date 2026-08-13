// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// The "geotable" panel: a map above the table it came from, for the apps
// whose entries are positions -- AIS, APRS, WardriveMap. Data shape is
// documented in ../../../PANELS.md.
//
// This file composes; it does not draw. The map half is
// MayhemPanels.renderPanel("map", ...) and the table half is
// renderPanel("table", ...), both called on child elements this panel owns,
// so there is exactly one map renderer and one table renderer in this repo
// and a fix to either lands here for free. Everything below is layout and
// the one number neither half can show on its own: how many of the table's
// entries actually made it onto the map.
//
// The absent-position rule is the whole reason this kind exists as its own
// panel rather than a table with a map bolted on. A station heard only
// through a status or telemetry packet, or a vessel that has broadcast its
// name but not yet its position, belongs in the table and NOT on the map. The
// backend enforces that by only emitting a marker for an entry with a real
// fix (see provider_aprs.cpp's has_position and provider_ais.cpp's
// latitude.is_valid() gate); this file's job is to not undo it -- there is no
// path here that derives a marker from a table row.

(() => {
  "use strict";

  // countLocated is the one number neither child renderer can show on its
  // own: how many of the table's entries actually made it onto the map. It
  // uses the same has-a-real-position test panels/map.js uses to decide what
  // it can draw, so "3 of 5 located" can never disagree with the map.
  //
  // The marker array itself is passed through UNTOUCHED. The contract's shape
  // (heading_deg, no id) is reconciled inside map.js's normalizeMarkers(),
  // where it covers every host of that renderer rather than only this one --
  // this panel used to carry its own copy, which left the direct `map` kind
  // (WardriveMap) with no adaptation at all.
  function countLocated(markers) {
    let n = 0;
    markers.forEach((m) => {
      if (m && Number.isFinite(m.lat) && Number.isFinite(m.lon)) n++;
    });
    return n;
  }

  // The two halves are full .mp-panel renderers in their own right, so they
  // arrive with their own border, radius, shadow and height:100%. Inside this
  // panel they are flex children of one card, so strip the chrome that would
  // otherwise draw a card inside a card, and let flex do the sizing.
  //
  // The min-height is load-bearing, not padding. The portal shell gives
  // .panel-mount no definite height (app.css: `flex: 1`, on a chain with no
  // resolved height), so .mp-panel's `height: 100%` resolves to auto and a
  // flex child sized only as a percentage of that would come out zero-high --
  // the map would be a canvas of no height at all. With a floor, the panel
  // has an intrinsic height when the host imposes none, and the percentages
  // still divide the space when it does (as harness.html's does).
  function asSection(el, grow, minHeight) {
    el.style.border = "none";
    el.style.borderRadius = "0";
    el.style.boxShadow = "none";
    el.style.background = "transparent";
    el.style.height = "auto";
    el.style.minHeight = minHeight;
    // flex-basis 0 with a weighted grow, NOT a percentage basis. A percentage
    // basis is resolved against the container and then GROWN by the content
    // that cannot shrink -- the map's canvas sizes itself to its wrapper --
    // so `flex: 1 1 55%` made the two sections total 1006px inside a 781px
    // card, overflowing it. With a zero basis the whole card height is free
    // space and the weights divide it exactly.
    el.style.flex = grow + " 1 0";
  }

  function buildSkeleton(el) {
    el.innerHTML = "";
    el.classList.add("mp-panel", "mp-geotable-panel");

    const toolbar = document.createElement("div");
    toolbar.className = "mp-toolbar";
    const title = document.createElement("span");
    title.className = "mp-title";
    const count = document.createElement("span");
    count.className = "mp-count";
    toolbar.appendChild(title);
    toolbar.appendChild(count);
    el.appendChild(toolbar);

    const mapEl = document.createElement("div");
    asSection(mapEl, 55, "260px");
    el.appendChild(mapEl);

    const divider = document.createElement("div");
    divider.style.flex = "none";
    divider.style.height = "1px";
    divider.style.background = "var(--mp-border)";
    el.appendChild(divider);

    const tableEl = document.createElement("div");
    asSection(tableEl, 45, "200px");
    el.appendChild(tableEl);

    const st = { titleEl: title, countEl: count, mapEl, tableEl };
    el.__mpGeoTable = st;
    return st;
  }

  // liveState returns the stashed state only if the DOM it points at is still
  // in the document. The host clears its mount with `innerHTML = ''` in
  // several places (app.js's showPanel, mountScreenView, the error paths)
  // while el.__mp* and el.dataset.mpKind survive on the element itself, and
  // registry.js only drops __mp* when the KIND changes -- so a renderer that
  // trusts its stash can end up appending to a detached skeleton and drawing
  // nothing at all. A renderer that stashes state on an element it does not
  // own should check that the state is still live; this is one comparison per
  // render and removes the whole class of problem.
  function liveState(el) {
    const st = el.__mpGeoTable;
    return st && st.mapEl.isConnected ? st : null;
  }

  function render(el, data) {
    data = data || {};
    const st = liveState(el) || buildSkeleton(el);

    const appName = data.app_name || "";
    st.titleEl.textContent = appName || "Positions";

    const table = data.table || {};
    const rows = Array.isArray(table.rows) ? table.rows : [];
    const markers = (data.map && Array.isArray(data.map.markers)) ? data.map.markers : [];
    const located = countLocated(markers);

    // The one thing neither child renderer can say: the gap between the two.
    // "3 of 5 located" is how an operator sees at a glance that two contacts
    // exist but have not reported a position -- as opposed to two contacts
    // having been dropped.
    st.countEl.textContent = `${located} of ${rows.length} located`;

    // Child renderers own their own state on their own elements; go through
    // renderPanel (not the render functions directly) so they get the same
    // kind-change state clearing every other host gives them.
    window.MayhemPanels.renderPanel("map", st.mapEl, {
      app_name: appName || "Map",
      markers,
    });

    // table.js titles itself from `title`; the geotable contract carries the
    // name once, at the top level. Anything the backend did put on the table
    // sub-object still wins.
    window.MayhemPanels.renderPanel("table", st.tableEl, Object.assign(
      { title: appName ? `${appName} — entries` : "Entries" },
      table,
    ));
  }

  window.MayhemPanels.registerPanel("geotable", render);
})();
