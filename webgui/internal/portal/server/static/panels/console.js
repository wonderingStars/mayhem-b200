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

  // ---- legibility floor ----------------------------------------------------
  //
  // TERM_COLORS above is a mirror of the device's palette and must stay
  // byte-for-byte what src/ui/ui.hpp says. What it is painted ON, however, is
  // this portal's choice: the console well is --mp-bg-deep (#0b0a08), and six
  // of the sixteen device colours are illegible on it. Measured against that
  // well: BLACK 1.06:1 (invisible), DARK_BLUE 1.65, DARK_GREY 1.88, BLUE 2.30,
  // DARK_RED 2.33, DARK_MAGENTA 3.74 -- all under the 4.5:1 WCAG AA asks of
  // body text, and console output is this panel's body text.
  //
  // So the table stays exact and the *rendering* enforces a floor: lift a run's
  // colour just far enough to clear MIN_CONTRAST, and no further. Two steps, in
  // this order, because the first preserves hue exactly and the second does
  // not:
  //
  //   1. scale all three channels by one factor -- same hue, same saturation,
  //      brighter -- stopping at the smallest factor that clears the floor.
  //   2. only if that still falls short, blend towards white. A saturated blue
  //      on near-black cannot reach 4.5:1 at any brightness (pure #0000ff is
  //      2.30:1 here), so the choice is a desaturated blue or an unreadable one.
  //
  // A colour that already passes is returned untouched, which is ten of the
  // sixteen. The other six land at 4.63-4.94:1, and the only pair that ends up
  // sharing a value is DARK_BLUE/BLUE -- neither of which any app in src/
  // actually emits, and both of which are already at the brightest a blue can
  // be. In practice src/ emits nine of the sixteen and DARK_GREY was the only
  // failing one, at 1.88:1: pocsag_app.cpp writes "No radio attached: decode
  // is untested on air." in it, which is a real warning nobody could read.
  //
  // The result is cached per colour: the arithmetic is trivial, but it would
  // otherwise run per coloured run per line on a panel that appends forever.
  const MIN_CONTRAST = 4.5;

  // WCAG relative luminance. srgb components are 0..255.
  function luminance(rgb) {
    const c = rgb.map((v) => {
      const s = v / 255;
      return s <= 0.04045 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4);
    });
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
  }

  function contrast(a, b) {
    const la = luminance(a), lb = luminance(b);
    const hi = Math.max(la, lb), lo = Math.min(la, lb);
    return (hi + 0.05) / (lo + 0.05);
  }

  // The well's own colour, read from the token rather than repeated here, so
  // re-theming the panel re-derives the floor instead of silently invalidating
  // it. Falls back to the current value of --mp-bg-deep if the property is
  // missing (a bare page with no stylesheet, i.e. a unit-test harness).
  function wellRgb() {
    const raw = getComputedStyle(document.documentElement)
      .getPropertyValue("--mp-bg-deep").trim();
    const m = /^#([0-9a-f]{6})$/i.exec(raw);
    if (!m) return [11, 10, 8];
    const n = parseInt(m[1], 16);
    return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
  }

  let legibleCache = null;

  function legibleColor(rgb) {
    if (legibleCache === null) legibleCache = new Map();
    const key = rgb[0] * 65536 + rgb[1] * 256 + rgb[2];
    const hit = legibleCache.get(key);
    if (hit) return hit;

    const bg = wellRgb();
    let out = rgb;
    if (contrast(out, bg) < MIN_CONTRAST) {
      // Step 1: brighten in place, by the SMALLEST factor that clears the
      // floor rather than all the way to the clip point. Stopping early is
      // what keeps DARK_RED from landing exactly on RED and DARK_GREY on
      // WHITE -- two device colours that render identically are a different
      // bug from an invisible one, not a fix for it.
      const peak = Math.max(rgb[0], rgb[1], rgb[2]);
      if (peak > 0) {
        const maxScale = 255 / peak; // beyond this a channel would clip
        for (let s = 1.05; s < maxScale && contrast(out, bg) < MIN_CONTRAST; s += 0.05) {
          out = rgb.map((v) => Math.round(v * s));
        }
        if (contrast(out, bg) < MIN_CONTRAST) out = rgb.map((v) => Math.round(v * maxScale));
      }

      // Step 2: blend that towards white until it clears, 5% at a time.
      // Terminates by construction -- white is 19.8:1 on this theme's darkest
      // surface -- but the loop bound says so without relying on it.
      const lifted = out;
      for (let t = 0.05; t <= 1.0001 && contrast(out, bg) < MIN_CONTRAST; t += 0.05) {
        out = lifted.map((v) => Math.round(v + (255 - v) * t));
      }
    }
    const css = `rgb(${out[0]},${out[1]},${out[2]})`;
    legibleCache.set(key, css);
    return css;
  }

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

  // --- the C++ backend's flat shape -------------------------------------------
  //
  // src/remote/app_data.cpp's to_json(ConsoleData) -- the single funnel every
  // provider publishing a PanelKind::Console goes through -- emits
  //
  //     { "lines": ["sync acquired", "POCSAG512 addr=1001001 ..."] }
  //
  // bare strings, not the {seq, ts_ms, text} objects the rest of this file and
  // PANELS.md are written against. Fed here verbatim, `line.text` was
  // undefined on every line, so the panel drew the right NUMBER of rows,
  // every one of them empty, and reported them in the line counter as if they
  // were real content. Normalize instead, exactly as table.js does for the
  // same class of mismatch on its own kind.
  //
  // What is NOT invented, because it is not in the flat shape:
  //
  //   - Timestamps. There is no ts_ms and none is synthesized. appendLine
  //     only draws the time column `if (line.ts_ms)`, so leaving the field
  //     absent correctly suppresses it rather than stamping every line with
  //     the moment the browser happened to poll. A line's real age is
  //     unknown; "now" is a lie, and it is the exact lie the table panel's
  //     Age column had to have removed.
  function isFlatShape(lines) {
    return lines.length > 0 && lines.every((l) => typeof l === "string");
  }

  function normalize(data) {
    const lines = Array.isArray(data.lines) ? data.lines : [];
    if (!isFlatShape(lines)) return { lines, flat: false };
    return { lines: lines.map((text, index) => ({ seq: index, text: String(text) })), flat: true };
  }

  // appendCountFor works out how many of `next` are new, given `prev` (the
  // texts already on screen), for the flat shape only.
  //
  // `seq` above is the array index, which is a POSITION, not an identity:
  // ui::Console keeps a std::deque bounded by max_lines (256 by default) and
  // pops the front once it is full (src/ui/ui_widget.cpp: Console::write), so
  // after the buffer wraps, index 0 is a different line than it was last
  // poll. Deduping by that seq -- the natural reading of "append by seq" --
  // would therefore suppress every genuinely new line forever from the 257th
  // one onward, silently, on exactly the long-running decode session this
  // panel exists for. So the flat shape is diffed by content: find the
  // longest tail of what is already shown that matches the head of the new
  // buffer, and append only what follows it. Ambiguity (repeated identical
  // lines) resolves toward the longest match, i.e. toward appending less,
  // which duplicates nothing.
  function appendCountFor(prev, next) {
    const max = Math.min(prev.length, next.length);
    for (let k = max; k > 0; k--) {
      let match = true;
      for (let i = 0; i < k; i++) {
        if (prev[prev.length - k + i] !== next[i]) { match = false; break; }
      }
      if (match) return next.length - k;
    }
    return next.length;
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
      flatShown: [],
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
      if (run.rgb) span.style.color = legibleColor(run.rgb);
      span.textContent = run.text;
      div.appendChild(span);
    });
    st.scroll.appendChild(div);
  }

  function render(el, data) {
    data = data || {};
    const st = el.__mpConsole || buildSkeleton(el);

    st.titleEl.textContent = data.app_name || "Console";
    const norm = normalize(data);
    const lines = norm.lines;
    const maxLines = data.max_lines || 1000;

    let appended = 0;

    if (norm.flat) {
      // Positional seq -- diff by content instead. See appendCountFor.
      const texts = lines.map((l) => l.text);
      const count = appendCountFor(st.flatShown, texts);
      for (let i = lines.length - count; i < lines.length; i++) {
        appendLine(st, lines[i]);
        appended++;
      }
      st.flatShown = texts;
    } else {
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
    }

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
