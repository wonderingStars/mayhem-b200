// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// The "table" panel -- ADS-B, AIS, POCSAG, TPMS, ERT, APRS, and every other
// app that fills a ui::RecentEntriesTable. Data shape is documented in
// ../../../PANELS.md, which also covers the flatter shape the C++ backend
// actually sends and normalize() (below) accepts.
//
// State lives on the container element as el.__mpTable and is rebuilt only
// when the *set* of columns changes (a genuinely different table shape);
// otherwise every render() call diffs the incoming rows against what's
// already in the DOM (keyed by row.key) and patches only what changed --
// that's what keeps this smooth at hundreds of rows without a virtual list.

(() => {
  "use strict";

  const { trackAge } = window.MayhemPanels.util;

  // The C++ backend's generic table panel (src/remote/app_data.cpp's
  // to_json(TableData), which every src/remote/provider_*.cpp that publishes
  // a PanelKind::Table goes through) emits the flat shape app.js's original
  // built-in renderer was written against:
  //
  //     { "columns": ["MMSI", "Name/Call"], "rows": [["232003859", "SEA SPIRIT"]] }
  //
  // -- column labels as bare strings, rows as positional arrays, no per-row
  // key and no timestamp. That is NOT the shape documented in PANELS.md, and
  // fed to this renderer verbatim it produced a table with blank headers and
  // every cell empty (the cells are looked up by `col.key`, which does not
  // exist on a string). The portal passes `data` through untouched
  // (client.Panel.Data is json.RawMessage), so nothing between the two ever
  // reconciled them.
  //
  // Rather than have the renderer only work against fixtures, normalize the
  // flat shape here, in the same spirit as rowKey() below already tolerating
  // a keyless backend. Data that already conforms to PANELS.md is returned
  // untouched, so this costs the documented shape nothing.
  //
  // Two things genuinely are not in the flat shape and are NOT invented:
  //
  //   - Row identity. There is no stable key, so rows are keyed positionally
  //     (the same fallback rowKey() uses). Using the first cell as a key
  //     instead would collapse two rows that happen to share it -- the app's
  //     own screen shows both -- so position it is. The cost is that the
  //     new-row/updated-row highlights key on position rather than identity.
  //   - Age. There is no updated_at_ms, and createRow()'s `|| Date.now()`
  //     fallback would otherwise report every row as "now" forever, however
  //     long ago it was actually heard. An unknown age is not "now", so the
  //     Age column is dropped entirely for this shape rather than filled with
  //     a value that reads as real.
  function isFlatShape(columns) {
    return columns.length > 0 && columns.every((c) => typeof c === "string");
  }

  function normalize(data) {
    const columns = Array.isArray(data.columns) ? data.columns : [];
    const rows = Array.isArray(data.rows) ? data.rows : [];
    if (!isFlatShape(columns)) return { data, columns, rows, showAge: true };

    // Keys are synthetic and never displayed; they only have to be unique
    // and stable for as long as the column set is.
    const cols = columns.map((label, i) => ({ key: "c" + i, label: String(label), width: 0 }));
    const mapped = rows.map((row) => {
      const cells = {};
      const values = Array.isArray(row) ? row : [row];
      cols.forEach((col, i) => {
        cells[col.key] = values[i] === undefined || values[i] === null ? "" : String(values[i]);
      });
      return { cells };
    });
    return { data, columns: cols, rows: mapped, showAge: false };
  }

  function columnsSignature(columns, showAge) {
    const cols = columns
      .map((c) => [c.key, c.width || 0, c.align || "", !!c.mono].join(":"))
      .join("|");
    return cols + "#age:" + (showAge ? "1" : "0");
  }

  function parseNumeric(text) {
    const m = String(text).match(/-?[\d,]*\.?\d+/);
    if (!m) return NaN;
    return parseFloat(m[0].replace(/,/g, ""));
  }

  function compareValues(a, b) {
    const na = parseNumeric(a);
    const nb = parseNumeric(b);
    if (Number.isFinite(na) && Number.isFinite(nb)) return na - nb;
    return String(a).localeCompare(String(b));
  }

  function sortRows(rows, key, dir) {
    if (!key) return rows;
    const copy = rows.slice();
    copy.sort((ra, rb) => {
      const va = (ra.cells && ra.cells[key]) || "";
      const vb = (rb.cells && rb.cells[key]) || "";
      const cmp = compareValues(va, vb);
      return dir < 0 ? -cmp : cmp;
    });
    return copy;
  }

  function buildSkeleton(el, columns, showAge) {
    el.innerHTML = "";
    el.classList.add("mp-panel", "mp-table-panel");
    el.style.position = "relative";

    const toolbar = document.createElement("div");
    toolbar.className = "mp-toolbar";
    const title = document.createElement("span");
    title.className = "mp-title";
    const count = document.createElement("span");
    count.className = "mp-count";
    toolbar.appendChild(title);
    toolbar.appendChild(count);
    el.appendChild(toolbar);

    const scroll = document.createElement("div");
    scroll.className = "mp-table-scroll";
    const table = document.createElement("table");
    table.className = "mp-table";

    const colgroup = document.createElement("colgroup");
    let flexAssigned = false;
    columns.forEach((col) => {
      const width = col.width || 0;
      const colEl = document.createElement("col");
      if (width > 0) {
        colEl.style.width = width + "ch";
      } else if (!flexAssigned) {
        flexAssigned = true; // the first zero-width column absorbs remaining space, unstyled
      } else {
        // A second (or later) zero-width column: RecentEntriesColumns only
        // ever gives the *first* such column the remaining space; give this
        // one just enough room for its own label rather than leaving it to
        // fight the first for space.
        colEl.style.width = Math.max(8, (col.label || "").length + 2) + "ch";
      }
      colgroup.appendChild(colEl);
    });
    if (showAge) {
      const ageCol = document.createElement("col");
      ageCol.style.width = "7ch";
      colgroup.appendChild(ageCol);
    }
    table.appendChild(colgroup);

    const thead = document.createElement("thead");
    const headRow = document.createElement("tr");
    columns.forEach((col) => {
      const th = document.createElement("th");
      th.dataset.key = col.key;
      if (col.align === "right") th.classList.add("mp-align-right");
      if (col.align === "center") th.classList.add("mp-align-center");
      const label = document.createElement("span");
      label.textContent = col.label || col.key;
      const arrow = document.createElement("span");
      arrow.className = "mp-sort-arrow";
      th.appendChild(label);
      th.appendChild(arrow);
      th.addEventListener("click", () => onHeaderClick(el, col.key));
      headRow.appendChild(th);
    });
    if (showAge) {
      const ageHeader = document.createElement("th");
      ageHeader.className = "mp-align-right";
      ageHeader.textContent = "Age";
      ageHeader.style.cursor = "default";
      headRow.appendChild(ageHeader);
    }
    thead.appendChild(headRow);
    table.appendChild(thead);

    const tbody = document.createElement("tbody");
    table.appendChild(tbody);
    scroll.appendChild(table);
    el.appendChild(scroll);

    const detail = document.createElement("div");
    detail.className = "mp-table-detail mp-hidden";
    el.appendChild(detail);

    el.__mpTable = {
      columnsSig: columnsSignature(columns, showAge),
      columns,
      showAge,
      titleEl: title,
      countEl: count,
      tbody,
      detailEl: detail,
      headerCells: Array.from(headRow.querySelectorAll("th[data-key]")),
      rowsByKey: new Map(),
      sortKey: null,
      sortDir: 1,
      selectedKey: null,
      lastData: null,
    };
  }

  function onHeaderClick(el, key) {
    const st = el.__mpTable;
    if (!st) return;
    if (st.sortKey === key) {
      st.sortDir = st.sortDir === 1 ? -1 : (st.sortDir === -1 ? 0 : 1);
      if (st.sortDir === 0) st.sortKey = null;
    } else {
      st.sortKey = key;
      st.sortDir = 1;
    }
    updateSortArrows(st);
    if (st.lastData) applyRows(el, st, st.lastData);
  }

  function updateSortArrows(st) {
    st.headerCells.forEach((th) => {
      const arrow = th.querySelector(".mp-sort-arrow");
      if (th.dataset.key === st.sortKey) {
        arrow.textContent = st.sortDir < 0 ? "▼" : "▲";
      } else {
        arrow.textContent = "";
      }
    });
  }

  function cellText(row, key) {
    const v = row.cells && row.cells[key];
    return v === undefined || v === null ? "" : String(v);
  }

  // rowKey defends against data that doesn't conform to the documented
  // contract (PANELS.md: every row has a stable `key`) -- e.g. a
  // placeholder/mock backend still speaking an older, keyless shape. Without
  // this, rows with no `.key` all collide on the same `undefined` map entry
  // and silently collapse into one; a positional fallback at least keeps
  // each row distinct and the table state internally consistent, even
  // though a keyless backend won't get correct identity-based diffing
  // (new/updated-row detection) until it sends real keys.
  function rowKey(row, index) {
    return row && row.key !== undefined && row.key !== null ? row.key : `__row${index}`;
  }

  function buildDetail(st, row) {
    st.detailEl.innerHTML = "";
    st.columns.forEach((col) => {
      const k = document.createElement("div");
      k.className = "mp-detail-key";
      k.textContent = col.label || col.key;
      const v = document.createElement("div");
      v.className = "mp-detail-val";
      v.textContent = cellText(row, col.key);
      st.detailEl.appendChild(k);
      st.detailEl.appendChild(v);
    });
    if (row.updated_at_ms) {
      const k = document.createElement("div");
      k.className = "mp-detail-key";
      k.textContent = "Updated";
      const v = document.createElement("div");
      v.className = "mp-detail-val";
      v.textContent = new Date(row.updated_at_ms).toLocaleTimeString();
      st.detailEl.appendChild(k);
      st.detailEl.appendChild(v);
    }
    st.detailEl.classList.remove("mp-hidden");
  }

  function onRowClick(el, row, key) {
    const st = el.__mpTable;
    if (!st) return;
    const entry = st.rowsByKey.get(key);
    if (st.selectedKey === key) {
      st.selectedKey = null;
      if (entry) entry.tr.classList.remove("mp-row-selected");
      st.detailEl.classList.add("mp-hidden");
      return;
    }
    if (st.selectedKey) {
      const prev = st.rowsByKey.get(st.selectedKey);
      if (prev) prev.tr.classList.remove("mp-row-selected");
    }
    st.selectedKey = key;
    if (entry) entry.tr.classList.add("mp-row-selected");
    buildDetail(st, row);
  }

  function flash(tr, className) {
    tr.classList.add(className);
    tr.addEventListener("animationend", () => tr.classList.remove(className), { once: true });
  }

  function createRow(el, st, row, key) {
    const tr = document.createElement("tr");
    tr.addEventListener("click", () => onRowClick(el, row, key));
    const cellEls = {};
    st.columns.forEach((col) => {
      const td = document.createElement("td");
      if (col.mono) td.classList.add("mp-mono");
      if (col.align === "right") td.classList.add("mp-align-right");
      if (col.align === "center") td.classList.add("mp-align-center");
      td.textContent = cellText(row, col.key);
      tr.appendChild(td);
      cellEls[col.key] = td;
    });
    let ageTd = null;
    let tsMs = 0;
    let ageHandle = { update: () => {}, stop: () => {} };
    if (st.showAge) {
      ageTd = document.createElement("td");
      ageTd.className = "mp-age";
      tr.appendChild(ageTd);
      tsMs = row.updated_at_ms || Date.now();
      ageHandle = trackAge(ageTd, tsMs);
    }

    const entry = {
      tr,
      cellEls,
      ageTd,
      tsMs,
      ageHandle,
    };
    st.rowsByKey.set(key, entry);
    if (st.selectedKey === key) tr.classList.add("mp-row-selected");
    return entry;
  }

  function reorderRows(tbody, order, rowsByKey) {
    let anchor = tbody.firstChild;
    for (const key of order) {
      const tr = rowsByKey.get(key).tr;
      if (anchor === tr) {
        anchor = anchor.nextSibling;
      } else {
        tbody.insertBefore(tr, anchor);
      }
    }
  }

  function applyRows(el, st, data) {
    const rows = data.rows || [];
    const displayRows = sortRows(rows, st.sortKey, st.sortDir);
    const displayKeys = displayRows.map((r, i) => rowKey(r, i));
    const newKeys = new Set(displayKeys);

    // Drop rows no longer present.
    for (const [key, entry] of Array.from(st.rowsByKey.entries())) {
      if (!newKeys.has(key)) {
        entry.ageHandle.stop();
        entry.tr.remove();
        st.rowsByKey.delete(key);
        if (st.selectedKey === key) {
          st.selectedKey = null;
          st.detailEl.classList.add("mp-hidden");
        }
      }
    }

    displayRows.forEach((row, i) => {
      const key = displayKeys[i];
      let entry = st.rowsByKey.get(key);
      if (!entry) {
        entry = createRow(el, st, row, key);
        st.tbody.appendChild(entry.tr);
        flash(entry.tr, "mp-row-new");
        return;
      }
      let changed = false;
      st.columns.forEach((col) => {
        const text = cellText(row, col.key);
        const td = entry.cellEls[col.key];
        if (td.textContent !== text) {
          td.textContent = text;
          changed = true;
        }
      });
      const tsMs = row.updated_at_ms || entry.tsMs;
      if (tsMs !== entry.tsMs) {
        entry.tsMs = tsMs;
        entry.ageHandle.update(tsMs);
        if (changed) flash(entry.tr, "mp-row-updated");
      }
      if (st.selectedKey === key) buildDetail(st, row);
    });

    reorderRows(st.tbody, displayKeys, st.rowsByKey);

    if (displayRows.length === 0) {
      if (!st.emptyRow || !st.emptyRow.isConnected) {
        const tr = document.createElement("tr");
        const td = document.createElement("td");
        td.colSpan = st.columns.length + (st.showAge ? 1 : 0);
        td.className = "mp-empty";
        td.textContent = "No entries yet.";
        tr.appendChild(td);
        st.tbody.appendChild(tr);
        st.emptyRow = tr;
      }
    } else if (st.emptyRow) {
      st.emptyRow.remove();
      st.emptyRow = null;
    }
  }

  function render(el, rawData) {
    const data = rawData || {};
    /* Everything below this line works on the normalized view; `data` is only
     * consulted for the fields both shapes carry verbatim (title, row_count,
     * row_limit). See normalize()'s comment for what the flat shape costs. */
    const norm = normalize(data);
    const columns = norm.columns;
    const sig = columnsSignature(columns, norm.showAge);

    if (!el.__mpTable || el.__mpTable.columnsSig !== sig) {
      buildSkeleton(el, columns, norm.showAge);
    }
    const st = el.__mpTable;
    st.lastData = { rows: norm.rows };

    st.titleEl.textContent = data.title || "Table";
    const rowCount = data.row_count != null ? data.row_count : norm.rows.length;
    let countText = `${rowCount} ${rowCount === 1 ? "entry" : "entries"}`;
    st.countEl.innerHTML = "";
    st.countEl.appendChild(document.createTextNode(countText));
    if (data.row_limit && rowCount >= data.row_limit) {
      const capped = document.createElement("span");
      capped.className = "mp-capped";
      capped.textContent = `capped at ${data.row_limit}`;
      st.countEl.appendChild(capped);
    }

    applyRows(el, st, st.lastData);
  }

  window.MayhemPanels.registerPanel("table", render);
})();
