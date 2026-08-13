/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Part of mayhem-b200. */

/*
 * The animated app-icon set: window.MayhemIcons.
 *
 * ---------------------------------------------------------------------------
 * PUBLIC API  (this is the contract the app grid codes against)
 * ---------------------------------------------------------------------------
 *
 *   MayhemIcons.render(appId, categoryName) -> SVGSVGElement | null
 *
 *     The one call a tile needs. `appId` is the `id` field from GET /api/apps
 *     ("adsbrx", "pocsag_tx", ...); `categoryName` is that app's `category`
 *     ("Receive", "Transmit", ... -- matched case-insensitively). Returns a
 *     fresh, detached <svg> ready to append; the caller owns it and may append
 *     it exactly once.
 *
 *     Resolution order: the app's own icon, else the category's default icon,
 *     else null. null means "this set has nothing honest to show for that
 *     app" -- an app id it has never heard of in a category it has never heard
 *     of -- and the grid should fall back to whatever it drew before. It is
 *     never thrown, and never a broken or placeholder graphic.
 *
 *   MayhemIcons.renderKind(kind) -> SVGSVGElement | null
 *
 *     Same, addressing an icon by its own name instead of by app. For the
 *     chrome around the grid that has no app behind it: renderKind("magnifier")
 *     for the empty-search state, renderKind("antenna-rx") for a section
 *     heading. Unknown kind -> null.
 *
 *   MayhemIcons.kindFor(appId, categoryName) -> string | null
 *
 *     The resolution above without building anything. Mostly for tests and for
 *     a caller that wants to key its own styling off the icon choice.
 *
 *   MayhemIcons.kinds()   -> string[]   every icon name, sorted
 *   MayhemIcons.appIds()  -> string[]   every app id with a bespoke icon
 *   MayhemIcons.ensureStyles()          idempotent; see "styles" below
 *   MayhemIcons.VERSION   -> string
 *
 * ---------------------------------------------------------------------------
 * COLOUR
 * ---------------------------------------------------------------------------
 * Every stroke and fill in every icon is currentColor. An icon has no colour
 * of its own at all, so the grid colours it by state purely in CSS -- set
 * `color` on the tile for native/TX/running/disabled and the icon follows,
 * including on hover and including through a disabled tile's opacity. Relative
 * weight inside an icon is carried by per-element `opacity`, never by a second
 * colour, so this keeps working against any accent the theme picks.
 *
 * ---------------------------------------------------------------------------
 * MOTION
 * ---------------------------------------------------------------------------
 * All of it is CSS, from the mp* keyframe library the design ships. An icon
 * carries motion classes (.mpi-pulse, .mpi-bar, ...) defined in icons.css; no
 * icon runs a timer, a rAF loop, or any script after it is built. That is what
 * lets `prefers-reduced-motion` switch the whole set off from one media query
 * in icons.css, and lets the host retime everything by setting --ics/--icp on
 * an ancestor (see icons.css for those two knobs).
 *
 * Cost per icon is deliberately small: a handful of elements, of which one to
 * three animate, and only ever `transform` and `opacity` -- nothing that
 * invalidates layout. See the note above ART for the rules each icon follows.
 *
 * ---------------------------------------------------------------------------
 * STYLES
 * ---------------------------------------------------------------------------
 * The host is expected to link icons.css, and the theme is expected to define
 * the mp* keyframes in app.css. ensureStyles() runs once on the first render
 * and repairs either of those if it finds them missing -- it links icons.css
 * relative to this script, and injects the keyframe library only when the
 * document defines no mpPulse at all. Both checks are additive: they cannot
 * override a definition that is already present, so a host that wires
 * everything up correctly never notices this code ran.
 */

