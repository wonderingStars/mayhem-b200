/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Part of mayhem-b200. */

/*
 * The live device screen, in a browser tab.
 *
 * mayhem-b200 draws every one of its ~103 apps into one 240x320 framebuffer
 * and funnels every key, encoder detent and touch through a single
 * dispatcher. This file is the browser end of mirroring those two things:
 * frames in over a WebSocket, input back out over the same socket. There is
 * no per-app code here and there never should be — what you are looking at
 * IS the app, not a re-implementation of it.
 *
 * Public surface (a global, like the rest of this portal — no build step):
 *
 *   MayhemScreenView.mount(el[, ctx])  // build the view inside el (idempotent)
 *   MayhemScreenView.unmount(el)       // close the socket and tear it down
 *
 * WIRE CONTRACT 3 (frozen; the Go end is internal/portal/server/screen.go):
 *
 *   ws://<host>/api/screen/ws
 *     server->client binary: the 16-byte MBSF frame header
 *         magic "MBSF" | version=1 | format | width u16 LE | height u16 LE |
 *         seq u32 LE | reserved u16
 *       followed by width*height*2 bytes of little-endian RGB565 rows, top to
 *       bottom (format 1) or the same payload raw-DEFLATEd (format 2).
 *     server->client text:   {"type":"status","controlling":bool,"viewers":N}
 *     client->server text:   {"events":[ ... ]}   (contract 2's schema)
 *
 * Three house rules this file exists under, all learned the hard way:
 *
 *  - PAINT ON ARRIVAL, never requestAnimationFrame alone. rAF does not fire
 *    in a hidden or non-compositing tab — including the verification browser
 *    pane — so an rAF-scheduled paint is both slower to first pixel and
 *    invisible to the one place changes get checked. Every frame here is
 *    drawn synchronously in the message handler (see paintFrame).
 *  - ABSENT DATA STAYS ABSENT. Before the first frame arrives the canvas is
 *    not shown at all and the view says so in words, on a warm panel colour
 *    rather than black: a black rectangle inside a bezel IS a picture of a
 *    switched-on screen, and this view must never invent one.
 *  - THE DEVICE PIXEL GRID IS SACRED. 240x320 is a small, hand-drawn
 *    framebuffer; a fractional scale factor smears every 1px rule in it.
 *    relayout() therefore picks an INTEGER number of physical pixels per
 *    device pixel (dividing by devicePixelRatio so a 125%-scaled Windows
 *    desktop still lands on whole device pixels) rather than letting CSS
 *    stretch the canvas to whatever space is going spare.
 *
 * PRESENTATION NOTE (2026-08-13 redesign): the chrome around the canvas — the
 * bezel, the two context columns, the key map, the focus call to action — is
 * all new, and all of it is presentation. The socket, the frame decode, the
 * input protocol and the controller/viewer rules below are byte-for-byte what
 * they were; the only new *input* behaviour is that focus and blur now also
 * repaint the chrome, because "does this canvas have focus" is the single
 * thing a first-time user cannot guess and the whole right-hand column exists
 * to answer it.
 */
