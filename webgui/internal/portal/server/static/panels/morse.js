// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// The "morse" panel: the CW app's two honest halves in one card --- a live RX
// decoder read-out on top, and an interactive text-to-Morse transmitter below.
// Data shape is documented in ../../../PANELS.md under `morse`.
//
// RECEIVE. Every field it draws was produced by the decoder on the C++ side
// (decoded_text, an estimated wpm, a locked tone in Hz, and a receiving flag)
// and is published verbatim. This panel derives nothing of its own on the RX
// side, and it obeys the same absent-stays-absent rule the ADS-B and AIS panels
// are built around: a decoder that has formed no wpm estimate yet sends no
// `wpm` field, and one with no tone lock sends no `tone_hz` --- so those
// read-outs are HIDDEN, never shown as "0 WPM" or "0 Hz". `decoded_text` may
// legitimately be "" (nothing heard yet); that draws an honest "listening"
// state, not fabricated characters. A JSON `null` is treated as absent, never
// coerced through Number() to a fake zero.
//
// TRANSMIT. This is the first interactive native panel that can key the radio,
// so its whole design is about not doing that by accident:
//
//   - The live dot/dash translation, and the local "Play" sidetone (Web Audio,
//     gated at the standard CW timing: dot 1 unit, dash 3, intra-char gap 1,
//     inter-char 3, word 7, unit_ms = 1200/wpm), key NOTHING. They exist so an
//     operator can see and hear their message before it goes anywhere near an
//     antenna.
//   - The one control that radiates RF --- "Transmit" --- POSTs to the backend
//     and is disabled, with the reason shown in words, unless GET /api/status
//     reports `can_transmit: true`. A receive-only radio (an RTL-SDR) or a
//     state where nothing has reported the capability leaves the button off.
//     The portal shell's own deviceCanTransmit() lives inside app.js's closure
//     and is not reachable from here, so this panel reads /api/status itself
//     and caches it briefly (the shell is already polling the same endpoint;
//     this is one extra light request every few seconds while the panel is up).
//   - A persistent, unmissable licensing warning sits next to that button.
//     Transmitting CW over the air needs a licence almost everywhere and is the
//     operator's responsibility; the panel says so and does not let the point
//     scroll away.
//   - Nothing auto-transmits. Enter in the text field does not key the radio;
//     only a deliberate click on Transmit does. While a transmit is in flight
//     the button is disabled and reads "Transmitting..."; on success it reports
//     the keyed duration; on failure it shows the backend's error string
//     verbatim.
//
// Per PANELS.md the whole contract is render(el, data): no mount/update split
// and no destroy hook, so all per-instance state hangs off el.__mpMorse and the
// skeleton is built exactly once. Styles are injected from here (one <style>
// appended to document.head, once) for the same reason the sibling panels do
// it: the panel is self-contained so the harness can exercise it without the
// shared stylesheet, and the classes are all mp-morse-* so nothing collides
// with the shell or another panel sharing the reused mount.