(() => {
  "use strict";

  const VERSION = "1.0.0";

  // Captured at load: document.currentScript is null by the time a render
  // happens, and it is the only reliable way to find icons.css next to this
  // file regardless of where the page lives.
  const SCRIPT_SRC = (document.currentScript && document.currentScript.src) || "";

  // --- the art ---------------------------------------------------------------
  //
  // One entry per icon kind: the *children* of the <svg>, as markup. The root
  // element is added by buildPrototype() below, which also sets the shared
  // stroke presentation, so an entry only overrides what differs from it.
  //
  // Rules every entry follows, because they are what make 94 of these cheap
  // and safe to clone:
  //
  //   - viewBox is 0 0 24 24 for all of them. The mp* keyframes are written in
  //     px against that box (mpScan travels 13, mpSlideX 2.6, mpFall 3, mpBob
  //     1.4), and inside an SVG those are user units -- so the library only
  //     lines up at this one size. Keep drawings inside roughly 2..22 so a
  //     ping ring or a scan bar has somewhere to travel.
  //   - No `id` attributes, anywhere. Every icon is cloned per tile; an id
  //     would be duplicated 94 times and any url(#...) reference would silently
  //     bind to whichever copy the browser saw first. That also rules out
  //     <clipPath>, <mask> and gradients -- hence the opacity-only shading.
  //   - Only currentColor. See the COLOUR note above.
  //   - Animated elements need a legible resting state, because reduced motion
  //     strips the animation and leaves the attributes: a pulsing element gets
  //     a mid opacity, a ping ring a faint one, a bar its full height.
  //   - Pivots are declared inline as --o in viewBox coordinates when they are
  //     not the centre (icons.css explains why).
  const ART = {
    // -- category defaults ----------------------------------------------------

    // Receive: mast under arriving wavefronts. Outermost arc pulses first, so
    // the motion reads inward.
    "antenna-rx": `
      <path d="M12 20.6V9.4"/>
      <path d="M8.6 20.6h6.8"/>
      <circle cx="12" cy="9.4" r="1.1" fill="currentColor" stroke="none"/>
      <path class="mpi-pulse" opacity=".5" d="M4.8 9.4a7.2 7.2 0 0 1 14.4 0"/>
      <path class="mpi-pulse mpi-d1" opacity=".5" d="M6.8 9.4a5.2 5.2 0 0 1 10.4 0"/>
      <path class="mpi-pulse mpi-d2" opacity=".5" d="M8.8 9.4a3.2 3.2 0 0 1 6.4 0"/>`,

    // Transmit: the same mast throwing side lobes, inner pair first so the
    // motion reads outward. Paired arcs share one group each -- two animations
    // instead of four, and the two sides stay in phase by construction.
    "antenna-tx": `
      <path d="M12 20.6V9.4"/>
      <path d="M8.6 20.6h6.8"/>
      <g class="mpi-pulse" opacity=".55">
        <path d="M9.8 6.9a3.3 3.3 0 0 0 0 5"/>
        <path d="M14.2 6.9a3.3 3.3 0 0 1 0 5"/>
      </g>
      <g class="mpi-pulse mpi-d2" opacity=".5">
        <path d="M7.4 5.2a6.4 6.4 0 0 0 0 8.4"/>
        <path d="M16.6 5.2a6.4 6.4 0 0 1 0 8.4"/>
      </g>
      <circle cx="12" cy="9.4" r="1.3" fill="currentColor" stroke="none" class="mpi-led"/>`,

    // Transceiver: traffic both ways, the two arrows sliding in opposition.
    transceiver: `
      <g class="mpi-slide">
        <path d="M3.6 9.4h14.2"/>
        <path d="M14.8 6.2l3.2 3.2-3.2 3.2"/>
      </g>
      <g class="mpi-slide mpi-rev">
        <path d="M20.4 15.4H6.2"/>
        <path d="M9.2 12.2 6 15.4l3.2 3.2"/>
      </g>`,

    // Home: the launcher itself.
    home: `
      <path d="M3.6 10.8 12 4.2l8.4 6.6"/>
      <path d="M5.8 12.4v7.4h12.4v-7.4"/>
      <rect x="10.1" y="14.8" width="3.8" height="5" rx=".6" fill="currentColor" stroke="none" opacity=".5" class="mpi-pulse"/>`,

    // Utilities: toolbox, latch pulsing.
    tool: `
      <rect x="3" y="9" width="18" height="10.8" rx="1.8"/>
      <path d="M8.6 9V7.2a1.6 1.6 0 0 1 1.6-1.6h3.6a1.6 1.6 0 0 1 1.6 1.6V9"/>
      <path d="M3 13.4h18" opacity=".45"/>
      <rect x="10.4" y="11.6" width="3.2" height="3.6" rx=".8" fill="currentColor" stroke="none" opacity=".6" class="mpi-pulse"/>`,

    // Settings: three sliders, thumbs drifting out of step.
    sliders: `
      <path d="M3.4 7h17.2M3.4 12h17.2M3.4 17h17.2" opacity=".55"/>
      <circle cx="8.4" cy="7" r="2.1" fill="currentColor" stroke="none" class="mpi-slide"/>
      <circle cx="15.2" cy="12" r="2.1" fill="currentColor" stroke="none" class="mpi-slide mpi-rev mpi-d1"/>
      <circle cx="10" cy="17" r="2.1" fill="currentColor" stroke="none" class="mpi-slide mpi-d2"/>`,

    // Debug.
    bug: `
      <path d="M8.6 8.4a3.4 3.4 0 0 1 6.8 0"/>
      <rect x="7.4" y="8.4" width="9.2" height="10.4" rx="4.6"/>
      <path d="M7.4 11.4H4M7.4 14.6H3.4M7.4 17.6l-2.8 2M16.6 11.4H20M16.6 14.6h3.6M16.6 17.6l2.8 2" opacity=".55"/>
      <path d="M9.8 5.9 8.6 4M14.2 5.9 15.4 4" opacity=".55"/>
      <circle cx="12" cy="12.6" r="1.4" fill="currentColor" stroke="none" class="mpi-blink"/>`,

    // -- specific apps --------------------------------------------------------

    // ADS-B, both directions. Plan-view airframe over an interrogation ring.
    aircraft: `
      <circle cx="12" cy="12" r="8.2" opacity=".28" class="mpi-ping"/>
      <path class="mpi-bob" fill="currentColor" stroke="none"
            d="M12 2.6 13.3 8.6 20.4 12.4v1.9l-7.1-2v4.1l2.4 1.8v1.5L12 18.6l-3.7 1.1v-1.5l2.4-1.8v-4.1l-7.1 2v-1.9l7.1-3.8z"/>`,

    // AIS.
    ship: `
      <g class="mpi-bob">
        <path d="M3.4 15.2h17.2l-2.4 4.4a2 2 0 0 1-1.8 1H7.6a2 2 0 0 1-1.8-1z" fill="currentColor" stroke="none" opacity=".85"/>
        <path d="M8.4 15.2V10.4h5.4l2.6 4.8"/>
        <path d="M12 10.4V5.4"/>
        <path d="M12 6 16 6l-4 2.2z" fill="currentColor" stroke="none" opacity=".6"/>
      </g>
      <path d="M3.2 12.6h3M17.8 12.6h3" opacity=".45" class="mpi-slide"/>`,

    // Recon / scanner / anything that sweeps a band.
    radar: `
      <circle cx="12" cy="12" r="8.4"/>
      <circle cx="12" cy="12" r="4.2" opacity=".4"/>
      <g class="mpi-spin">
        <path d="M12 12 12 3.6A8.4 8.4 0 0 1 18.4 6.6Z" fill="currentColor" stroke="none" opacity=".22"/>
        <path d="M12 12V3.6" opacity=".75"/>
      </g>
      <circle cx="15.4" cy="8.6" r="1.3" fill="currentColor" stroke="none" class="mpi-blink"/>`,

    // Search and the empty-results state.
    magnifier: `
      <circle cx="10.6" cy="10.6" r="6.4"/>
      <path d="M15.3 15.3 19.8 19.8"/>
      <path d="M7.6 10.6h6" opacity=".5" class="mpi-slide"/>`,

    // Looking Glass. The signature icon of the set, so it gets six bars.
    spectrum: `
      <path d="M2.6 20.4h18.8" opacity=".45"/>
      <g fill="currentColor" stroke="none">
        <rect x="3.4" y="15.4" width="2" height="5" rx=".7" class="mpi-bar"/>
        <rect x="6.6" y="11.4" width="2" height="9" rx=".7" class="mpi-bar mpi-d1"/>
        <rect x="9.8" y="6.6" width="2" height="13.8" rx=".7" class="mpi-bar mpi-d2"/>
        <rect x="13" y="9.4" width="2" height="11" rx=".7" class="mpi-bar mpi-d3"/>
        <rect x="16.2" y="13.4" width="2" height="7" rx=".7" class="mpi-bar mpi-d1"/>
        <rect x="19.4" y="16.6" width="2" height="3.8" rx=".7" class="mpi-bar mpi-d4"/>
      </g>`,

    // Waterfall designer: rows scrolling down the display.
    waterfall: `
      <rect x="3" y="3.4" width="18" height="17.2" rx="1.8" opacity=".55"/>
      <g opacity=".6">
        <path d="M5.4 6.6h13.2" stroke-dasharray="3 1.6 5 2 2.4" class="mpi-fall"/>
        <path d="M5.4 10.2h13.2" stroke-dasharray="2 2 6 1.6 3" class="mpi-fall mpi-d1"/>
        <path d="M5.4 13.8h13.2" stroke-dasharray="5 2 2 2 4" class="mpi-fall mpi-d2"/>
        <path d="M5.4 17.4h13.2" stroke-dasharray="3 2 4 2 3" class="mpi-fall mpi-d3"/>
      </g>`,

    // gfxEQ: bars off a centre line rather than off the floor.
    equalizer: `
      <g fill="currentColor" stroke="none">
        <rect x="4" y="7.4" width="2.4" height="9.2" rx="1.2" class="mpi-bar-c"/>
        <rect x="8.4" y="4.6" width="2.4" height="14.8" rx="1.2" class="mpi-bar-c mpi-d1"/>
        <rect x="12.8" y="6.6" width="2.4" height="10.8" rx="1.2" class="mpi-bar-c mpi-d2"/>
        <rect x="17.2" y="8.6" width="2.4" height="6.8" rx="1.2" class="mpi-bar-c mpi-d3"/>
      </g>`,

    speaker: `
      <path d="M4.4 9.2h3.2L12 5.4v13.2L7.6 14.8H4.4z" fill="currentColor" stroke="none" opacity=".85"/>
      <path d="M14.8 9.4a4 4 0 0 1 0 5.2" opacity=".55" class="mpi-pulse"/>
      <path d="M17.4 7a7.6 7.6 0 0 1 0 10" opacity=".5" class="mpi-pulse mpi-d2"/>`,

    mic: `
      <rect x="9.2" y="2.8" width="5.6" height="10.4" rx="2.8"/>
      <path d="M6.2 11.4a5.8 5.8 0 0 0 11.6 0"/>
      <path d="M12 17.2v3.4M8.8 20.6h6.4"/>
      <circle cx="12" cy="8" r="6.2" opacity=".25" class="mpi-ping" style="--o:12px 8px"/>`,

    // Every pager protocol: POCSAG, FLEX, 2-tone, BurgerPgr.
    pager: `
      <rect x="4.4" y="5.6" width="15.2" height="12.8" rx="2.2"/>
      <rect x="6.8" y="8" width="10.4" height="5" rx=".9" fill="currentColor" stroke="none" opacity=".3" class="mpi-blink"/>
      <path d="M7.4 15.8h3.2M13.4 15.8h3.2" opacity=".5"/>
      <path d="M17.4 5.6V3.4" opacity=".6"/>`,

    // ACARS and the teletype modes: a log with a live caret.
    console: `
      <rect x="3" y="4.2" width="18" height="15.6" rx="2"/>
      <path d="M3 8.2h18" opacity=".5"/>
      <circle cx="6" cy="6.2" r=".8" fill="currentColor" stroke="none" opacity=".6"/>
      <path d="M6 11.6h8.2M6 14.6h10.6" opacity=".45"/>
      <path d="M6 17.6h5" class="mpi-blink"/>`,

    // SSTV, NOAA APT, WeFax, analog TV: a frame filling in line by line.
    "image-scan": `
      <rect x="3.4" y="4.6" width="17.2" height="14.8" rx="1.8"/>
      <path d="M5.6 16.6 9.6 11.8l2.8 3.1 2.8-2.9 3.4 4.6" opacity=".45"/>
      <circle cx="8.4" cy="8.8" r="1.4" opacity=".5"/>
      <rect x="4.6" y="5.6" width="14.8" height="1.2" rx=".6" fill="currentColor" stroke="none" opacity=".75" class="mpi-scan"/>`,

    morse: `
      <g fill="currentColor" stroke="none" opacity=".6">
        <circle cx="4.2" cy="12" r="1.5" class="mpi-pulse"/>
        <rect x="7.2" y="10.5" width="6" height="3" rx="1.5" class="mpi-pulse mpi-d1"/>
        <circle cx="15.4" cy="12" r="1.5" class="mpi-pulse mpi-d2"/>
        <rect x="18.4" y="10.5" width="3.4" height="3" rx="1.5" class="mpi-pulse mpi-d3"/>
      </g>`,

    bluetooth: `
      <circle cx="12" cy="12" r="8.2" opacity=".22" class="mpi-ping"/>
      <path d="M8.6 8.4 15.4 15.6 12 18.6V5.4l3.4 3.2-6.8 6.8"/>`,

    // Radiosonde: balloon and payload, drifting.
    balloon: `
      <g class="mpi-bob">
        <path d="M12 2.8a5 5 0 0 1 5 5c0 3.2-2.8 5-5 7.6-2.2-2.6-5-4.4-5-7.6a5 5 0 0 1 5-5z"/>
        <path d="M12 15.4v1.8"/>
        <rect x="9.8" y="17.2" width="4.4" height="3.4" rx=".8" fill="currentColor" stroke="none" opacity=".8"/>
      </g>`,

    // APRS and anything that plots a position.
    pin: `
      <path d="M12 3a5.6 5.6 0 0 1 5.6 5.6c0 4.2-5.6 9.8-5.6 9.8S6.4 12.8 6.4 8.6A5.6 5.6 0 0 1 12 3z"/>
      <circle cx="12" cy="8.6" r="2" fill="currentColor" stroke="none"/>
      <ellipse cx="12" cy="20.2" rx="4.4" ry="1.5" opacity=".3" class="mpi-marker" style="--o:12px 20.2px"/>`,

    // GPS sim, wardrive: a bird overhead.
    globe: `
      <circle cx="12" cy="12" r="7.6"/>
      <ellipse cx="12" cy="12" rx="7.6" ry="3.2" opacity=".5"/>
      <path d="M12 4.4a10.4 10.4 0 0 1 0 15.2 10.4 10.4 0 0 1 0-15.2" opacity=".5"/>
      <g class="mpi-spin" style="--dur:7s">
        <circle cx="12" cy="2" r="1.3" fill="currentColor" stroke="none"/>
      </g>`,

    // Level, tuner, VOR, ERT: anything whose answer is a reading.
    gauge: `
      <path d="M4 17.4a8 8 0 1 1 16 0"/>
      <path d="M5.8 11.2 7.2 12M12 6.6V8.2M18.2 11.2 16.8 12" opacity=".5"/>
      <path d="M3.6 20.4h16.8" opacity=".45"/>
      <path d="M12 17.4V9.2" class="mpi-needle" style="--o:12px 17.4px"/>
      <circle cx="12" cy="17.4" r="1.6" fill="currentColor" stroke="none"/>`,

    clock: `
      <circle cx="12" cy="12" r="8.2"/>
      <path d="M12 7.4V12h3.2" opacity=".8"/>
      <g class="mpi-spin" style="--dur:6s">
        <path d="M12 12V4.9" opacity=".5"/>
      </g>
      <circle cx="12" cy="12" r="1.1" fill="currentColor" stroke="none"/>`,

    // Metronome.
    pendulum: `
      <path d="M9.4 3.8h5.2l4.4 16.6H5z"/>
      <g class="mpi-needle" style="--o:12px 20.4px">
        <path d="M12 20.4V6.6" opacity=".8"/>
        <rect x="10.1" y="8.6" width="3.8" height="1.8" rx=".6" fill="currentColor" stroke="none"/>
      </g>`,

    // Keyfob, KeeLoq, Security+, BHT, OOK brute, password generator.
    key: `
      <circle cx="7.4" cy="12" r="4"/>
      <circle cx="7.4" cy="12" r="1.3" fill="currentColor" stroke="none" class="mpi-blink"/>
      <path d="M11.4 12h9.2M17.6 12v3.4M14.6 12v2.4"/>`,

    lock: `
      <rect x="4.8" y="10.6" width="14.4" height="9.4" rx="2.2"/>
      <path d="M8.2 10.6V7.8a3.8 3.8 0 0 1 7.6 0v2.8" class="mpi-bob"/>
      <circle cx="12" cy="14.2" r="1.5" fill="currentColor" stroke="none"/>
      <path d="M12 15.4v2.2"/>`,

    // SD card: storage and the USB/wipe utilities.
    disk: `
      <path d="M6.2 3.4h7.4l4.2 4.2v11.2a1.8 1.8 0 0 1-1.8 1.8H6.2a1.8 1.8 0 0 1-1.8-1.8V5.2a1.8 1.8 0 0 1 1.8-1.8z"/>
      <path d="M8.2 3.6v2.6M10.4 3.6v2.6M12.6 3.6v2.6" opacity=".5"/>
      <circle cx="11.4" cy="14.4" r="1.5" fill="currentColor" stroke="none" class="mpi-led"/>`,

    record: `
      <circle cx="12" cy="12" r="8.2"/>
      <circle cx="12" cy="12" r="6.4" opacity=".3" class="mpi-ping"/>
      <circle cx="12" cy="12" r="3.6" fill="currentColor" stroke="none" opacity=".7" class="mpi-pulse"/>`,

    // Replay, soundboard, TouchTunes. --dfo is 2x the 7-unit dash period so
    // the loop closes without a seam.
    play: `
      <circle cx="12" cy="12" r="8.4" stroke-dasharray="4 3" opacity=".55" class="mpi-dash" style="--dfo:-14px"/>
      <path d="M9.8 8 16.4 12l-6.6 4z" fill="currentColor" stroke="none"/>`,

    // Jammer: broadband hash. Inset from the edges by the 2.6 mpSlideX travel
    // so the trace never runs off the box.
    noise: `
      <path d="M3.4 12h2.2l1.6-5.4 2.2 10.8 2-7.6 1.8 4.8 1.6-3.2 1.6 2.6 1.4-1.8h2.2" class="mpi-slide"/>
      <circle cx="4.6" cy="6.4" r="1" fill="currentColor" stroke="none" class="mpi-blink"/>
      <circle cx="19.4" cy="17.4" r="1" fill="currentColor" stroke="none" class="mpi-blink mpi-d2"/>`,

    // Notepad, freq manager, editors, font/PMem viewers.
    doc: `
      <path d="M6.4 3.4h7.2l4 4v11.2a1.8 1.8 0 0 1-1.8 1.8H6.4a1.8 1.8 0 0 1-1.8-1.8V5.2a1.8 1.8 0 0 1 1.8-1.8z"/>
      <path d="M13.4 3.6v4h4" opacity=".5"/>
      <path d="M7.8 11.6h7.4M7.8 14.4h7.4" opacity=".45"/>
      <path d="M7.8 17.2h2.6" class="mpi-blink"/>`,

    calc: `
      <rect x="5" y="3.2" width="14" height="17.6" rx="2.2"/>
      <rect x="7.4" y="5.6" width="9.2" height="3.6" rx=".8" fill="currentColor" stroke="none" opacity=".3"/>
      <g fill="currentColor" stroke="none" opacity=".8">
        <circle cx="8.6" cy="12.6" r="1"/>
        <circle cx="12" cy="12.6" r="1"/>
        <circle cx="15.4" cy="12.6" r="1"/>
        <circle cx="8.6" cy="16.4" r="1"/>
        <circle cx="12" cy="16.4" r="1" class="mpi-blink"/>
        <circle cx="15.4" cy="16.4" r="1"/>
      </g>`,

    // MCU temp, ext sensors, weather. The bulb is a 4.4 arc hanging off the
    // tube, so the tube stops at 12.5 to keep the whole thing above y=20.6
    // like every other icon rather than sitting low in the box.
    thermo: `
      <path d="M14.4 12.5V5.4a2.4 2.4 0 0 0-4.8 0v7.1a4.4 4.4 0 1 0 4.8 0z"/>
      <circle cx="12" cy="16.2" r="2.2" fill="currentColor" stroke="none"/>
      <rect x="11" y="7.8" width="2" height="7" rx="1" fill="currentColor" stroke="none" opacity=".8" class="mpi-bar"/>
      <path d="M16.4 7.6h2.4M16.4 10.4h2.4" opacity=".45"/>`,

    // Signal gen, AFSK, subcarrier, WAV view. Dash period is 5.2, so --dfo is
    // -10.4 for a seamless loop.
    wave: `
      <path d="M2.6 19.4h18.8" opacity=".3"/>
      <path d="M2.6 11.4q2.35-6.4 4.7 0t4.7 0 4.7 0 4.7 0" stroke-dasharray="3 2.2" class="mpi-dash" style="--dfo:-10.4px"/>`,

    // Hard reset.
    power: `
      <path d="M7.6 6.4a7.6 7.6 0 1 0 8.8 0"/>
      <path d="M12 3v8.2" opacity=".85" class="mpi-pulse"/>`,

    about: `
      <circle cx="12" cy="12" r="8.2"/>
      <circle cx="12" cy="12" r="7" opacity=".22" class="mpi-ping"/>
      <circle cx="12" cy="7.8" r="1.15" fill="currentColor" stroke="none"/>
      <path d="M12 11.2v5.4"/>`,

    // App manager.
    grid: `
      <rect x="3.4" y="3.4" width="7.4" height="7.4" rx="1.6"/>
      <rect x="13.2" y="3.4" width="7.4" height="7.4" rx="1.6"/>
      <rect x="3.4" y="13.2" width="7.4" height="7.4" rx="1.6"/>
      <rect x="13.2" y="13.2" width="7.4" height="7.4" rx="1.6" fill="currentColor" stroke="none" opacity=".5" class="mpi-pulse"/>`,

    // Antenna length, IQ trim: measuring something.
    ruler: `
      <rect x="2.6" y="8.4" width="18.8" height="7.2" rx="1.4"/>
      <path d="M6.4 8.4v3M10.2 8.4v2M14 8.4v3M17.8 8.4v2" opacity=".5"/>
      <path d="M8.8 6.4v11.2" opacity=".85" class="mpi-slide"/>`,

    folder: `
      <path d="M2.8 7.4a1.8 1.8 0 0 1 1.8-1.8h4.4l2 2.4h8.4a1.8 1.8 0 0 1 1.8 1.8v8.6a1.8 1.8 0 0 1-1.8 1.8H4.6a1.8 1.8 0 0 1-1.8-1.8z"/>
      <path d="M2.8 11.6h18.4" opacity=".4"/>
      <circle cx="17.8" cy="15.4" r="1.3" fill="currentColor" stroke="none" class="mpi-led"/>`,

    // EPIRB and SAME: distress beacons. Rings only -- nothing else to say.
    beacon: `
      <circle cx="12" cy="12" r="8.6" opacity=".3" class="mpi-ping mpi-d2"/>
      <circle cx="12" cy="12" r="5.6" opacity=".4" class="mpi-ping"/>
      <circle cx="12" cy="12" r="2.4" fill="currentColor" stroke="none"/>`,

    remote: `
      <rect x="6.4" y="4.6" width="11.2" height="15.8" rx="2.2"/>
      <rect x="8.6" y="6.6" width="6.8" height="2.2" rx=".7" fill="currentColor" stroke="none" opacity=".3"/>
      <g fill="currentColor" stroke="none" opacity=".8">
        <circle cx="9.8" cy="11.6" r="1"/>
        <circle cx="14.2" cy="11.6" r="1"/>
        <circle cx="9.8" cy="14.8" r="1"/>
        <circle cx="14.2" cy="14.8" r="1"/>
        <circle cx="9.8" cy="18" r="1"/>
        <circle cx="14.2" cy="18" r="1" class="mpi-blink"/>
      </g>
      <path d="M19 4.6a4.2 4.2 0 0 1 0 5" opacity=".55" class="mpi-pulse"/>`,

    // FM radio, RDS, radio setup: a set with a tuning dial.
    "radio-set": `
      <rect x="2.8" y="7.4" width="18.4" height="12.8" rx="2"/>
      <path d="M7.2 7.4 17.4 3.4" opacity=".6"/>
      <path d="M5.6 11.4h5.2M5.6 14h5.2M5.6 16.6h5.2" opacity=".45"/>
      <circle cx="16.6" cy="13.8" r="3.2"/>
      <path d="M16.6 13.8V11.2" class="mpi-needle" style="--o:16.6px 13.8px"/>`,
  };

  // --- app id -> icon --------------------------------------------------------
  //
  // Every id here is a real one from GET /api/apps (verified against the live
  // backend, 104 registry entries of which the portal offers 94 -- Games are
  // excluded by internal/portal/appindex). Anything not listed falls through to
  // its category default, which is the whole point of having those: a new app
  // the backend adds tomorrow gets a sensible icon with no change here.
  const BY_APP = {
    // Home
    capture: "record",
    lookingglass: "spectrum",
    recon: "radar",
    remote: "remote",
    replay: "play",

    // Receive
    two_tone_rx: "pager",
    acars_rx: "console",
    adsbrx: "aircraft",
    afsk_rx: "wave",
    ais: "ship",
    aprsrx: "pin",
    analogtv: "image-scan",
    audio: "speaker",
    blerx: "bluetooth",
    detector_rx: "magnifier",
    epirb_rx: "beacon",
    ert: "gauge",
    flex_rx: "pager",
    fmradio: "radio-set",
    fpv_detect: "radar",
    foxhunt: "radar",
    level: "gauge",
    morseradio: "morse",
    noaaapt_rx: "image-scan",
    nrf_rx: "bluetooth",
    pocsag: "pager",
    rtty_rx: "console",
    radiosonde: "balloon",
    sstvrx: "image-scan",
    scanner: "radar",
    search: "magnifier",
    signal_hunter: "magnifier",
    subcarrx: "wave",
    tetra_rx: "antenna-rx",
    time_sink: "clock",
    vor_rx: "gauge",
    wefax_rx: "image-scan",
    weather: "thermo",
    gfxeq: "equalizer",

    // Transmit
    two_tone_pager: "pager",
    adsbtx: "aircraft",
    aprstx: "pin",
    bht_tx: "key",
    bletx: "bluetooth",
    coasterp: "pager",
    epirb_tx: "beacon",
    flex_tx: "pager",
    gpssim: "globe",
    jammer: "noise",
    keeloqtx: "key",
    keyfob: "key",
    morse_tx: "morse",
    ookbrute: "key",
    ook_editor: "doc",
    ooktx: "wave",
    pocsag_tx: "pager",
    rdstx: "radio-set",
    rtty_tx: "console",
    same_tx: "beacon",
    sstvtx: "image-scan",
    secplustx: "key",
    siggen: "wave",
    soundboard: "play",
    touchtune: "play",
    vor_tx: "gauge",

    // Transceiver
    kiss_tnc: "transceiver",
    microphone: "mic",

    // Utilities
    antenna_length: "ruler",
    calculator: "calc",
    shoppingcart_lock: "lock",
    filemanager: "folder",
    freqman: "doc",
    iqtrim: "ruler",
    metronome: "pendulum",
    notepad: "doc",
    playlist_editor: "doc",
    random_password: "key",
    sdusb: "disk",
    stopwatch: "clock",
    tuner: "gauge",
    wav_view: "wave",
    wardrivemap: "globe",
    sd_wipe: "disk",
    waterfall_designer: "waterfall",

    // Settings
    about: "about",
    app_manager: "grid",
    hard_reset: "power",
    radio: "radio-set",
    settings_ui: "sliders",

    // Debug
    audio_test: "speaker",
    debug_pmem: "doc",
    extsensors: "thermo",
    font_viewer: "doc",
    mcu_temperature: "thermo",
  };

  // --- category -> default icon ----------------------------------------------
  //
  // Keys are lower-cased category names as the backend spells them (see
  // app::Category / appindex.canonicalOrder). "games" is listed even though the
  // portal excludes that category, so that lifting the exclusion never leaves a
  // section of blank tiles.
  const BY_CATEGORY = {
    home: "home",
    receive: "antenna-rx",
    transmit: "antenna-tx",
    transceiver: "transceiver",
    utilities: "tool",
    games: "grid",
    settings: "sliders",
    debug: "bug",
  };

  // The mp* keyframe library, verbatim from doc/design-ref/mayhem-portal-design.html.
  // Injected ONLY when the document defines none of it -- see ensureStyles.
  const KEYFRAME_FALLBACK = `
@keyframes mpPulse{0%,100%{opacity:.12}45%,60%{opacity:1}}
@keyframes mpBlink{0%,45%{opacity:1}55%,100%{opacity:.1}}
@keyframes mpDashFlow{to{stroke-dashoffset:var(--dfo,-44px)}}
@keyframes mpSpin{to{transform:rotate(360deg)}}
@keyframes mpBar{0%,100%{transform:scaleY(.28)}50%{transform:scaleY(1)}}
@keyframes mpScan{0%{transform:translateY(0);opacity:0}10%,90%{opacity:1}100%{transform:translateY(13px);opacity:0}}
@keyframes mpBob{0%,100%{transform:translateY(1px)}50%{transform:translateY(-1.4px)}}
@keyframes mpPing{0%{transform:scale(.35);opacity:.9}75%,100%{transform:scale(1.12);opacity:0}}
@keyframes mpNeedle{0%,100%{transform:rotate(-52deg)}50%{transform:rotate(52deg)}}
@keyframes mpSlideX{0%,100%{transform:translateX(-2.6px)}50%{transform:translateX(2.6px)}}
@keyframes mpFall{0%{transform:translateY(-3px);opacity:0}25%,75%{opacity:1}100%{transform:translateY(3px);opacity:0}}
@keyframes mpLed{0%,100%{opacity:.4;box-shadow:0 0 3px 0 currentColor}50%{opacity:1;box-shadow:0 0 10px 2px currentColor}}
@keyframes mpIn{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:none}}
@keyframes mpMarkerPing{0%{transform:scale(.3);opacity:.8}100%{transform:scale(1.6);opacity:0}}
@keyframes mpRowIn{from{background:rgba(61,245,140,.16)}to{background:transparent}}
`;

  // --- building --------------------------------------------------------------

  // One parsed prototype per kind, cloned per call. Parsing markup 94 times a
  // keystroke would be the expensive part of this module; cloneNode is not.
  const prototypes = new Map();

  function buildPrototype(kind) {
    // The HTML parser handles inline <svg> as foreign content and applies the
    // SVG attribute-adjustment table, so viewBox survives with its capital B.
    // Using a <template> rather than DOMParser keeps namespaces automatic.
    const tpl = document.createElement("template");
    tpl.innerHTML =
      '<svg class="mpi" viewBox="0 0 24 24" width="100%" height="100%"' +
      ' fill="none" stroke="currentColor" stroke-width="1.6"' +
      ' stroke-linecap="round" stroke-linejoin="round"' +
      ' aria-hidden="true" focusable="false" data-mp-icon="' + kind + '">' +
      ART[kind] +
      "</svg>";
    const svg = tpl.content.firstElementChild;
    // Defensive: a typo in ART would otherwise surface as a mystery blank tile
    // rather than as something a developer can see.
    if (!svg || svg.tagName.toLowerCase() !== "svg") return null;
    return svg;
  }

  function prototypeFor(kind) {
    if (prototypes.has(kind)) return prototypes.get(kind);
    const svg = Object.prototype.hasOwnProperty.call(ART, kind) ? buildPrototype(kind) : null;
    prototypes.set(kind, svg);
    return svg;
  }

  // --- styles ----------------------------------------------------------------

  let stylesChecked = false;

  function iconsCssApplied() {
    const probe = document.createElement("span");
    probe.className = "mpi-styles-probe";
    const host = document.body || document.documentElement;
    host.appendChild(probe);
    const marker = getComputedStyle(probe).getPropertyValue("--mpi-styles").trim();
    host.removeChild(probe);
    return marker === "1";
  }

  function keyframesDefined(name) {
    const sheets = document.styleSheets;
    for (let i = 0; i < sheets.length; i++) {
      let rules;
      try {
        rules = sheets[i].cssRules;
      } catch (e) {
        // Cross-origin stylesheet: unreadable, and not one of ours.
        continue;
      }
      if (!rules) continue;
      for (let j = 0; j < rules.length; j++) {
        const rule = rules[j];
        if (rule.type === 7 /* CSSRule.KEYFRAMES_RULE */ && rule.name === name) return true;
      }
    }
    return false;
  }

  // ensureStyles repairs a host that did not wire icons.css or the mp* keyframe
  // library up. Both repairs are additive -- neither can override a definition
  // that is already in the document -- so this is a no-op on a correctly wired
  // page. Runs once, lazily, on the first render; safe to call directly.
  function ensureStyles() {
    if (stylesChecked) return;
    stylesChecked = true;

    if (!iconsCssApplied()) {
      const link = document.createElement("link");
      link.rel = "stylesheet";
      // Resolved against this script so it works from any page path.
      link.href = SCRIPT_SRC ? new URL("icons.css", SCRIPT_SRC).href : "icons.css";
      document.head.appendChild(link);
    }

    if (!keyframesDefined("mpPulse")) {
      const style = document.createElement("style");
      style.setAttribute("data-mp-keyframes", "fallback");
      style.textContent = KEYFRAME_FALLBACK;
      document.head.appendChild(style);
    }
  }

  // --- public API ------------------------------------------------------------

  function kindFor(appId, categoryName) {
    if (typeof appId === "string") {
      const id = appId.trim();
      if (Object.prototype.hasOwnProperty.call(BY_APP, id)) return BY_APP[id];
    }
    if (typeof categoryName === "string") {
      const cat = categoryName.trim().toLowerCase();
      if (Object.prototype.hasOwnProperty.call(BY_CATEGORY, cat)) return BY_CATEGORY[cat];
    }
    return null;
  }

  function renderKind(kind) {
    if (typeof kind !== "string") return null;
    ensureStyles();
    const proto = prototypeFor(kind);
    return proto ? proto.cloneNode(true) : null;
  }

  function render(appId, categoryName) {
    const kind = kindFor(appId, categoryName);
    return kind === null ? null : renderKind(kind);
  }

  window.MayhemIcons = {
    VERSION,
    render,
    renderKind,
    kindFor,
    ensureStyles,
    kinds() {
      return Object.keys(ART).sort();
    },
    appIds() {
      return Object.keys(BY_APP).sort();
    },
    categories() {
      return Object.keys(BY_CATEGORY).sort();
    },
  };
})();
