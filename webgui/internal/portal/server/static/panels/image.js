// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// The "image" panel: the decoded picture an image-producing RX app is
// building up -- NOAA APT, WeFax, SSTV -- drawn at its native pixel size and
// CSS-scaled to fit, so the browser shows the operator more of it at once
// than the device's own 240x320 screen can. Data shape is documented in
// ../../../PANELS.md.
//
// Three things about this panel are deliberate rather than incidental:
//
//   - It NEVER synthesizes pixels. An app that has decoded nothing yet sends
//     rev 0, no data_b64 and a note; that renders as the note, not as a black
//     rectangle that reads like a picture of a black sky. Same for a payload
//     whose format this file does not understand, or whose byte count does
//     not match width*height*3 -- say what is wrong, draw nothing.
//   - It keeps the last image when an update arrives without data_b64. That
//     is the normal steady state, not an error: the client tells the backend
//     which rev it already has (GET /api/panel?have_image_rev=N) and the
//     backend omits the ~160 kB payload when it would only be re-sending it.
//   - It paints synchronously, inside render(), never from
//     requestAnimationFrame. rAF does not fire in a non-composited browser
//     pane, which is where this gets verified; the adsb panel's drawNow()
//     exists for the same reason.

(() => {
  "use strict";

  // decodeBase64 turns a base64 string into the raw bytes it encodes.
  // Returns null (rather than throwing) for anything atob rejects, so a
  // truncated or corrupt payload becomes an honest on-screen message instead
  // of an exception that leaves the panel showing stale pixels with no
  // explanation.
  function decodeBase64(b64) {
    let binary;
    try {
      binary = atob(b64);
    } catch (err) {
      return null;
    }
    const out = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) out[i] = binary.charCodeAt(i);
    return out;
  }

  function buildSkeleton(el) {
    el.innerHTML = "";
    el.classList.add("mp-panel", "mp-image-panel");

    const toolbar = document.createElement("div");
    toolbar.className = "mp-toolbar";
    const title = document.createElement("span");
    title.className = "mp-title";
    const meta = document.createElement("span");
    meta.className = "mp-count";
    const saveBtn = document.createElement("button");
    saveBtn.className = "mp-btn";
    saveBtn.type = "button";
    saveBtn.textContent = "Save PNG";
    saveBtn.disabled = true;
    toolbar.appendChild(title);
    toolbar.appendChild(meta);
    toolbar.appendChild(saveBtn);
    el.appendChild(toolbar);

    // The stage is the fit box: the canvas is sized in CSS pixels to the
    // largest whole-pixel multiple of its native size that fits inside it.
    const stage = document.createElement("div");
    stage.style.flex = "1";
    stage.style.minHeight = "0";
    stage.style.display = "flex";
    stage.style.alignItems = "center";
    stage.style.justifyContent = "center";
    stage.style.overflow = "auto";
    stage.style.padding = "10px";
    stage.style.background = "var(--mp-bg)";

    const canvas = document.createElement("canvas");
    canvas.width = 1;
    canvas.height = 1;
    canvas.style.display = "none";
    // Nearest-neighbour on purpose: these are small decoded rasters and a
    // smoothed upscale would invent detail the demodulator never produced.
    canvas.style.imageRendering = "pixelated";
    canvas.style.flex = "none";
    stage.appendChild(canvas);

    const note = document.createElement("div");
    note.className = "mp-empty";
    stage.appendChild(note);

    el.appendChild(stage);

    const st = {
      titleEl: title,
      metaEl: meta,
      saveBtn,
      stage,
      canvas,
      ctx: canvas.getContext("2d"),
      noteEl: note,
      // Rev/size of the pixels currently ON the canvas -- not of the last
      // payload seen. Everything the panel claims about the image it is
      // showing is read from these.
      haveRev: null,
      haveWidth: 0,
      haveHeight: 0,
      appName: "",
    };
    el.__mpImage = st;

    saveBtn.addEventListener("click", () => savePng(st));

    // Re-fit on container resize. This changes the canvas's CSS size, never
    // its backing store, so no repaint (and no re-decode) is needed. Keep the
    // observer on the state object: an observer nothing holds a reference to
    // is collectable even while it has live observations, and this one
    // silently stopped firing once the panel settled into steady state.
    // render() re-fits on every call as well, so a resize is picked up within
    // one poll even if the observer does go away.
    if (window.ResizeObserver) {
      st.resizeObserver = new ResizeObserver(() => fitCanvas(st));
      st.resizeObserver.observe(stage);
    } else {
      window.addEventListener("resize", () => fitCanvas(st));
    }

    return st;
  }

  // fitCanvas sizes the canvas element (CSS pixels) to fit the stage while
  // preserving aspect ratio. Upscaling snaps to a whole multiple so every
  // source pixel becomes an identical square block; downscaling (a source
  // larger than the pane) uses the fractional ratio, since there is no
  // integer choice that both fits and stays crisp.
  //
  // The height side of the fit needs care. Both hosts today do give this
  // panel a definite height (the portal's .panel-mount and harness.html's
  // #panelHost), and that height is what the fit uses. But `.mp-panel`'s
  // `height: 100%` resolves to auto under a host that does not, and then the
  // stage is only as tall as what is inside it -- which is the canvas whose
  // size is being computed. Measured naively that is a latch: a collapsed
  // stage yields a 1px canvas, which keeps the stage collapsed, permanently.
  // So the stage height is trusted only when it is demonstrably taller than
  // the canvas sitting in it; otherwise the fit is bounded by the viewport
  // and the stage takes its height from the canvas, as an auto-height box
  // should. Note this is a single-shot decision each call, never a feedback
  // loop -- when the stage height IS the canvas height, it is not used, so
  // the size cannot ratchet.
  function fitCanvas(st) {
    if (!st.haveWidth || !st.haveHeight) return;
    const style = window.getComputedStyle(st.stage);
    const padX = parseFloat(style.paddingLeft) + parseFloat(style.paddingRight);
    const padY = parseFloat(style.paddingTop) + parseFloat(style.paddingBottom);
    const availW = Math.max(1, st.stage.clientWidth - padX);
    const inner = st.stage.clientHeight - padY;
    const canvasH = st.canvas.style.display === "none"
      ? 0 : st.canvas.getBoundingClientRect().height;
    const imposedH = inner - canvasH > 4 ? inner : 0;
    const availH = imposedH > 80 ? imposedH : Math.max(160, Math.round(window.innerHeight * 0.7));
    let scale = Math.min(availW / st.haveWidth, availH / st.haveHeight);
    if (scale >= 1) scale = Math.floor(scale);
    if (!(scale > 0)) scale = Math.min(availW / st.haveWidth, availH / st.haveHeight);
    st.canvas.style.width = Math.max(1, Math.round(st.haveWidth * scale)) + "px";
    st.canvas.style.height = Math.max(1, Math.round(st.haveHeight * scale)) + "px";
  }

  function slug(s) {
    return String(s || "image").toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-+|-+$/g, "") || "image";
  }

  // savePng is entirely client-side -- toDataURL on the canvas the operator is
  // already looking at. No round trip, and nothing is written on the device.
  function savePng(st) {
    if (st.haveRev === null) return;
    const a = document.createElement("a");
    a.href = st.canvas.toDataURL("image/png");
    a.download = `${slug(st.appName)}-rev${st.haveRev}.png`;
    document.body.appendChild(a);
    a.click();
    a.remove();
  }

  function showNote(st, text) {
    st.noteEl.textContent = text;
    st.noteEl.style.display = "";
    st.canvas.style.display = "none";
  }

  function showCanvas(st) {
    st.noteEl.textContent = "";
    st.noteEl.style.display = "none";
    st.canvas.style.display = "";
  }

  // paint decodes the payload straight into the canvas backing store at the
  // image's native resolution. Returns an error string, or null on success.
  function paint(st, width, height, bytes) {
    const want = width * height * 3;
    if (bytes.length !== want) {
      return `payload is ${bytes.length} bytes, but ${width}×${height} rgb888 needs ${want} — not drawing a partial image`;
    }
    if (st.canvas.width !== width || st.canvas.height !== height) {
      st.canvas.width = width;
      st.canvas.height = height;
    }
    const img = st.ctx.createImageData(width, height);
    const dst = img.data;
    for (let i = 0, j = 0; i < want; i += 3, j += 4) {
      dst[j] = bytes[i];
      dst[j + 1] = bytes[i + 1];
      dst[j + 2] = bytes[i + 2];
      dst[j + 3] = 255;
    }
    st.ctx.putImageData(img, 0, 0);
    return null;
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
    const st = el.__mpImage;
    return st && st.canvas.isConnected ? st : null;
  }

  function render(el, data) {
    data = data || {};
    const st = liveState(el) || buildSkeleton(el);

    st.appName = data.app_name || "";
    st.titleEl.textContent = data.app_name || "Image";

    const width = Number(data.width) || 0;
    const height = Number(data.height) || 0;
    const rev = Number(data.rev) || 0;
    const format = data.format || "rgb888";
    const hasPayload = typeof data.data_b64 === "string" && data.data_b64.length > 0;

    let problem = null;

    if (hasPayload) {
      if (format !== "rgb888") {
        // A format this file cannot decode is not a reason to show the
        // previous image as if it were current, and definitely not a reason
        // to guess at the layout.
        problem = `unsupported image format "${format}" — this panel only decodes rgb888`;
      } else if (width <= 0 || height <= 0) {
        problem = `image payload with no usable size (${width}×${height})`;
      } else {
        const bytes = decodeBase64(data.data_b64);
        if (bytes === null) {
          problem = "image payload is not valid base64";
        } else {
          problem = paint(st, width, height, bytes);
          if (problem === null) {
            st.haveRev = rev;
            st.haveWidth = width;
            st.haveHeight = height;
            showCanvas(st);
            fitCanvas(st);
          }
        }
      }
    }

    // rev 0 means "nothing has been decoded", which is a statement about the
    // present, not a cache hit. If the app was reset (or restarted) after this
    // panel had pixels, the picture on the canvas is of something that is no
    // longer there, so drop it rather than keep showing it under a caveat.
    if (!hasPayload && rev === 0 && st.haveRev !== null) {
      st.haveRev = null;
      st.haveWidth = 0;
      st.haveHeight = 0;
      st.canvas.width = 1;
      st.canvas.height = 1;
    }

    // No payload this time. Either we already have these pixels (the normal
    // case -- the backend omits data_b64 for a rev the client reported having)
    // or there is nothing decoded yet.
    if (!hasPayload && st.haveRev !== null) {
      showCanvas(st);
      if (rev !== st.haveRev) {
        // The backend announced a content change but sent no pixels for it.
        // Keep showing what we have, clearly labelled with the rev it really
        // is, rather than implying the canvas is up to date.
        problem = `showing rev ${st.haveRev}; backend announced rev ${rev} without sending it`;
      }
    } else if (!hasPayload && st.haveRev === null) {
      showNote(st, data.note || "No image decoded yet.");
    }

    if (problem !== null && st.haveRev === null) {
      showNote(st, problem);
    }

    // Meta line: only ever describes pixels that are actually on the canvas.
    const parts = [];
    if (st.haveRev !== null) {
      parts.push(`${st.haveWidth}×${st.haveHeight}`);
      parts.push(`rev ${st.haveRev}`);
    } else {
      parts.push("no image");
    }
    if (problem !== null && st.haveRev !== null) parts.push(problem);
    else if (data.note && st.haveRev !== null) parts.push(String(data.note));
    st.metaEl.textContent = parts.join(" · ");

    st.saveBtn.disabled = st.haveRev === null;

    // Re-fit every call, not only after a paint: in steady state the backend
    // omits the payload, so the paint path does not run and this would
    // otherwise be the container-resize observer's job alone.
    if (st.haveRev !== null) fitCanvas(st);

    // Contract 4's client half. app.js reads data-mp-have-image-rev off the
    // mount element and forwards it as GET /api/panel?have_image_rev=N, which
    // is what lets the backend omit a payload the browser already holds (see
    // app.js's "PANEL POLL QUERY" header comment). It is published from
    // st.haveRev -- the rev of the pixels actually on the canvas -- and never
    // from the rev the backend just announced: claiming to hold an image that
    // failed to decode, or was never sent, would make the backend stop
    // sending it. Absent while there is nothing decoded, so the first image
    // always arrives.
    if (st.haveRev === null) delete el.dataset.mpHaveImageRev;
    else el.dataset.mpHaveImageRev = String(st.haveRev);
  }

  window.MayhemPanels.registerPanel("image", render);
})();