(() => {
  "use strict";

  // No shared util is needed: every dynamic string reaches the DOM through
  // textContent or a .title property, never string-built innerHTML, so there is
  // nothing to escape. (registry.js's util is still required to exist for the
  // registerPanel call at the foot of this file.)

  // ---------------------------------------------------------------------------
  // International Morse code (ITU-R M.1677). Letters, digits and the punctuation
  // the app's keyer accepts: . , ? / = + --- plus space as a word gap, handled
  // structurally rather than as a table entry. A character not in this table is
  // NOT silently dropped: it renders as a "·?" marker so the operator sees that
  // something in their text has no Morse and will not be sent, rather than
  // discovering it by a hole in what goes on the air.
  // ---------------------------------------------------------------------------

  const MORSE = {
    A: ".-", B: "-...", C: "-.-.", D: "-..", E: ".", F: "..-.", G: "--.",
    H: "....", I: "..", J: ".---", K: "-.-", L: ".-..", M: "--", N: "-.",
    O: "---", P: ".--.", Q: "--.-", R: ".-.", S: "...", T: "-", U: "..-",
    V: "...-", W: ".--", X: "-..-", Y: "-.--", Z: "--..",
    "0": "-----", "1": ".----", "2": "..---", "3": "...--", "4": "....-",
    "5": ".....", "6": "-....", "7": "--...", "8": "---..", "9": "----.",
    ".": ".-.-.-", ",": "--..--", "?": "..--..", "/": "-..-.",
    "=": "-...-", "+": ".-.-.",
  };

  const UNKNOWN_MARK = "·?"; // "·?"

  // Web Audio local sidetone. 600 Hz is a conventional CW monitor pitch; this
  // is a preview beep only and is entirely independent of the decoder's
  // detected tone_hz --- it never keys anything.
  const SIDETONE_HZ = 600;
  const SIDETONE_GAIN = 0.18;
  const RAMP_S = 0.005; // 5 ms on/off ramp, so gating a bare oscillator doesn't click

  const WPM_MIN = 5;
  const WPM_MAX = 40;
  const WPM_DEFAULT = 18;

  // How long a cached /api/status answer is trusted before the panel asks
  // again. render() runs on every poll (~700 ms); this keeps the capability
  // probe to a few requests per minute rather than one per render.
  const STATUS_TTL_MS = 4000;

  // ---------------------------------------------------------------------------
  // Pure Morse helpers (also exported for the node fixture/unit tests via the
  // browser-inert module.exports seam at the bottom of this file).
  // ---------------------------------------------------------------------------

  function unitMs(wpm) {
    return 1200 / wpm;
  }

  function clampWpm(v) {
    const n = Math.round(Number(v));
    if (!Number.isFinite(n)) return WPM_DEFAULT;
    return Math.min(WPM_MAX, Math.max(WPM_MIN, n));
  }

  // morseTokens turns text into a flat, typed token stream the preview renders
  // directly: {type:"sym", ch, code} for a known character, {type:"unknown",
  // ch} for one with no Morse, {type:"word"} for a word gap. Leading/trailing
  // word gaps are trimmed so a stray space does not draw a dangling separator.
  function morseTokens(text) {
    const out = [];
    for (const raw of String(text)) {
      if (raw === " " || raw === "\t" || raw === "\n") {
        out.push({ type: "word" });
        continue;
      }
      const ch = raw.toUpperCase();
      const code = MORSE[ch];
      if (code) out.push({ type: "sym", ch, code });
      else out.push({ type: "unknown", ch: raw });
    }
    while (out.length && out[0].type === "word") out.shift();
    while (out.length && out[out.length - 1].type === "word") out.pop();
    // Collapse runs of word gaps (double spaces) to a single gap.
    return out.filter((t, i) => !(t.type === "word" && out[i - 1] && out[i - 1].type === "word"));
  }

  // textToMorse is the string form of the same tokenisation: codes separated by
  // a single space, words by " / ", unknown characters as the "·?" marker.
  function textToMorse(text) {
    return morseTokens(text)
      .map((t) => (t.type === "word" ? "/" : t.type === "unknown" ? UNKNOWN_MARK : t.code))
      .join(" ");
  }

  // morseSchedule turns text into an on/off gate list in milliseconds at the
  // given unit, applying the standard CW spacing. Unknown characters produce no
  // tone (you cannot key what has no Morse); they are simply skipped for audio,
  // exactly as they are marked but not sent.
  function morseSchedule(text, unit) {
    const seg = [];
    const words = String(text).toUpperCase().split(/\s+/).filter((w) => w.length);
    words.forEach((word, wi) => {
      if (wi > 0) seg.push({ on: false, dur: 7 * unit });
      const codes = word.split("").map((c) => MORSE[c]).filter(Boolean);
      codes.forEach((code, ci) => {
        if (ci > 0) seg.push({ on: false, dur: 3 * unit });
        for (let i = 0; i < code.length; i++) {
          if (i > 0) seg.push({ on: false, dur: unit });
          seg.push({ on: true, dur: (code[i] === "-" ? 3 : 1) * unit });
        }
      });
    });
    return seg;
  }

  function scheduleTotalMs(text, unit) {
    return morseSchedule(text, unit).reduce((a, s) => a + s.dur, 0);
  }

  // ---------------------------------------------------------------------------
  // Styles
  // ---------------------------------------------------------------------------

  const STYLE_ID = "mp-morse-style";
  const CSS = `
/* Layout hangs off .mp-morse-root, not the panel root: kind classes are sticky
   (app.js reuses one #panelMount and never removes the prior kind's class), so
   a root-level rule would go on distorting whatever is rendered next. The root
   div is this panel's own and the next renderer's innerHTML clear takes it. */
.mp-morse-root { display: flex; flex-direction: column; gap: 8px;
  flex: 1; min-height: 360px; min-width: 0; }

.mp-morse-section { display: flex; flex-direction: column; min-height: 0;
  border: 1px solid var(--mp-line, #242019); border-radius: var(--mp-radius, 6px);
  background: var(--mp-surface, #14120e); overflow: hidden; }
.mp-morse-rx-section { flex: 1; min-height: 120px; }
.mp-morse-tx-section { flex: none; }

.mp-morse-head { display: flex; align-items: center; flex-wrap: wrap; gap: 6px 12px;
  padding: 6px 10px; background: var(--mp-bg-raised, #12100d);
  border-bottom: 1px solid var(--mp-line, #242019); }
.mp-morse-label { font: 10px var(--mp-mono, monospace); letter-spacing: 1.5px;
  text-transform: uppercase; color: var(--mp-text-dim, #a09681); margin-right: auto; }
.mp-morse-readouts { display: flex; align-items: center; gap: 6px 8px; flex-wrap: wrap; }
.mp-morse-readout { font: 11px var(--mp-mono, monospace); color: var(--mp-text, #ede8db);
  font-variant-numeric: tabular-nums; padding: 1px 7px;
  border: 1px solid var(--mp-border, #2b2820); border-radius: 999px;
  background: var(--mp-bg-sunken, #100e0b); white-space: nowrap; }
.mp-morse-chip { font: 10px var(--mp-mono, monospace); letter-spacing: 1px;
  text-transform: uppercase; color: var(--mp-text-dim, #a09681);
  padding: 2px 8px; border: 1px solid var(--mp-border, #2b2820); border-radius: 999px;
  white-space: nowrap; }
.mp-morse-chip.mp-live { color: var(--mp-ok, #3ddc84); border-color: var(--mp-accent-edge, rgba(61,245,140,.6));
  background: var(--mp-accent-wash, rgba(61,245,140,.12)); }

/* The decoded-text well: a scrolling monospace read-out, large enough to read
   at a glance, like the console panel's log area. */
.mp-morse-rx { flex: 1; min-height: 0; overflow: auto; padding: 10px 12px;
  background: var(--mp-bg-deep, #0b0a08); }
.mp-morse-rx-text { font: 20px/1.5 var(--mp-mono, monospace); color: var(--mp-accent, #3df58c);
  white-space: pre-wrap; word-break: break-word; letter-spacing: 1px; }
.mp-morse-empty { font: 12px var(--mp-mono, monospace); color: var(--mp-text-dim, #a09681);
  padding: 6px 0; }

/* Transmit body. */
.mp-morse-tx { display: flex; flex-direction: column; gap: 8px; padding: 10px 12px; }
.mp-morse-row { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.mp-morse-field { display: inline-flex; align-items: center; gap: 6px;
  font: 10px var(--mp-mono, monospace); letter-spacing: 1px; text-transform: uppercase;
  color: var(--mp-text-dim, #a09681); }
.mp-morse-input { flex: 1 1 220px; min-width: 0; height: 28px;
  background: var(--mp-surface, #14120e); color: var(--mp-text, #ede8db);
  border: 1px solid var(--mp-border, #2b2820); border-radius: 5px;
  caret-color: var(--mp-accent, #3df58c);
  font: 13px var(--mp-mono, monospace); text-transform: uppercase; padding: 0 8px; }
.mp-morse-input:focus { border-color: var(--mp-accent-edge, rgba(61,245,140,.6)); outline: none; }
.mp-morse-wpm { width: 68px; height: 28px; background: var(--mp-surface, #14120e);
  color: var(--mp-text, #ede8db); border: 1px solid var(--mp-border, #2b2820);
  border-radius: 5px; font: 12px var(--mp-mono, monospace); padding: 0 6px;
  font-variant-numeric: tabular-nums; }
.mp-morse-wpm:focus { border-color: var(--mp-accent-edge, rgba(61,245,140,.6)); outline: none; }

/* The live translation. Symbols in phosphor, word gaps and the "·?" unknown
   marker distinct so a character with no Morse cannot be mistaken for one. */
.mp-morse-preview { min-height: 34px; padding: 7px 9px; border-radius: 5px;
  background: var(--mp-bg-sunken, #100e0b); border: 1px solid var(--mp-line, #242019);
  display: flex; flex-wrap: wrap; align-items: baseline; gap: 4px 12px;
  font: 15px var(--mp-mono, monospace); }
.mp-morse-sym { color: var(--mp-accent, #3df58c); letter-spacing: 2px; white-space: nowrap; }
.mp-morse-word { color: var(--mp-text-faint, #6a6252); }
.mp-morse-unknown { color: var(--mp-danger, #ff6a55); letter-spacing: 1px; white-space: nowrap; }
.mp-morse-hint { color: var(--mp-text-dim, #a09681); font-size: 12px; }
.mp-morse-dur { font: 10px var(--mp-mono, monospace); color: var(--mp-text-dim, #a09681);
  letter-spacing: .5px; }

/* Licensing warning: amber, bordered, and never hidden --- transmitting CW is
   the operator's legal responsibility and this is where that is said. */
.mp-morse-warn { display: flex; gap: 8px; align-items: flex-start;
  font: 11px/1.5 var(--mp-sans, sans-serif); color: var(--mp-warn, #ff9438);
  border: 1px solid var(--mp-warn-line, #3a2a18); background: var(--mp-warn-wash, rgba(255,148,56,.08));
  border-radius: 5px; padding: 7px 9px; }
.mp-morse-warn b { color: var(--mp-warn, #ff9438); font-weight: 700; }
.mp-morse-warn .mp-morse-warn-ico { flex: none; font-size: 13px; line-height: 1.35; }

.mp-morse-actions { display: flex; align-items: center; gap: 8px; flex-wrap: wrap; }
.mp-morse-btn { appearance: none; height: 30px; padding: 0 12px; cursor: pointer;
  border-radius: 5px; font: 11px var(--mp-mono, monospace); letter-spacing: 1px;
  text-transform: uppercase; white-space: nowrap;
  border: 1px solid var(--mp-border, #2b2820); background: transparent;
  color: var(--mp-text-dim, #a09681);
  transition: border-color .15s ease, color .15s ease, background .15s ease; }
.mp-morse-btn:hover:not(:disabled) { border-color: var(--mp-accent-edge, rgba(61,245,140,.6));
  color: var(--mp-accent, #3df58c); }
.mp-morse-btn:disabled { color: var(--mp-text-faint, #6a6252);
  border-color: var(--mp-line-soft, #1c1913); cursor: not-allowed; }
.mp-morse-btn.mp-playing { border-color: var(--mp-accent-edge, rgba(61,245,140,.6));
  color: var(--mp-accent, #3df58c); background: var(--mp-accent-wash, rgba(61,245,140,.12)); }

/* The one control that radiates: warm/danger emphasis when live, so it never
   reads as just another button. Disabled it recedes but stays legible. */
.mp-morse-tx-btn { border-color: var(--mp-warn-dim, #b06a2a); color: var(--mp-warn, #ff9438); }
.mp-morse-tx-btn:hover:not(:disabled) { border-color: var(--mp-danger, #ff6a55);
  color: var(--mp-danger, #ff6a55); background: rgba(255,106,85,.10); }
.mp-morse-tx-btn:disabled { color: var(--mp-text-faint, #6a6252);
  border-color: var(--mp-line-soft, #1c1913); background: transparent; }

.mp-morse-reason { font: 11px var(--mp-mono, monospace); color: var(--mp-text-dim, #a09681); }
.mp-morse-txstatus { font: 11px var(--mp-mono, monospace); color: var(--mp-text-dim, #a09681);
  min-height: 1.2em; }
.mp-morse-txstatus.mp-ok { color: var(--mp-ok, #3ddc84); }
.mp-morse-txstatus.mp-error { color: var(--mp-danger, #ff6a55); }
`;

  function ensureStyle() {
    if (document.getElementById(STYLE_ID)) return;
    const style = document.createElement("style");
    style.id = STYLE_ID;
    style.textContent = CSS;
    document.head.appendChild(style);
  }

  // ---------------------------------------------------------------------------
  // Skeleton --- built once per el; render() only updates it thereafter.
  // ---------------------------------------------------------------------------

  function buildSkeleton(el) {
    ensureStyle();
    el.innerHTML = "";
    el.classList.add("mp-panel", "mp-morse-panel");

    const root = document.createElement("div");
    root.className = "mp-morse-root";

    // --- RECEIVE ---
    const rxSec = document.createElement("div");
    rxSec.className = "mp-morse-section mp-morse-rx-section";
    rxSec.innerHTML = `
      <div class="mp-morse-head">
        <span class="mp-morse-label">Receive · decoded</span>
        <div class="mp-morse-readouts">
          <span class="mp-morse-readout" data-r="wpm"></span>
          <span class="mp-morse-readout" data-r="tone"></span>
          <span class="mp-morse-chip" data-r="recv">Idle</span>
        </div>
      </div>
      <div class="mp-morse-rx">
        <div class="mp-morse-rx-text"></div>
        <div class="mp-morse-empty">Listening… no Morse decoded yet.</div>
      </div>
    `;

    // --- TRANSMIT ---
    const txSec = document.createElement("div");
    txSec.className = "mp-morse-section mp-morse-tx-section";
    txSec.innerHTML = `
      <div class="mp-morse-head">
        <span class="mp-morse-label">Transmit · text → Morse</span>
        <span class="mp-morse-dur" data-t="dur"></span>
      </div>
      <div class="mp-morse-tx">
        <div class="mp-morse-row">
          <input class="mp-morse-input" data-t="text" type="text" autocomplete="off"
                 spellcheck="false" placeholder="e.g. CQ CQ DE M0ABC K" aria-label="Text to send as Morse" />
          <label class="mp-morse-field">WPM
            <input class="mp-morse-wpm" data-t="wpm" type="number"
                   min="${WPM_MIN}" max="${WPM_MAX}" step="1" value="${WPM_DEFAULT}" aria-label="Words per minute" />
          </label>
        </div>
        <div class="mp-morse-preview" data-t="preview"></div>
        <div class="mp-morse-warn">
          <span class="mp-morse-warn-ico" aria-hidden="true">⚠</span>
          <span><b>Transmit keys the radio and radiates RF.</b> Sending Morse on the air
          requires an amateur (or other) licence almost everywhere, on a band and mode you
          are authorised to use. Operating legally is your responsibility — “Play” below is
          local audio only and keys nothing.</span>
        </div>
        <div class="mp-morse-actions">
          <button class="mp-morse-btn" data-t="play" type="button" disabled>▶ Play</button>
          <button class="mp-morse-btn mp-morse-tx-btn" data-t="tx" type="button" disabled>Transmit (keys radio)</button>
          <span class="mp-morse-reason" data-t="reason"></span>
        </div>
        <div class="mp-morse-txstatus" data-t="status"></div>
      </div>
    `;

    root.appendChild(rxSec);
    root.appendChild(txSec);
    el.appendChild(root);

    const st = {
      el,
      // RX
      rx: rxSec.querySelector(".mp-morse-rx"),
      rxText: rxSec.querySelector(".mp-morse-rx-text"),
      rxEmpty: rxSec.querySelector(".mp-morse-empty"),
      wpmEl: rxSec.querySelector('[data-r="wpm"]'),
      toneEl: rxSec.querySelector('[data-r="tone"]'),
      recvChip: rxSec.querySelector('[data-r="recv"]'),
      lastDecoded: null,
      // TX
      txInput: txSec.querySelector('[data-t="text"]'),
      txWpm: txSec.querySelector('[data-t="wpm"]'),
      preview: txSec.querySelector('[data-t="preview"]'),
      durEl: txSec.querySelector('[data-t="dur"]'),
      playBtn: txSec.querySelector('[data-t="play"]'),
      txBtn: txSec.querySelector('[data-t="tx"]'),
      reasonEl: txSec.querySelector('[data-t="reason"]'),
      statusEl: txSec.querySelector('[data-t="status"]'),
      // capability probe
      canTx: undefined, // undefined = not yet asked; true/false; null = unknown
      statusAt: 0,
      statusInFlight: false,
      // audio + transmit
      audioCtx: null,
      playing: null,
      txInFlight: false,
    };
    el.__mpMorse = st;

    wireTransmit(st);
    updatePreview(st);
    updateTxControls(st);
    return st;
  }

  // ---------------------------------------------------------------------------
  // Receive
  // ---------------------------------------------------------------------------

  function updateReceive(st, decoded) {
    if (decoded === st.lastDecoded) return;
    const nearBottom = st.rx.scrollHeight - st.rx.scrollTop - st.rx.clientHeight < 24;
    if (decoded === "") {
      st.rxText.textContent = "";
      st.rxText.style.display = "none";
      st.rxEmpty.style.display = "";
    } else {
      st.rxEmpty.style.display = "none";
      st.rxText.style.display = "";
      st.rxText.textContent = decoded;
    }
    st.lastDecoded = decoded;
    if (nearBottom) st.rx.scrollTop = st.rx.scrollHeight;
  }

  // ---------------------------------------------------------------------------
  // Transmit --- preview, local audio, capability gating, POST
  // ---------------------------------------------------------------------------

  function wireTransmit(st) {
    st.txInput.addEventListener("input", () => {
      updatePreview(st);
      updateTxControls(st);
    });
    // Enter must NOT transmit --- keying the radio is a deliberate click only.
    st.txInput.addEventListener("keydown", (ev) => {
      if (ev.key === "Enter") ev.preventDefault();
    });
    st.txWpm.addEventListener("input", () => updatePreview(st));
    st.txWpm.addEventListener("change", () => {
      st.txWpm.value = String(clampWpm(st.txWpm.value));
      updatePreview(st);
    });
    st.playBtn.addEventListener("click", () => {
      if (st.playing) stopMorse(st);
      else playMorse(st);
    });
    st.txBtn.addEventListener("click", () => doTransmit(st));
  }

  function updatePreview(st) {
    const text = st.txInput.value;
    const tokens = morseTokens(text);
    st.preview.innerHTML = "";
    if (tokens.length === 0) {
      const hint = document.createElement("span");
      hint.className = "mp-morse-hint";
      hint.textContent = "Type text above to see its Morse.";
      st.preview.appendChild(hint);
      st.durEl.textContent = "";
      st.playBtn.disabled = true;
      return;
    }
    let audible = false;
    for (const t of tokens) {
      const span = document.createElement("span");
      if (t.type === "sym") {
        span.className = "mp-morse-sym";
        span.textContent = t.code;
        span.title = t.ch;
        audible = true;
      } else if (t.type === "word") {
        span.className = "mp-morse-word";
        span.textContent = "/";
      } else {
        span.className = "mp-morse-unknown";
        span.textContent = UNKNOWN_MARK;
        span.title = `No Morse for “${t.ch}” — it will not be sent`;
      }
      st.preview.appendChild(span);
    }
    const wpm = clampWpm(st.txWpm.value);
    const total = scheduleTotalMs(text, unitMs(wpm));
    st.durEl.textContent = total > 0 ? `≈ ${(total / 1000).toFixed(1)} s @ ${wpm} wpm` : "";
    // Play is local and safe; enable it whenever there is something audible.
    st.playBtn.disabled = !audible || st.txInFlight;
  }

  function setPlayLabel(st, playing) {
    st.playBtn.textContent = playing ? "■ Stop" : "▶ Play";
    st.playBtn.classList.toggle("mp-playing", playing);
  }

  // playMorse schedules the whole message on a single gated oscillator using the
  // audio clock (sample-accurate, and immune to a throttled background tab in a
  // way a setInterval keyer would not be). It keys NOTHING but the speaker.
  function playMorse(st) {
    stopMorse(st);
    const wpm = clampWpm(st.txWpm.value);
    const seg = morseSchedule(st.txInput.value, unitMs(wpm));
    if (!seg.some((s) => s.on)) return;

    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) return; // no Web Audio here; Play simply does nothing
    const ctx = st.audioCtx || (st.audioCtx = new AC());
    if (ctx.state === "suspended" && ctx.resume) ctx.resume();

    const osc = ctx.createOscillator();
    const gain = ctx.createGain();
    osc.type = "sine";
    osc.frequency.value = SIDETONE_HZ;
    gain.gain.value = 0;
    osc.connect(gain);
    gain.connect(ctx.destination);

    let t = ctx.currentTime + 0.06;
    for (const s of seg) {
      const dur = s.dur / 1000;
      if (s.on) {
        gain.gain.setValueAtTime(0, t);
        gain.gain.linearRampToValueAtTime(SIDETONE_GAIN, t + RAMP_S);
        gain.gain.setValueAtTime(SIDETONE_GAIN, t + Math.max(RAMP_S, dur - RAMP_S));
        gain.gain.linearRampToValueAtTime(0, t + dur);
      }
      t += dur;
    }
    osc.start();
    osc.stop(t + 0.02);

    const handle = { osc, gain };
    st.playing = handle;
    osc.onended = () => {
      if (st.playing === handle) {
        st.playing = null;
        setPlayLabel(st, false);
      }
    };
    setPlayLabel(st, true);
  }

  function stopMorse(st) {
    if (st.playing) {
      const { osc, gain } = st.playing;
      try {
        gain.gain.cancelScheduledValues(0);
        gain.gain.value = 0;
        osc.onended = null;
        osc.stop();
      } catch (_) { /* already stopped */ }
      st.playing = null;
    }
    setPlayLabel(st, false);
  }

  // refreshCanTx reads GET /api/status for `can_transmit` (a bool, or absent =
  // unknown) and caches it briefly. A failed request, a missing field, or no
  // fetch() at all all resolve to "unknown", which keeps the Transmit button
  // OFF --- the safe default. This never presents a live keying control to a
  // receive-only or unknown device state.
  function refreshCanTx(st) {
    if (st.statusInFlight) return;
    const now = Date.now();
    if (st.statusAt && now - st.statusAt < STATUS_TTL_MS) return;
    if (typeof fetch !== "function") {
      st.canTx = null;
      st.statusAt = now;
      return;
    }
    st.statusInFlight = true;
    fetch("/api/status", { headers: { Accept: "application/json" } })
      .then((r) => (r.ok ? r.json() : null))
      .then((body) => {
        const v = body ? body.can_transmit : undefined;
        st.canTx = typeof v === "boolean" ? v : null;
      })
      .catch(() => { st.canTx = null; })
      .finally(() => {
        st.statusInFlight = false;
        st.statusAt = Date.now();
        updateTxControls(st);
      });
  }

  function updateTxControls(st) {
    const hasText = st.txInput.value.trim().length > 0;
    const canTx = st.canTx === true;
    st.txBtn.disabled = !canTx || !hasText || st.txInFlight;

    let reason = "";
    if (st.txInFlight) {
      reason = "";
    } else if (st.canTx === false) {
      reason = "Attached radio is receive-only — transmit disabled.";
    } else if (st.canTx === undefined) {
      reason = "Checking transmit capability…";
    } else if (st.canTx !== true) {
      reason = "Transmit capability unknown (no device reporting it) — button stays off.";
    } else if (!hasText) {
      reason = "Enter text above to enable transmit.";
    }
    st.reasonEl.textContent = reason;
    st.reasonEl.style.display = reason ? "" : "none";

    st.txBtn.textContent = st.txInFlight ? "Transmitting…" : "Transmit (keys radio)";
  }

  function setTxStatus(st, msg, kind) {
    st.statusEl.textContent = msg || "";
    st.statusEl.classList.toggle("mp-error", kind === "error");
    st.statusEl.classList.toggle("mp-ok", kind === "ok");
  }

  // doTransmit POSTs the message and keys the radio. It is the only path in this
  // file that radiates. Failure is any non-2xx OR any ok:false, and the backend's
  // error string is shown verbatim; there is no automatic retry.
  function doTransmit(st) {
    if (st.txInFlight) return;
    const text = st.txInput.value.trim();
    if (!text || st.canTx !== true) return; // gated; updateTxControls keeps the button off too
    if (typeof fetch !== "function") {
      setTxStatus(st, "transmit unavailable: no fetch() in this environment", "error");
      return;
    }
    const wpm = clampWpm(st.txWpm.value);

    st.txInFlight = true;
    updateTxControls(st);
    st.playBtn.disabled = true;
    setTxStatus(st, "Transmitting…", null);

    fetch("/api/morse/transmit", {
      method: "POST",
      headers: { "Content-Type": "application/json", Accept: "application/json" },
      body: JSON.stringify({ text, wpm }),
    })
      .then((res) =>
        res.json()
          .catch(() => null)
          .then((body) => ({ res, body }))
      )
      .then(({ res, body }) => {
        const ok = res.ok && body && body.ok === true;
        if (ok) {
          const ms = Number(body.duration_ms);
          setTxStatus(
            st,
            Number.isFinite(ms) ? `Sent — radio keyed for ${(ms / 1000).toFixed(1)} s.` : "Sent.",
            "ok"
          );
        } else {
          const err =
            body && typeof body.error === "string" && body.error
              ? body.error
              : `transmit failed (HTTP ${res.status})`;
          setTxStatus(st, err, "error");
        }
      })
      .catch((e) => {
        setTxStatus(st, "transmit request failed: " + ((e && e.message) || "network error"), "error");
      })
      .finally(() => {
        st.txInFlight = false;
        updateTxControls(st);
        updatePreview(st); // re-enable Play from the audible check
      });
  }

  // ---------------------------------------------------------------------------
  // The contract
  // ---------------------------------------------------------------------------

  function render(el, data) {
    data = data || {};
    const st = el.__mpMorse || buildSkeleton(el);

    // RECEIVE. decoded_text may legitimately be "" (nothing heard yet).
    const decoded = typeof data.decoded_text === "string" ? data.decoded_text : "";
    updateReceive(st, decoded);

    // wpm / tone_hz are OMITTED until the decoder has an estimate. A JSON null
    // is not a number and stays hidden --- never coerced to 0.
    if (typeof data.wpm === "number" && Number.isFinite(data.wpm)) {
      st.wpmEl.textContent = `${Math.round(data.wpm)} WPM`;
      st.wpmEl.style.display = "";
    } else {
      st.wpmEl.style.display = "none";
    }
    if (typeof data.tone_hz === "number" && Number.isFinite(data.tone_hz)) {
      st.toneEl.textContent = `${Math.round(data.tone_hz)} Hz`;
      st.toneEl.style.display = "";
    } else {
      st.toneEl.style.display = "none";
    }

    const receiving = data.receiving === true;
    st.recvChip.textContent = receiving ? "Receiving" : "Idle";
    st.recvChip.classList.toggle("mp-live", receiving);

    // TRANSMIT capability: probe /api/status (cached), then reconcile the button.
    refreshCanTx(st);
    updateTxControls(st);
  }

  window.MayhemPanels.registerPanel("morse", render);

  // Browser-inert test seam: in a plain <script> `module` is undefined, so this
  // is skipped entirely and nothing leaks onto the page. Under node it lets the
  // fixture/unit tests exercise the real Morse table and timing in this file
  // rather than a copy of it.
  if (typeof module !== "undefined" && module.exports) {
    module.exports = { MORSE, morseTokens, textToMorse, morseSchedule, scheduleTotalMs, unitMs, clampWpm };
  }
})();
