// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// The "console" panel: a scrolling monospace log with colour for the
// STR_COLOR_* escapes the C++ side already writes (src/ui/ui.hpp: 0x1B
// followed by one byte, 0x00-0x0F indexing term_colors[16] in
// src/ui/ui.cpp, 0x10 = STR_COLOR_FOREGROUND meaning "reset to default").
// The RGB values below are copied from ui.hpp's Color:: factory functions,
// not guessed -- e.g. dark_red() is {159,0,0}, not a generic "dark red".
// Data shape is documented in ../../../PANELS.md.

(() => {
  "use strict";

  // Index must match STR_COLOR_* in src/ui/ui.hpp / term_colors[] in
  // src/ui/ui.cpp exactly.
  const TERM_COLORS = [
    [0, 0, 0],       // STR_COLOR_BLACK
    [0, 0, 191],     // STR_COLOR_DARK_BLUE
    [0, 159, 0],     // STR_COLOR_DARK_GREEN
    [0, 191, 191],   // STR_COLOR_DARK_CYAN
    [159, 0, 0],     // STR_COLOR_DARK_RED
    [191, 0, 191],   // STR_COLOR_DARK_MAGENTA
    [191, 191, 0],   // STR_COLOR_DARK_YELLOW
    [191, 191, 191], // STR_COLOR_LIGHT_GREY
    [63, 63, 63],    // STR_COLOR_DARK_GREY
    [0, 0, 255],     // STR_COLOR_BLUE
    [0, 255, 0],     // STR_COLOR_GREEN
    [0, 255, 255],   // STR_COLOR_CYAN
    [255, 0, 0],     // STR_COLOR_RED
    [255, 0, 255],   // STR_COLOR_MAGENTA
    [255, 255, 0],   // STR_COLOR_YELLOW
    [255, 255, 255], // STR_COLOR_WHITE
  ];
  const FOREGROUND_RESET = 0x10;

  // parseColorRuns splits `text` on \x1B<byte> escapes into runs of
  // {text, rgb|null}. rgb is null for the default/reset colour.
  function parseColorRuns(text) {
    const runs = [];
    let color = null;
    let buf = "";
    for (let i = 0; i < text.length; i++) {
      if (text.charCodeAt(i) === 0x1b && i + 1 < text.length) {
        const idx = text.charCodeAt(i + 1);
        if (idx < TERM_COLORS.length || idx === FOREGROUND_RESET) {
          if (buf) runs.push({ text: buf, rgb: color });
          buf = "";
          color = idx === FOREGROUND_RESET ? null : TERM_COLORS[idx];
          i++; // consume the colour-index byte too
          continue;
        }
      }
      buf += text[i];
    }
    if (buf) runs.push({ text: buf, rgb: color });
    return runs;
  }

  function isNearBottom(scroll) {
    return scroll.scrollHeight - scroll.scrollTop - scroll.clientHeight < 32;
  }

  function buildSkeleton(el) {
    el.innerHTML = "";
    el.classList.add("mp-panel", "mp-console-panel");
    el.style.position = "relative";

    const toolbar = document.createElement("div");
    toolbar.className = "mp-toolbar";
    const title = document.createElement("span");
    title.className = "mp-title";
    const count = document.createElement("span");
    count.className = "mp-count";
    const autoLabel = document.createElement("label");
    autoLabel.className = "mp-field-inline";
    const autoBox = document.createElement("input");
    autoBox.type = "checkbox";
    autoBox.checked = true;
    autoLabel.appendChild(autoBox);
    autoLabel.appendChild(document.createTextNode("Autoscroll"));
    toolbar.appendChild(title);
    toolbar.appendChild(count);
    toolbar.appendChild(autoLabel);
    el.appendChild(toolbar);

    const scroll = document.createElement("div");
    scroll.className = "mp-console-scroll";
    el.appendChild(scroll);

    const pill = document.createElement("div");
    pill.className = "mp-jump-pill";
    pill.textContent = "Jump to latest ↓";
    el.appendChild(pill);

    const st = {
      titleEl: title,
      countEl: count,
      autoBox,
      scroll,
      pill,
      seenSeqs: new Set(),
      lastLineCount: 0,
      autoPaused: false,
      programmaticScroll: false,
    };
    el.__mpConsole = st;

    scroll.addEventListener("scroll", () => {
      if (st.programmaticScroll) return;
      const near = isNearBottom(scroll);
      st.autoPaused = !near;
      st.pill.classList.toggle("mp-visible", st.autoPaused && st.autoBox.checked);
    });

    autoBox.addEventListener("change", () => {
      if (autoBox.checked) {
        st.autoPaused = false;
        scrollToBottom(st);
      }
      st.pill.classList.toggle("mp-visible", st.autoPaused && autoBox.checked);
    });

    pill.addEventListener("click", () => {
      st.autoPaused = false;
      scrollToBottom(st);
      st.pill.classList.remove("mp-visible");
    });

    return st;
  }

  function scrollToBottom(st) {
    st.programmaticScroll = true;
    st.scroll.scrollTop = st.scroll.scrollHeight;
    // Let the resulting scroll event (if any) land before un-guarding.
    requestAnimationFrame(() => { st.programmaticScroll = false; });
  }

  function appendLine(st, line) {
    const div = document.createElement("div");
    div.className = "mp-console-line";
    if (line.ts_ms) {
      const ts = document.createElement("span");
      ts.className = "mp-console-ts";
      ts.textContent = new Date(line.ts_ms).toLocaleTimeString([], { hour12: false });
      div.appendChild(ts);
    }
    const runs = parseColorRuns(line.text || "");
    if (runs.length === 0) {
      div.appendChild(document.createTextNode(""));
    }
    runs.forEach((run) => {
      const span = document.createElement("span");
      if (run.rgb) span.style.color = `rgb(${run.rgb[0]},${run.rgb[1]},${run.rgb[2]})`;
      span.textContent = run.text;
      div.appendChild(span);
    });
    st.scroll.appendChild(div);
  }

  function render(el, data) {
    data = data || {};
    const st = el.__mpConsole || buildSkeleton(el);

    st.titleEl.textContent = data.app_name || "Console";
    const lines = data.lines || [];
    const maxLines = data.max_lines || 1000;

    let appended = 0;

    lines.forEach((line, idx) => {
      if (line.seq !== undefined) {
        const key = `seq:${line.seq}`;
        if (st.seenSeqs.has(key)) return;
        st.seenSeqs.add(key);
        appendLine(st, line);
        appended++;
      } else if (idx >= st.lastLineCount) {
        appendLine(st, line);
        appended++;
      }
    });
    st.lastLineCount = lines.length;
    // Defensive bound: a session with no `seq` field running for a very long
    // time would otherwise grow this set forever; it only exists for
    // dedup, so it's safe to drop once it's well past anything max_lines
    // would keep on screen anyway.
    if (st.seenSeqs.size > maxLines * 4) st.seenSeqs.clear();

    while (st.scroll.childElementCount > maxLines) {
      st.scroll.removeChild(st.scroll.firstChild);
    }

    st.countEl.textContent = `${st.scroll.childElementCount} lines`;

    if (appended > 0 && st.autoBox.checked && !st.autoPaused) {
      scrollToBottom(st);
    } else if (appended > 0 && st.autoPaused) {
      st.pill.classList.toggle("mp-visible", st.autoBox.checked);
    }
  }

  window.MayhemPanels.registerPanel("console", render);
})();