(function () {
  "use strict";

  // ---- contract constants -------------------------------------------------

  var SCREEN_HEADER = 16;
  var FMT_RGB565 = 1;
  var FMT_DEFLATE = 2;
  var FRAME_VERSION = 1;

  // ---- tunables -----------------------------------------------------------

  var RECONNECT_MIN_MS = 500;
  var RECONNECT_MAX_MS = 5000;
  // Input is batched over roughly one device frame. The Go side batches
  // again into one POST; this one exists to stop a 120Hz pointer emitting a
  // WebSocket frame per sample.
  var INPUT_FLUSH_MS = 16;
  var INPUT_QUEUE_MAX = 256;
  // One encoder detent per notch of a real mouse wheel. Browsers disagree on
  // the units: Chrome/Edge/Safari report pixels (deltaMode 0, ~100px per
  // notch), Firefox reports lines (deltaMode 1, 3 lines per notch), and page
  // mode (2) is one screen per notch. Everything is converted to NOTCHES
  // before it is accumulated, so a notch is a detent in every browser and a
  // trackpad's small deltas add up smoothly instead of being rounded away.
  // (Measured: a deltaY of -100 in deltaMode 0 must produce exactly +1.)
  var WHEEL_PX_PER_NOTCH = 100;
  var WHEEL_LINES_PER_NOTCH = 3;

  // Bezel geometry, in one place because both the stylesheet and the layout
  // maths need it: the CSS below interpolates these numbers, and relayout()
  // subtracts them from the space it is given to find how much room the
  // pixels themselves have. Editing one therefore cannot desync the other.
  var BEZEL_PAD_X = 26;   // .mbsv-device left+right padding, each side
  var BEZEL_PAD_TOP = 22; // .mbsv-device padding-top
  var BEZEL_PAD_BOT = 14; // .mbsv-device padding-bottom
  var WELL_BORDER = 2;    // .mbsv-well border-width
  var PLATE_H = 26;       // .mbsv-plate line under the screen
  // Slack left below the bezel so the device never sits flush against the
  // bottom of the window with its own drop shadow clipped off.
  var STAGE_BOTTOM_SLACK = 28;
  // Cap the magnification: past 3x the device's own 1px UI rules turn into
  // fence posts and the thing stops reading as a handheld screen.
  var MAX_SCALE = 3;

  // The six keys the device physically has (ui::KeyEvent). Anything else
  // printable becomes a char event; anything else at all is left to the
  // browser, so the rest of the page keeps working.
  var KEY_MAP = {
    ArrowUp: "up",
    ArrowDown: "down",
    ArrowLeft: "left",
    ArrowRight: "right",
    Enter: "select",
    Escape: "back",
  };

  // The key map as the operator sees it. Same six keys, plus the two pointer
  // gestures — this is a rendering of KEY_MAP and the wheel/pointer handlers
  // below, not a second source of truth. Keep them in step by hand.
  var KEY_HELP = [
    ["↑↓←→", "Navigate"],
    ["Enter", "Select · press encoder"],
    ["Esc", "Back"],
    ["Wheel", "Turn the encoder"],
    ["Click", "Touch the screen"],
    ["A-Z 0-9", "Text entry"],
  ];

  // hintUsedOnce is module scope on purpose: the big "click to capture input"
  // call to action is a teaching aid, and the brief is that it disappears
  // once used. Once this operator has focused the screen even once, every
  // later mount (a panel switch, another app) drops back to the quiet border
  // + status-chip treatment, which still says what state input is in without
  // shouting the lesson again. A page reload teaches it again, deliberately:
  // that is also a new operator arriving at a shared machine.
  var hintUsedOnce = false;

  // ---- deflate support ----------------------------------------------------

  // Probed once, at load: a browser without DecompressionStream("deflate-raw")
  // asks the server for format 1 with ?deflate=0 rather than receiving frames
  // it cannot decode and showing a frozen screen.
  var CAN_INFLATE = (function () {
    if (typeof DecompressionStream !== "function") return false;
    try {
      /* eslint-disable no-new */
      new DecompressionStream("deflate-raw");
      return true;
    } catch (e) {
      return false;
    }
  })();

  function inflateRaw(bytes) {
    var stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("deflate-raw"));
    return new Response(stream).arrayBuffer().then(function (ab) {
      return new Uint8Array(ab);
    });
  }

  // ---- styles -------------------------------------------------------------

  // Injected rather than added to app.css: this file is self-contained, and
  // the portal's stylesheet is owned elsewhere. Everything is .mbsv-* so it
  // cannot collide with app.css or the panels/*.css conventions.
  //
  // The palette is the terminal / amber-phosphor one from the portal design
  // (doc/design-ref), written out here rather than read from app.css's
  // custom properties: this view has to look like itself in the panels
  // harness and in any page that loads it, and a var(--accent, …) fallback
  // would silently pick up whatever the shell's accent happens to be.
  //
  // Contrast was measured, not eyeballed (WCAG 2.1 relative luminance):
  //   #ede8db on #14120e = 15.3:1   #cfc7b4 on #191713 = 10.6:1
  //   #a09681 on #14120e =  6.4:1   #3df58c on #12100d = 13.2:1
  //   #ff9438 on #14120e =  8.5:1
  // The design's #6a6252 measures 3.2:1 on #0d0c0a — that is below AA for
  // text of any size, so here it is used ONLY for hairlines and the inert
  // half of a state dot, never for a word. Its jobs in the mockup (small
  // mono captions) are done by #a09681 instead.
  //
  // The three @font-face rules repeat what app.css and panels/panels.css
  // declare, for the same reason: this view must look like itself even when
  // it is the only thing loaded. They cost nothing — same family, same
  // descriptors, same URL, so the browser matches one face and fetches it
  // once (measured: three woff2 requests on a page load, not nine).
  var STYLE_ID = "mbsv-style";
  var CSS = `
@font-face{font-family:'Fragment Mono';font-style:normal;font-weight:400;font-display:swap;
  src:url("/fonts/fragment-mono-400.woff2") format("woff2")}
@font-face{font-family:'Archivo';font-style:normal;font-weight:500;font-stretch:100%;font-display:swap;
  src:url("/fonts/archivo-500.woff2") format("woff2")}
@font-face{font-family:'Archivo';font-style:italic;font-weight:800;font-stretch:100%;font-display:swap;
  src:url("/fonts/archivo-800-italic.woff2") format("woff2")}

/* Namespaced rather than reusing the design's shared mpLed: this <style> is
   appended at runtime, so a same-named @keyframes here would win over the
   shell's copy for every element on the page, not just this view. */
@keyframes mbsvLed{0%,100%{opacity:.4;box-shadow:0 0 3px 0 currentColor}
  50%{opacity:1;box-shadow:0 0 10px 2px currentColor}}
@keyframes mbsvCta{0%,100%{opacity:.72}50%{opacity:1}}

.mbsv-root{
  --mbsv-card:#14120e; --mbsv-key:#191713; --mbsv-well:#100e0b;
  --mbsv-line:#242019; --mbsv-line-2:#2b2820; --mbsv-key-line:#3a3428; --mbsv-inert:#6a6252;
  --mbsv-text:#ede8db; --mbsv-text-2:#cfc7b4; --mbsv-dim:#a09681;
  --mbsv-accent:#3df58c; --mbsv-accent-2:#3ddc84; --mbsv-warn:#ff9438;
  --mbsv-mono:'Fragment Mono',ui-monospace,'Cascadia Mono',Consolas,monospace;
  --mbsv-sans:'Archivo','Segoe UI',system-ui,-apple-system,sans-serif;
  display:flex;flex-direction:column;gap:16px;min-width:0;
  font-family:var(--mbsv-sans);color:var(--mbsv-text)}
.mbsv-root *{box-sizing:border-box}

/* ---- head strip ---- */
.mbsv-head{display:flex;align-items:center;gap:12px;flex-wrap:wrap;
  padding-bottom:12px;border-bottom:1px solid var(--mbsv-line)}
.mbsv-app{font-size:15px;font-weight:500;letter-spacing:1.5px;text-transform:uppercase}
.mbsv-sub{font-family:var(--mbsv-mono);font-size:10.5px;letter-spacing:1px;color:var(--mbsv-dim)}
.mbsv-spacer{flex:1 1 auto}
.mbsv-state{display:flex;align-items:center;gap:14px;flex-wrap:wrap;
  font-family:var(--mbsv-mono);font-size:11px;letter-spacing:1px}
.mbsv-chip{display:inline-flex;align-items:center;gap:7px;white-space:nowrap;color:var(--mbsv-dim)}
.mbsv-dot{width:6px;height:6px;border-radius:50%;background:var(--mbsv-inert);color:var(--mbsv-inert);flex:none}
.mbsv-chip.live{color:var(--mbsv-accent)}
.mbsv-chip.live .mbsv-dot{background:var(--mbsv-accent);color:var(--mbsv-accent);
  animation:mbsvLed 2.4s ease-in-out infinite}
.mbsv-chip.busy{color:var(--mbsv-warn)}
.mbsv-chip.busy .mbsv-dot{background:var(--mbsv-warn);color:var(--mbsv-warn);
  animation:mbsvLed 1.1s ease-in-out infinite}
.mbsv-chip.down{color:var(--mbsv-warn)}
.mbsv-chip.down .mbsv-dot{background:var(--mbsv-warn);color:var(--mbsv-warn)}
.mbsv-warn{font-family:var(--mbsv-mono);font-size:11px;color:var(--mbsv-warn);flex-basis:100%}
.mbsv-warn:empty{display:none}

/* ---- three columns: context, device, controls ---- */
.mbsv-cols{display:flex;flex-wrap:wrap;justify-content:center;align-items:flex-start;gap:20px}
.mbsv-side{display:flex;flex-direction:column;gap:12px;flex:1 1 250px;min-width:0;max-width:340px}
.mbsv-side-l{order:1}
/* flex-basis + min-width:0 is load-bearing, not styling: it makes the stage's
   width a function of the ROW, never of the canvas inside it. Sized by its
   content instead, the ResizeObserver below would feed each new scale back
   into the next measurement. */
.mbsv-stage{display:flex;justify-content:center;flex:1 1 340px;min-width:0;order:2}
.mbsv-side-r{order:3}
@media (max-width:900px){
  /* Stacked, the screen itself has to come first — the context is context. */
  .mbsv-stage{order:0;flex:1 1 100%}
  .mbsv-side{max-width:none}
}

.mbsv-card{background:var(--mbsv-card);border:1px solid var(--mbsv-line);border-radius:6px;padding:16px 18px}
.mbsv-card-h{font-family:var(--mbsv-mono);font-size:10px;letter-spacing:2px;
  color:var(--mbsv-dim);margin-bottom:12px}
.mbsv-name{font-size:16px;font-weight:500;letter-spacing:.3px;margin-bottom:4px}
.mbsv-kind{font-family:var(--mbsv-mono);font-size:10px;letter-spacing:1px;
  color:var(--mbsv-warn);margin-bottom:10px}
.mbsv-kind:empty{display:none}
.mbsv-blurb{font-size:12.5px;line-height:1.55;color:var(--mbsv-dim)}

/* ---- stream facts ---- */
.mbsv-facts{display:grid;grid-template-columns:auto 1fr;gap:7px 14px;
  font-family:var(--mbsv-mono);font-size:11px;line-height:1.5}
.mbsv-facts dt{color:var(--mbsv-dim);letter-spacing:1px}
.mbsv-facts dd{margin:0;color:var(--mbsv-text-2);text-align:right}

/* ---- the device ---- */
.mbsv-device{background:linear-gradient(180deg,#191713,#12100d);border:1px solid var(--mbsv-line-2);
  border-radius:16px;padding:${BEZEL_PAD_TOP}px ${BEZEL_PAD_X}px ${BEZEL_PAD_BOT}px;
  box-shadow:0 24px 60px rgba(0,0,0,.5);max-width:100%}
/* content-box, and sized in px by relayout(), so the border sits OUTSIDE the
   pixel grid: a border-box well would eat two device pixels of the image. No
   max-width, deliberately — relayout() already guarantees the fit, and a
   clamp here would squash the width while the height stayed put, which is
   the one thing a pixel-exact mirror must never do. */
.mbsv-well{position:relative;box-sizing:content-box;border-radius:4px;
  border:${WELL_BORDER}px solid var(--mbsv-line-2);background:var(--mbsv-well);overflow:hidden;
  box-shadow:inset 0 0 24px rgba(0,0,0,.8);
  transition:border-color .18s ease,box-shadow .18s ease}
/* Phosphor: only once there are real pixels behind it, and stronger while
   the screen holds focus — the glow doubles as the focus affordance. */
.mbsv-well.lit{background:#000;box-shadow:inset 0 0 24px rgba(0,0,0,.8),0 0 26px -8px rgba(61,245,140,.28)}
/* .focus is the plain "this element has the keyboard" ring and is shown even
   for a read-only viewer, who must still be able to see where Tab landed;
   .hot is the stronger phosphor that means keys are actually going out. */
.mbsv-well.focus{border-color:var(--mbsv-text-2);
  box-shadow:inset 0 0 24px rgba(0,0,0,.8),0 0 0 2px rgba(207,199,180,.35)}
.mbsv-well.hot{border-color:var(--mbsv-accent);
  box-shadow:inset 0 0 24px rgba(0,0,0,.8),0 0 0 1px rgba(61,245,140,.45),0 0 38px -6px rgba(61,245,140,.5)}
/* pixelated LAST on purpose: where both are understood it is the one that
   means nearest-neighbour by definition, and crisp-edges is left underneath
   as the fallback for engines that only ever shipped that spelling.
   (Measured before the swap: Chrome computed image-rendering: crisp-edges.) */
.mbsv-canvas{display:block;width:100%;height:100%;
  image-rendering:crisp-edges;image-rendering:pixelated;
  background:#000;touch-action:none;outline:none;cursor:crosshair}
.mbsv-canvas.hidden{display:none}
.mbsv-plate{display:flex;align-items:center;justify-content:space-between;gap:12px;
  height:${PLATE_H}px;padding-top:10px;font-family:var(--mbsv-mono);font-size:9.5px;
  letter-spacing:2px;color:var(--mbsv-dim);min-width:0}
.mbsv-plate span:first-child{overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.mbsv-plate .mbsv-dot{width:7px;height:7px}
.mbsv-plate .mbsv-dot.on{background:var(--mbsv-accent-2);color:var(--mbsv-accent-2);
  animation:mbsvLed 2.4s ease-in-out infinite}

/* Waiting / error text, inside the well but never black: a black rectangle
   would read as "the device is on and showing nothing", which is a lie. */
.mbsv-empty{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;
  justify-content:center;gap:8px;padding:24px;text-align:center;
  background:var(--mbsv-well);color:var(--mbsv-dim);font-size:12.5px;line-height:1.5}
.mbsv-empty::before{content:"NO SIGNAL";font-family:var(--mbsv-mono);font-size:10px;
  letter-spacing:2px;color:var(--mbsv-warn)}
.mbsv-empty.banner{inset:auto 0 0 0;background:rgba(11,10,8,.92);border-top:1px solid var(--mbsv-line);
  padding:8px 12px;font-size:11px;flex-direction:row;gap:8px}
.mbsv-empty.banner::before{content:"NOTE";color:var(--mbsv-warn)}
.mbsv-empty.hidden{display:none}

/* ---- the thing nobody guesses: the canvas must have focus ---- */
.mbsv-cta{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;
  justify-content:center;gap:10px;padding:20px;text-align:center;
  background:rgba(11,10,8,.72);pointer-events:none}
.mbsv-cta.hidden{display:none}
.mbsv-cta-key{font-family:var(--mbsv-mono);font-size:12px;letter-spacing:2px;color:var(--mbsv-accent);
  border:1px solid rgba(61,245,140,.5);border-radius:5px;padding:8px 14px;
  animation:mbsvCta 2s ease-in-out infinite}
.mbsv-cta-sub{font-size:12px;line-height:1.5;color:var(--mbsv-text-2);max-width:22em}

/* ---- key map ---- */
.mbsv-keys{display:flex;flex-direction:column;gap:10px}
.mbsv-keyrow{display:flex;align-items:center;gap:10px;font-size:12.5px;color:var(--mbsv-dim)}
.mbsv-cap{min-width:56px;text-align:center;padding:3px 7px;border:1px solid var(--mbsv-key-line);
  border-bottom-width:2.5px;border-radius:4px;background:var(--mbsv-key);
  font-family:var(--mbsv-mono);font-size:11px;color:var(--mbsv-text-2);flex:none}
.mbsv-card.armed{border-color:rgba(61,245,140,.45)}
.mbsv-card.armed .mbsv-cap{border-color:rgba(61,245,140,.5);color:var(--mbsv-accent)}
.mbsv-card.idle .mbsv-cap{opacity:.75}

.mbsv-note{font-size:12px;line-height:1.55;color:var(--mbsv-dim)}
.mbsv-card.armed .mbsv-note{color:var(--mbsv-accent)}

@media (prefers-reduced-motion:reduce){
  /* The LEDs and the call to action are decoration; the state they carry is
     also in the border colour and in words, so stopping them loses nothing. */
  .mbsv-chip .mbsv-dot,.mbsv-plate .mbsv-dot,.mbsv-cta-key{animation:none}
  .mbsv-well{transition:none}
}
/* The shell's motion control (design's full/calm/off), if it ever sets one. */
:root[data-motion="off"] .mbsv-root .mbsv-dot,
:root[data-motion="off"] .mbsv-root .mbsv-cta-key{animation:none}
`;

  function ensureStyle() {
    if (document.getElementById(STYLE_ID)) return;
    var style = document.createElement("style");
    style.id = STYLE_ID;
    style.textContent = CSS;
    (document.head || document.documentElement).appendChild(style);
  }

  // ---- helpers ------------------------------------------------------------

  function clamp(v, lo, hi) {
    return v < lo ? lo : (v > hi ? hi : v);
  }

  // setText writes only when the string actually changed. paintFrame calls
  // the chrome renderer on every frame (30/s), and an unconditional
  // textContent write on a dozen nodes at that rate is layout work for
  // nothing.
  function setText(el, s) {
    if (el.textContent !== s) el.textContent = s;
  }

  function setClass(el, name, on) {
    if (el.classList.contains(name) !== !!on) el.classList.toggle(name, !!on);
  }

  function div(cls, text) {
    var d = document.createElement("div");
    if (cls) d.className = cls;
    if (text) d.textContent = text;
    return d;
  }

  function span(cls, text) {
    var s = document.createElement("span");
    if (cls) s.className = cls;
    if (text) s.textContent = text;
    return s;
  }

  // seqIsOlder compares two u32 sequence numbers wrap-aware: with format 2
  // the inflate is asynchronous, so two frames can finish out of order and
  // the newer one must not be overwritten by the older.
  function seqIsOlder(a, b) {
    return ((a - b) >>> 0) > 0x80000000;
  }

  // hostContext reads the app identity the portal shell has already put in
  // its own panel header (app.js owns those nodes; index.html owns the ids).
  // Read, never written, and every field is optional — if the shell changes
  // or this view is mounted somewhere else entirely (the panels harness), the
  // app card simply says less rather than claiming something untrue.
  function hostContext() {
    var t = document.getElementById("panelTitle");
    var k = document.getElementById("panelKindLabel");
    return {
      appName: t ? t.textContent.trim() : "",
      panelKind: k ? k.textContent.trim() : "",
    };
  }

  // ---- view ---------------------------------------------------------------

  function build(el, ctx) {
    ensureStyle();
    el.innerHTML = "";
    el.classList.add("mbsv-root");

    // -- head --------------------------------------------------------------
    var head = div("mbsv-head");
    var appHead = span("mbsv-app");
    var sub = span("mbsv-sub", "STREAMED SCREEN · 240×320");

    var state = div("mbsv-state");
    // Only the two state chips live in the polite region: the frame counter
    // ticks 30 times a second and would make a screen reader unusable.
    state.setAttribute("role", "status");
    state.setAttribute("aria-live", "polite");

    var linkChip = span("mbsv-chip");
    var linkDot = span("mbsv-dot");
    var linkText = span(null, "CONNECTING");
    linkChip.appendChild(linkDot);
    linkChip.appendChild(linkText);

    var inputChip = span("mbsv-chip");
    var inputDot = span("mbsv-dot");
    var inputText = span(null, "INPUT IDLE");
    inputChip.appendChild(inputDot);
    inputChip.appendChild(inputText);

    state.appendChild(linkChip);
    state.appendChild(inputChip);

    var warn = div("mbsv-warn");

    head.appendChild(appHead);
    head.appendChild(sub);
    head.appendChild(span("mbsv-spacer"));
    head.appendChild(state);
    head.appendChild(warn);

    // -- left column: what this is -----------------------------------------
    var sideL = div("mbsv-side mbsv-side-l");

    var appCard = div("mbsv-card");
    appCard.appendChild(div("mbsv-card-h", "APP"));
    var appName = div("mbsv-name");
    var appKind = div("mbsv-kind");
    appCard.appendChild(appName);
    appCard.appendChild(appKind);
    appCard.appendChild(div("mbsv-blurb",
      "This is the device's own 240×320 display, streamed pixel for pixel. " +
      "Keyboard and mouse drive the real hardware while the screen has focus."));

    var streamCard = div("mbsv-card");
    streamCard.appendChild(div("mbsv-card-h", "STREAM"));
    var facts = document.createElement("dl");
    facts.className = "mbsv-facts";
    var factLink = fact(facts, "LINK");
    var factFrames = fact(facts, "FRAMES");
    var factViewers = fact(facts, "VIEWERS");
    var factScale = fact(facts, "SCALE");
    var factCodec = fact(facts, "CODEC");
    streamCard.appendChild(facts);

    sideL.appendChild(appCard);
    sideL.appendChild(streamCard);

    // -- middle: the device ------------------------------------------------
    var stage = div("mbsv-stage");
    var device = div("mbsv-device");
    var well = div("mbsv-well");

    var canvas = document.createElement("canvas");
    canvas.className = "mbsv-canvas hidden";
    canvas.width = 240;
    canvas.height = 320;
    // tabindex is what scopes input capture: key events only reach a focused
    // element, so search boxes and buttons elsewhere on the page keep
    // working exactly as they did.
    canvas.tabIndex = 0;
    canvas.setAttribute("role", "img");
    canvas.setAttribute("aria-label",
      "Live mayhem-b200 device screen. Focus it to send keys, arrows navigate, " +
      "Enter selects, Escape goes back, the wheel turns the encoder.");

    var note = div("mbsv-empty");
    note.textContent = "Waiting for the device screen…";

    var cta = div("mbsv-cta hidden");
    cta.setAttribute("aria-hidden", "true"); // the same words are in the note card
    cta.appendChild(div("mbsv-cta-key", "CLICK TO CAPTURE INPUT"));
    cta.appendChild(div("mbsv-cta-sub",
      "Click the screen, or Tab to it. Keys, wheel and clicks reach the device "
      + "only while it has focus."));

    well.appendChild(canvas);
    well.appendChild(note);
    well.appendChild(cta);

    var plate = div("mbsv-plate");
    var plateText = span(null, "B200 REMOTE · 240×320");
    var plateDot = span("mbsv-dot");
    plate.appendChild(plateText);
    plate.appendChild(plateDot);

    device.appendChild(well);
    device.appendChild(plate);
    stage.appendChild(device);

    // -- right column: how to drive it -------------------------------------
    var sideR = div("mbsv-side mbsv-side-r");

    var keyCard = div("mbsv-card");
    keyCard.appendChild(div("mbsv-card-h", "CONTROLS"));
    var keyList = div("mbsv-keys");
    KEY_HELP.forEach(function (row) {
      var r = div("mbsv-keyrow");
      r.appendChild(span("mbsv-cap", row[0]));
      r.appendChild(span(null, row[1]));
      keyList.appendChild(r);
    });
    keyCard.appendChild(keyList);

    var noteCard = div("mbsv-card");
    var inputNote = div("mbsv-note");
    noteCard.appendChild(inputNote);

    sideR.appendChild(keyCard);
    sideR.appendChild(noteCard);

    var cols = div("mbsv-cols");
    cols.appendChild(sideL);
    cols.appendChild(stage);
    cols.appendChild(sideR);

    el.appendChild(head);
    el.appendChild(cols);

    ctx = ctx || {};
    return {
      el: el,
      canvas: canvas,
      ctx: canvas.getContext("2d", { alpha: false }),

      // chrome
      appHead: appHead,
      appName: appName,
      appKind: appKind,
      linkChip: linkChip,
      linkText: linkText,
      inputChip: inputChip,
      inputText: inputText,
      warn: warn,
      factLink: factLink,
      factFrames: factFrames,
      factViewers: factViewers,
      factScale: factScale,
      factCodec: factCodec,
      stage: stage,
      well: well,
      plateText: plateText,
      plateDot: plateDot,
      note: note,
      cta: cta,
      keyCard: keyCard,
      noteCard: noteCard,
      inputNote: inputNote,

      ws: null,
      conn: "connecting",
      retryMs: RECONNECT_MIN_MS,
      reconnectTimer: 0,
      aliveTimer: 0,
      stopped: false,

      imageData: null,
      paintedSeq: null,
      frames: 0,

      controlling: false,
      viewers: 0,
      backendError: "",

      // presentation state
      focused: false,
      scale: 0,
      scaleLabel: "",
      host: { appName: ctx.appName || "", panelKind: ctx.panelKind || "" },
      hostPinned: !!(ctx.appName || ctx.panelKind),
      resizeObserver: null,
      onWindowResize: null,

      pending: [],
      flushTimer: 0,
      held: new Set(),
      wheelAcc: 0,
      pointerId: null,
    };
  }

  // isWhole tolerates the float dust in 240*3/1.25; px trims it so the style
  // string is "480px", not "480.00px".
  function isWhole(v) {
    return Math.abs(v - Math.round(v)) < 1e-6;
  }

  function px(v) {
    return (Math.round(v * 100) / 100) + "px";
  }

  function fact(dl, label) {
    var dt = document.createElement("dt");
    dt.textContent = label;
    var dd = document.createElement("dd");
    dd.textContent = "—";
    dl.appendChild(dt);
    dl.appendChild(dd);
    return dd;
  }

  // ---- layout -------------------------------------------------------------

  // relayout picks how big the framebuffer is drawn. Integer PHYSICAL pixels
  // per device pixel is the whole point: at 1.5 CSS pixels per device pixel
  // every other row of a 240x320 UI is doubled and the thing looks like a
  // photograph of a screen rather than the screen. Dividing the integer
  // physical scale back out by devicePixelRatio is what keeps that true on a
  // 125%/150% Windows desktop, where CSS pixels are not physical ones.
  function relayout(st) {
    var w = st.canvas.width;
    var h = st.canvas.height;
    if (!w || !h) return;

    var availW = st.stage.clientWidth - 2 * BEZEL_PAD_X - 2 * WELL_BORDER;
    if (availW <= 0) availW = 240; // not laid out yet; ResizeObserver will re-run

    // rect.top + scrollY is the mount's offset in the DOCUMENT, which does
    // not change as the operator scrolls. Deriving the height budget from the
    // viewport position instead would resize the screen mid-scroll.
    var docTop = st.stage.getBoundingClientRect().top + (window.scrollY || 0);
    var availH = window.innerHeight - docTop
      - BEZEL_PAD_TOP - BEZEL_PAD_BOT - 2 * WELL_BORDER - PLATE_H - STAGE_BOTTOM_SLACK;
    if (!(availH > 0)) availH = h;
    availH = Math.max(availH, h); // never shrink below 1:1 on account of height

    var dpr = window.devicePixelRatio || 1;
    var kMax = Math.min(
      Math.floor(availW * dpr / w),
      Math.floor(availH * dpr / h),
      Math.floor(MAX_SCALE * dpr)
    );

    // An integer number of PHYSICAL pixels is necessary but not sufficient:
    // the size still has to be expressible in CSS pixels, and 320 rows at
    // k=4 on a 150% desktop comes to 853.333…px, which the browser then
    // rounds to its own grid — one smeared row, exactly what this is all
    // for. So step k down to the largest value that lands whole in BOTH
    // units (at dpr 1.5 that is 3, not 4) and trade the size for the grid.
    // Measured on the stub harness: dpr 1.25 and 2 never need the step.
    var k = kMax;
    for (var t = kMax; t >= 1; t--) {
      if (isWhole(w * t / dpr) && isWhole(h * t / dpr)) {
        k = t;
        break;
      }
    }

    var cssW;
    var cssH;
    var label;
    if (k >= 1) {
      cssW = w * k / dpr;
      cssH = h * k / dpr;
      // The honest number for "is this crisp" is PHYSICAL pixels per device
      // pixel, so report k; the display scaling only gets a mention when it
      // makes k differ from the CSS ratio.
      label = "×" + k + (dpr === 1 ? "" : " @" + dpr + "× dpr");
    } else {
      // Narrower than the framebuffer itself: fit the width and accept the
      // resampling rather than overflowing the column.
      cssW = Math.max(80, availW);
      cssH = cssW * h / w;
      label = "×" + (cssW / w).toFixed(2) + " fit";
    }

    // Only touch the DOM when something actually moved: relayout runs from a
    // ResizeObserver, and writing identical styles would keep re-triggering it.
    var wpx = px(cssW);
    var hpx = px(cssH);
    if (st.well.style.width !== wpx || st.well.style.height !== hpx) {
      st.well.style.width = wpx;
      st.well.style.height = hpx;
    }
    st.scale = k >= 1 ? k : cssW / w;
    if (st.scaleLabel !== label) {
      st.scaleLabel = label;
      renderChrome(st);
    }
  }

  function watchLayout(st) {
    if (typeof ResizeObserver === "function") {
      st.resizeObserver = new ResizeObserver(function () { relayout(st); });
      st.resizeObserver.observe(st.stage);
    }
    st.onWindowResize = function () { relayout(st); };
    window.addEventListener("resize", st.onWindowResize);
  }

  // ---- chrome -------------------------------------------------------------

  // renderChrome is the single place every piece of state becomes pixels.
  // It is called on every frame, so it must stay allocation-light and must
  // not read layout (setText/setClass both no-op when nothing changed).
  function renderChrome(st) {
    var live = st.conn === "live";
    var canDrive = live && st.controlling;

    // link chip + facts
    setClass(st.linkChip, "live", live);
    setClass(st.linkChip, "busy", st.conn === "connecting");
    setClass(st.linkChip, "down", st.conn === "offline");
    setText(st.linkText, live ? "LIVE" : (st.conn === "connecting" ? "CONNECTING" : "OFFLINE"));
    setText(st.factLink, live ? "live" : st.conn);
    setText(st.factFrames, st.frames > 0 ? String(st.frames) : "—");
    setText(st.factViewers, st.viewers > 0 ? String(st.viewers) : "—");
    setText(st.factScale, st.scaleLabel || "—");
    setText(st.factCodec, CAN_INFLATE ? "deflate" : "raw rgb565");

    // input chip: the honest three states — driving, watching, or nothing to
    // drive yet. "READ-ONLY" is not a failure, it is another tab holding it.
    setClass(st.inputChip, "live", canDrive && st.focused);
    setClass(st.inputChip, "busy", false);
    setClass(st.inputChip, "down", live && !st.controlling);
    setText(st.inputText,
      !live ? "NO LINK"
        : !st.controlling ? "READ-ONLY"
          : st.focused ? "INPUT LIVE" : "INPUT IDLE");

    setText(st.warn, st.backendError ? "⚠ " + st.backendError : "");

    // the device itself
    var lit = st.frames > 0;
    setClass(st.well, "lit", lit);
    setClass(st.well, "focus", st.focused);
    setClass(st.well, "hot", canDrive && st.focused);
    setClass(st.plateDot, "on", lit);
    setText(st.plateText, "B200 REMOTE · " + st.canvas.width + "×" + st.canvas.height
      + (st.scaleLabel ? " · " + st.scaleLabel : ""));

    // app identity, from whatever the shell knows
    setText(st.appHead, st.host.appName || "DEVICE SCREEN");
    setText(st.appName, st.host.appName || "This app");
    // panel_kind "screen" IS this view — saying "SCREEN · STREAMED SCREEN"
    // would be the shell talking to itself. Any other kind means the app does
    // have a native panel and the operator chose the raw device instead.
    setText(st.appKind, st.host.panelKind && st.host.panelKind !== "screen"
      ? st.host.panelKind.toUpperCase() + " PANEL AVAILABLE · SHOWING DEVICE SCREEN"
      : "NO NATIVE PANEL · STREAMED SCREEN");

    // the teaching overlay: only where it is both true and useful — there
    // are pixels to click on, this client may drive them, and it doesn't.
    var showCta = lit && canDrive && !st.focused && !hintUsedOnce;
    setClass(st.cta, "hidden", !showCta);

    // key map + note card
    setClass(st.keyCard, "armed", canDrive && st.focused);
    setClass(st.keyCard, "idle", !(canDrive && st.focused));
    setClass(st.noteCard, "armed", canDrive && st.focused);
    setText(st.inputNote, inputNoteText(st, live, canDrive, lit));
  }

  function inputNoteText(st, live, canDrive, lit) {
    if (!live) {
      return st.conn === "connecting"
        ? "Connecting to the portal's screen stream… nothing is being sent."
        : "Disconnected from the portal's screen stream. Reconnecting automatically — "
          + "keys and clicks are not being sent.";
    }
    if (!st.controlling) {
      return "Read-only. Another browser holds control of this device, so keys, "
        + "wheel and clicks here are watched but never sent.";
    }
    if (st.focused) {
      return "Input live — arrows, Enter, Esc, the wheel and clicks are going to the "
        + "device. Click outside the screen, or press Tab, to release it.";
    }
    if (!lit) {
      // The canvas is display:none until the first frame lands, so it is not
      // in the tab order yet: telling someone to Tab to it is an instruction
      // they cannot carry out. Say what is actually true instead.
      return "Waiting for the first frame. The screen becomes clickable — and "
        + "reachable with Tab — as soon as the device sends one.";
    }
    return "Click the screen (or Tab to it) to capture the keyboard. Nothing is sent "
      + "until it has focus — the border turns green when it does.";
  }

  function setConn(st, conn) {
    st.conn = conn;
    renderChrome(st);
  }

  function setNote(st, text) {
    if (text) {
      st.note.textContent = text;
      st.note.classList.remove("hidden");
      // Once there are real pixels, an error must not cover them: the last
      // good frame is still the truest thing on the page.
      setClass(st.note, "banner", st.frames > 0);
    } else {
      st.note.classList.add("hidden");
    }
  }

  // refreshHost re-reads the shell's idea of which app is open. Cheap enough
  // to run on the same 1s keepalive as the mount check; app.js can change the
  // title mid-stream (a panel poll returning a new title) and this view would
  // otherwise keep naming the app the operator left.
  function refreshHost(st) {
    if (st.hostPinned) return; // an explicit mount(el, ctx) wins over the DOM
    var h = hostContext();
    if (h.appName === st.host.appName && h.panelKind === st.host.panelKind) return;
    st.host = h;
    renderChrome(st);
  }

  // ---- frames -------------------------------------------------------------

  function onBinary(st, buf) {
    var bytes = new Uint8Array(buf);
    if (bytes.length < SCREEN_HEADER) return;
    if (bytes[0] !== 0x4d || bytes[1] !== 0x42 || bytes[2] !== 0x53 || bytes[3] !== 0x46) {
      return; // not "MBSF": not ours, and guessing at it would draw garbage
    }
    var version = bytes[4];
    var format = bytes[5];
    if (version !== FRAME_VERSION) {
      setNote(st, "This portal does not understand frame version " + version + " from mayhem-b200.");
      return;
    }
    var view = new DataView(buf);
    var w = view.getUint16(6, true);
    var h = view.getUint16(8, true);
    var seq = view.getUint32(10, true);
    var payload = new Uint8Array(buf, SCREEN_HEADER);

    if (format === FMT_RGB565) {
      paintFrame(st, w, h, seq, payload);
      return;
    }
    if (format === FMT_DEFLATE) {
      if (!CAN_INFLATE) {
        // Should not happen: this client asked for ?deflate=0.
        setNote(st, "mayhem-b200 sent a compressed frame this browser cannot decompress.");
        return;
      }
      inflateRaw(payload).then(function (out) {
        paintFrame(st, w, h, seq, out);
      }).catch(function (err) {
        setNote(st, "Could not decompress a screen frame: " + (err && err.message ? err.message : err));
      });
      return;
    }
    setNote(st, "Unknown screen frame format " + format + ".");
  }

  // paintFrame converts RGB565 to RGBA and blits, synchronously, right here
  // in the message handler. See the file header for why this is not
  // scheduled.
  function paintFrame(st, w, h, seq, payload) {
    if (st.stopped || !st.canvas.isConnected) return;
    if (w <= 0 || h <= 0) return;
    if (payload.length < w * h * 2) return; // truncated: draw nothing rather than half a screen
    if (st.paintedSeq !== null && (seq === st.paintedSeq || seqIsOlder(seq, st.paintedSeq))) {
      return; // an inflate that finished out of order
    }

    var resized = false;
    if (st.canvas.width !== w || st.canvas.height !== h) {
      st.canvas.width = w;
      st.canvas.height = h;
      st.imageData = null;
      resized = true;
    }
    if (!st.imageData) st.imageData = st.ctx.createImageData(w, h);

    var out = st.imageData.data;
    var n = w * h;
    var si = 0;
    var di = 0;
    for (var i = 0; i < n; i++) {
      var v = payload[si] | (payload[si + 1] << 8); // little-endian RGB565
      si += 2;
      var r = (v >> 11) & 0x1f;
      var g = (v >> 5) & 0x3f;
      var b = v & 0x1f;
      // Replicate the high bits into the low ones so full-scale stays full
      // scale (0x1f -> 0xff), the same expansion the C++ compositor uses.
      out[di++] = (r << 3) | (r >> 2);
      out[di++] = (g << 2) | (g >> 4);
      out[di++] = (b << 3) | (b >> 2);
      out[di++] = 255;
    }
    st.ctx.putImageData(st.imageData, 0, 0);

    st.paintedSeq = seq;
    st.frames++;
    if (st.frames === 1) {
      st.canvas.classList.remove("hidden");
      setNote(st, "");
    }
    // The bezel is sized from the framebuffer, so a device that changes
    // resolution mid-stream re-fits instead of stretching.
    if (resized || st.frames === 1) relayout(st);
    renderChrome(st);
  }

  // ---- socket -------------------------------------------------------------

  function socketURL() {
    var proto = window.location.protocol === "https:" ? "wss:" : "ws:";
    var url = proto + "//" + window.location.host + "/api/screen/ws";
    if (!CAN_INFLATE) url += "?deflate=0";
    return url;
  }

  function connect(st) {
    if (st.stopped || !st.el.isConnected) return;
    var ws;
    try {
      ws = new WebSocket(socketURL());
    } catch (e) {
      scheduleReconnect(st);
      return;
    }
    ws.binaryType = "arraybuffer";
    st.ws = ws;
    setConn(st, "connecting");

    ws.onopen = function () {
      st.retryMs = RECONNECT_MIN_MS;
      setConn(st, "live");
    };
    ws.onmessage = function (ev) {
      if (typeof ev.data === "string") {
        onText(st, ev.data);
      } else {
        onBinary(st, ev.data);
      }
    };
    ws.onclose = function () {
      if (st.ws === ws) st.ws = null;
      // Whatever was held down is not held any more as far as this page is
      // concerned; forget it so a reconnect doesn't send a stray key-up.
      st.held.clear();
      st.pending.length = 0;
      st.controlling = false;
      setConn(st, "offline");
      if (st.frames === 0) setNote(st, "Not connected to the portal's screen stream.");
      scheduleReconnect(st);
    };
    ws.onerror = function () {
      // onclose always follows; nothing useful to add here.
    };
  }

  function scheduleReconnect(st) {
    if (st.stopped || !st.el.isConnected) return;
    if (st.reconnectTimer) return;
    var delay = st.retryMs;
    st.retryMs = Math.min(RECONNECT_MAX_MS, st.retryMs * 2);
    st.reconnectTimer = window.setTimeout(function () {
      st.reconnectTimer = 0;
      connect(st);
    }, delay);
  }

  function onText(st, text) {
    var msg;
    try {
      msg = JSON.parse(text);
    } catch (e) {
      return;
    }
    if (!msg || msg.type !== "status") return;
    var wasControlling = st.controlling;
    st.controlling = !!msg.controlling;
    st.viewers = msg.viewers | 0;
    st.backendError = msg.backend_error ? String(msg.backend_error) : "";
    if (wasControlling && !st.controlling) {
      // Demoted: drop anything queued rather than replaying it later.
      st.held.clear();
      st.pending.length = 0;
    }
    if (st.backendError && st.frames === 0) {
      setNote(st, st.backendError + " — there is no screen to show yet.");
    }
    renderChrome(st);
  }

  // ---- input --------------------------------------------------------------

  function send(st, event, immediate) {
    if (!st.controlling) return; // read-only viewers send nothing at all
    // Coalesce pointer drags: only the latest position matters, and a drag
    // can otherwise emit a hundred events between two flushes.
    if (event.type === "touch" && event.phase === "move") {
      var last = st.pending[st.pending.length - 1];
      if (last && last.type === "touch" && last.phase === "move") {
        st.pending[st.pending.length - 1] = event;
        scheduleFlush(st, immediate);
        return;
      }
    }
    st.pending.push(event);
    if (st.pending.length > INPUT_QUEUE_MAX) {
      st.pending.splice(0, st.pending.length - INPUT_QUEUE_MAX);
    }
    scheduleFlush(st, immediate);
  }

  function scheduleFlush(st, immediate) {
    if (immediate) {
      flush(st);
      return;
    }
    if (st.flushTimer) return;
    st.flushTimer = window.setTimeout(function () {
      st.flushTimer = 0;
      flush(st);
    }, INPUT_FLUSH_MS);
  }

  function flush(st) {
    if (st.flushTimer) {
      window.clearTimeout(st.flushTimer);
      st.flushTimer = 0;
    }
    if (st.pending.length === 0) return;
    if (!st.ws || st.ws.readyState !== 1 /* OPEN */) {
      st.pending.length = 0;
      return;
    }
    try {
      st.ws.send(JSON.stringify({ events: st.pending }));
    } catch (e) {
      // A send that fails means the socket is going away; onclose handles it.
    }
    st.pending.length = 0;
  }

  // releaseHeldKeys sends the key-ups for anything still down. The device
  // tracks long presses from real down/up pairs (ui::key_is_long_pressed), so
  // a key that is never released stays latched down inside the app forever.
  function releaseHeldKeys(st) {
    if (st.held.size === 0) return;
    st.held.forEach(function (k) {
      send(st, { type: "key", key: k, down: false }, false);
    });
    st.held.clear();
    flush(st);
  }

  function onKeyDown(st, e) {
    // Leave browser and OS shortcuts alone.
    if (e.ctrlKey || e.metaKey || e.altKey) return;

    var mapped = KEY_MAP[e.key];
    if (mapped) {
      e.preventDefault(); // arrows would scroll the page, Enter would re-click
      // Auto-repeat is suppressed deliberately: the device sees one press
      // and one release, which is what makes its own long-press detection
      // and repeat behaviour work. Synthesising a stream of down/up pairs
      // here would override the app's own idea of a held key.
      if (e.repeat) return;
      st.held.add(mapped);
      send(st, { type: "key", key: mapped, down: true }, true);
      return;
    }

    if (e.key.length === 1) {
      var code = e.key.codePointAt(0);
      // Contract 2's char range. Anything outside it has no representation
      // on the device's keyboard, so it is dropped rather than mangled.
      if (code >= 0x20 && code <= 0xff) {
        e.preventDefault();
        // Char repeats are NOT suppressed: holding a letter to type it
        // several times is ordinary text entry, and each repeat is a real
        // new character rather than a still-held button.
        send(st, { type: "char", c: code }, true);
      }
    }
  }

  function onKeyUp(st, e) {
    var mapped = KEY_MAP[e.key];
    if (!mapped) return;
    e.preventDefault();
    if (!st.held.delete(mapped)) return; // never pressed here (e.g. focus arrived mid-press)
    send(st, { type: "key", key: mapped, down: false }, true);
  }

  function onWheel(st, e) {
    if (document.activeElement !== st.canvas) return; // unfocused: let the page scroll
    e.preventDefault();
    var perNotch = e.deltaMode === 1 ? WHEEL_LINES_PER_NOTCH
      : (e.deltaMode === 2 ? 1 : WHEEL_PX_PER_NOTCH);
    st.wheelAcc += e.deltaY / perNotch;
    var detents = 0;
    while (st.wheelAcc >= 1) {
      st.wheelAcc -= 1;
      detents--;
    }
    while (st.wheelAcc <= -1) {
      st.wheelAcc += 1;
      detents++;
    }
    // Sign matches the device's own host layer: a wheel notch away from the
    // user (deltaY < 0) is a positive encoder detent, as in window.cpp's
    // WM_MOUSEWHEEL handling.
    if (detents !== 0) send(st, { type: "encoder", delta: detents }, true);
  }

  // pointToScreen maps a browser coordinate onto the framebuffer, correctly
  // at any CSS scale (the canvas is displayed larger than its 240x320
  // backing store), including a page zoom or a responsive shrink.
  function pointToScreen(st, e) {
    var r = st.canvas.getBoundingClientRect();
    if (!r.width || !r.height) return null;
    var x = Math.floor((e.clientX - r.left) * st.canvas.width / r.width);
    var y = Math.floor((e.clientY - r.top) * st.canvas.height / r.height);
    return {
      x: clamp(x, 0, st.canvas.width - 1),
      y: clamp(y, 0, st.canvas.height - 1),
    };
  }

  function onPointerDown(st, e) {
    if (e.button !== undefined && e.button !== 0) return;
    st.canvas.focus();
    var p = pointToScreen(st, e);
    if (!p) return;
    e.preventDefault();
    st.pointerId = e.pointerId;
    if (st.canvas.setPointerCapture) {
      try {
        st.canvas.setPointerCapture(e.pointerId);
      } catch (err) {
        // Capture is an optimisation; a drag that leaves the canvas just ends.
      }
    }
    send(st, { type: "touch", x: p.x, y: p.y, phase: "start" }, true);
  }

  function onPointerMove(st, e) {
    if (st.pointerId === null || e.pointerId !== st.pointerId) return;
    var p = pointToScreen(st, e);
    if (!p) return;
    e.preventDefault();
    send(st, { type: "touch", x: p.x, y: p.y, phase: "move" }, false);
  }

  function onPointerEnd(st, e) {
    if (st.pointerId === null || e.pointerId !== st.pointerId) return;
    var p = pointToScreen(st, e);
    e.preventDefault();
    st.pointerId = null;
    if (st.canvas.releasePointerCapture) {
      try {
        st.canvas.releasePointerCapture(e.pointerId);
      } catch (err) {
        // Already released (pointercancel); nothing to do.
      }
    }
    if (p) send(st, { type: "touch", x: p.x, y: p.y, phase: "end" }, true);
  }

  function wireInput(st) {
    var c = st.canvas;
    c.addEventListener("keydown", function (e) { onKeyDown(st, e); });
    c.addEventListener("keyup", function (e) { onKeyUp(st, e); });
    // passive:false so preventDefault actually stops the page scrolling.
    c.addEventListener("wheel", function (e) { onWheel(st, e); }, { passive: false });
    c.addEventListener("pointerdown", function (e) { onPointerDown(st, e); });
    c.addEventListener("pointermove", function (e) { onPointerMove(st, e); });
    c.addEventListener("pointerup", function (e) { onPointerEnd(st, e); });
    c.addEventListener("pointercancel", function (e) { onPointerEnd(st, e); });
    c.addEventListener("contextmenu", function (e) { e.preventDefault(); });
    c.addEventListener("focus", function () {
      st.focused = true;
      // The lesson has landed: from here on the state is shown, not taught.
      hintUsedOnce = true;
      renderChrome(st);
    });
    c.addEventListener("blur", function () {
      st.focused = false;
      renderChrome(st);
      // Losing focus mid-press must not leave the key held on the device.
      releaseHeldKeys(st);
    });

    // Closing the tab or navigating away while a key is down would otherwise
    // leave it latched down inside the app forever: the socket dies without
    // a key-up, and the device has no way to know the finger left. This is
    // the same thing the device's own host layer does on WM_KILLFOCUS
    // (input::reset()), and it reports reality rather than inventing input —
    // the user is demonstrably not pressing anything any more.
    st.onPageHide = function () { releaseHeldKeys(st); };
    window.addEventListener("pagehide", st.onPageHide);
  }

  // ---- lifecycle ----------------------------------------------------------

  // mount builds the view. ctx is optional presentation context — currently
  // {appName, panelKind} — for a caller that already knows what it launched;
  // without it the same two facts are read from the portal shell's own header
  // (see hostContext), so app.js needs no change to get an app-labelled view.
  function mount(el, ctx) {
    if (!el) return null;
    if (el.__mbScreenView) return el.__mbScreenView;
    var st = build(el, ctx);
    el.__mbScreenView = st;
    wireInput(st);
    refreshHost(st);
    relayout(st);
    watchLayout(st);
    renderChrome(st);
    connect(st);
    // PANELS.md's contract has no destroy hook, and app.js can clear this
    // mount without telling anyone (a panel switch, a navigation back to the
    // grid). Watching isConnected is how every other view in this portal
    // notices, and it is what stops an orphaned socket streaming 150kB
    // frames into a detached canvas forever.
    st.aliveTimer = window.setInterval(function () {
      if (!el.isConnected) {
        unmount(el);
        return;
      }
      refreshHost(st);
    }, 1000);
    return st;
  }

  function unmount(el) {
    if (!el) return;
    var st = el.__mbScreenView;
    if (!st) return;
    st.stopped = true;
    // Best effort: if the socket is still open, hand back any held keys
    // before dropping it.
    releaseHeldKeys(st);
    if (st.onPageHide) window.removeEventListener("pagehide", st.onPageHide);
    if (st.onWindowResize) window.removeEventListener("resize", st.onWindowResize);
    if (st.resizeObserver) st.resizeObserver.disconnect();
    if (st.reconnectTimer) window.clearTimeout(st.reconnectTimer);
    if (st.flushTimer) window.clearTimeout(st.flushTimer);
    if (st.aliveTimer) window.clearInterval(st.aliveTimer);
    if (st.ws) {
      try {
        st.ws.close();
      } catch (e) {
        // Already closing.
      }
      st.ws = null;
    }
    delete el.__mbScreenView;
  }

  window.MayhemScreenView = {
    mount: mount,
    unmount: unmount,
    // Exposed for diagnostics only (and so a page can explain itself):
    // whether this browser can decompress format 2 frames.
    canInflate: CAN_INFLATE,
  };
})();
