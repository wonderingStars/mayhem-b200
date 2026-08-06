// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// The "receiver" panel: mode, a large frequency readout, gain/squelch/volume,
// a signal-level meter and squelch-open indicator, and controls that POST
// back to whatever endpoint the data says (see ../../../PANELS.md). Any
// control whose data.controls.<name> is absent renders read-only -- most
// decoder apps have no user-settable frequency, and this is how that
// degrades honestly rather than guessing an endpoint.

(() => {
  "use strict";

  const { clamp } = window.MayhemPanels.util;

  async function postControl(st, control, body, onSuccess) {
    if (!control || !control.endpoint) return;
    try {
      const res = await fetch(control.endpoint, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      let data = null;
      try { data = await res.json(); } catch (_) { /* no/invalid JSON body */ }
      if (!res.ok) {
        throw new Error((data && data.error) || `${res.status} ${res.statusText}`);
      }
      if (onSuccess) onSuccess(data || {});
      flashToast(st, "Updated", false);
    } catch (err) {
      flashToast(st, err.message, true);
    }
  }

  let toastTimer = null;
  function flashToast(st, message, isError) {
    st.toast.textContent = message;
    st.toast.classList.toggle("mp-error", !!isError);
    st.toast.classList.add("mp-visible");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(() => st.toast.classList.remove("mp-visible"), 2200);
  }

  function createRangeControl(label, unit, decimals) {
    const field = document.createElement("div");
    field.className = "mp-control-field";
    const row1 = document.createElement("div");
    row1.className = "mp-control-row";
    const labelEl = document.createElement("span");
    labelEl.className = "mp-control-label";
    labelEl.textContent = label;
    const valueEl = document.createElement("span");
    valueEl.className = "mp-control-value";
    row1.appendChild(labelEl);
    row1.appendChild(valueEl);

    const row2 = document.createElement("div");
    row2.className = "mp-control-row";
    const slider = document.createElement("input");
    slider.type = "range";
    const number = document.createElement("input");
    number.type = "number";
    number.step = decimals > 0 ? (1 / Math.pow(10, decimals)).toString() : "1";
    row2.appendChild(slider);
    row2.appendChild(number);

    field.appendChild(row1);
    field.appendChild(row2);

    return { field, slider, number, valueEl, unit, decimals };
  }

  function wireRangeControl(ctl, getControlSpec, opts) {
    const commit = (raw) => {
      const spec = getControlSpec();
      if (!spec) return;
      const body = {};
      body[spec.body_key] = raw;
      postControl(opts.st, spec, body, (res) => {
        const v = res[spec.body_key];
        opts.onCommitted(v !== undefined ? v : raw);
      });
    };
    ctl.slider.addEventListener("input", () => {
      ctl.number.value = ctl.slider.value;
    });
    ctl.slider.addEventListener("change", () => {
      const v = parseFloat(ctl.slider.value);
      if (!Number.isNaN(v)) commit(v);
    });
    ctl.number.addEventListener("change", () => {
      const v = parseFloat(ctl.number.value);
      if (!Number.isNaN(v)) {
        ctl.slider.value = v;
        commit(v);
      }
    });
  }

  function setRangeValue(ctl, value, min, max, step, controlEnabled) {
    const supported = controlEnabled && Number.isFinite(min) && Number.isFinite(max) && max > min;
    ctl.slider.disabled = !supported;
    ctl.number.disabled = !supported;
    if (Number.isFinite(min)) ctl.slider.min = ctl.number.min = min;
    if (Number.isFinite(max)) ctl.slider.max = ctl.number.max = max;
    if (Number.isFinite(step) && step > 0) ctl.slider.step = ctl.number.step = step;
    if (document.activeElement !== ctl.number && document.activeElement !== ctl.slider) {
      ctl.slider.value = value;
      ctl.number.value = value;
    }
    ctl.valueEl.textContent = Number.isFinite(value) ? `${value.toFixed(ctl.decimals)} ${ctl.unit}` : "–";
  }

  function buildSkeleton(el) {
    el.innerHTML = "";
    el.classList.add("mp-panel", "mp-receiver-panel");
    el.style.position = "relative";

    const toolbar = document.createElement("div");
    toolbar.className = "mp-toolbar";
    const title = document.createElement("span");
    title.className = "mp-title";
    toolbar.appendChild(title);
    el.appendChild(toolbar);

    const body = document.createElement("div");
    body.className = "mp-receiver-body";

    // Frequency block.
    const freqBlock = document.createElement("div");
    freqBlock.className = "mp-freq-block";
    const modeEl = document.createElement("div");
    modeEl.className = "mp-freq-mode";
    const readout = document.createElement("div");
    readout.className = "mp-freq-readout";
    const freqText = document.createElement("span");
    const freqInput = document.createElement("input");
    freqInput.type = "text";
    freqInput.inputMode = "decimal";
    freqInput.style.display = "none";
    const freqUnit = document.createElement("span");
    freqUnit.className = "mp-freq-unit";
    freqUnit.textContent = "MHz";
    readout.appendChild(freqText);
    readout.appendChild(freqInput);
    readout.appendChild(freqUnit);
    const steps = document.createElement("div");
    steps.className = "mp-freq-steps";
    const stepDown = document.createElement("button");
    stepDown.className = "mp-btn";
    stepDown.textContent = "− step";
    const stepUp = document.createElement("button");
    stepUp.className = "mp-btn";
    stepUp.textContent = "+ step";
    steps.appendChild(stepDown);
    steps.appendChild(stepUp);
    freqBlock.appendChild(modeEl);
    freqBlock.appendChild(readout);
    freqBlock.appendChild(steps);
    body.appendChild(freqBlock);

    // Level meter.
    const meterRow = document.createElement("div");
    meterRow.className = "mp-meter-row";
    const meterTrack = document.createElement("div");
    meterTrack.className = "mp-meter-track";
    const meterFill = document.createElement("div");
    meterFill.className = "mp-meter-fill";
    meterTrack.appendChild(meterFill);
    const meterLabel = document.createElement("span");
    meterLabel.className = "mp-meter-label";
    const sqlBadge = document.createElement("span");
    sqlBadge.className = "mp-sql-badge";
    sqlBadge.textContent = "SQL";
    meterRow.appendChild(meterTrack);
    meterRow.appendChild(meterLabel);
    meterRow.appendChild(sqlBadge);
    body.appendChild(meterRow);

    const gainCtl = createRangeControl("Gain", "dB", 1);
    const squelchCtl = createRangeControl("Squelch", "", 0);
    const volumeCtl = createRangeControl("Volume", "", 0);
    body.appendChild(gainCtl.field);
    body.appendChild(squelchCtl.field);
    body.appendChild(volumeCtl.field);

    const agcRow = document.createElement("div");
    agcRow.className = "mp-agc-row";
    const agcLabel = document.createElement("span");
    agcLabel.className = "mp-control-label";
    agcLabel.textContent = "AGC";
    const agcSwitch = document.createElement("label");
    agcSwitch.className = "mp-switch";
    const agcInput = document.createElement("input");
    agcInput.type = "checkbox";
    const agcTrack = document.createElement("span");
    agcTrack.className = "mp-switch-track";
    agcSwitch.appendChild(agcInput);
    agcSwitch.appendChild(agcTrack);
    agcRow.appendChild(agcLabel);
    agcRow.appendChild(agcSwitch);
    body.appendChild(agcRow);

    el.appendChild(body);

    const toast = document.createElement("div");
    toast.className = "mp-flash-toast";
    el.appendChild(toast);

    const st = {
      titleEl: title, modeEl, freqText, freqInput, meterFill, meterLabel, sqlBadge,
      gainCtl, squelchCtl, volumeCtl, agcInput, stepDown, stepUp, toast,
      lastData: null, editingFreq: false,
    };
    el.__mpReceiver = st;

    // --- frequency editing ---
    readout.addEventListener("click", () => {
      if (!st.lastData) return;
      const spec = st.lastData.controls && st.lastData.controls.frequency;
      if (!spec) return;
      st.editingFreq = true;
      freqInput.value = ((st.lastData.frequency_hz || 0) / 1e6).toFixed(6);
      freqText.style.display = "none";
      freqInput.style.display = "";
      freqInput.style.width = (freqInput.value.length + 1) + "ch";
      freqInput.focus();
      freqInput.select();
    });
    const commitFreq = () => {
      st.editingFreq = false;
      freqText.style.display = "";
      freqInput.style.display = "none";
      const mhz = parseFloat(freqInput.value);
      const spec = st.lastData && st.lastData.controls && st.lastData.controls.frequency;
      if (!spec || Number.isNaN(mhz)) return;
      postControl(st, spec, { hz: mhz * 1e6 }, (res) => {
        st.lastData.frequency_hz = res.hz !== undefined ? res.hz : mhz * 1e6;
        freqText.textContent = (st.lastData.frequency_hz / 1e6).toFixed(6);
      });
    };
    freqInput.addEventListener("keydown", (ev) => {
      if (ev.key === "Enter") { freqInput.blur(); }
      else if (ev.key === "Escape") { st.editingFreq = false; freqText.style.display = ""; freqInput.style.display = "none"; }
    });
    freqInput.addEventListener("blur", commitFreq);

    stepDown.addEventListener("click", () => {
      if (!st.lastData) return;
      const spec = st.lastData.controls && st.lastData.controls.frequency;
      const stepHz = st.lastData.frequency_step_hz || 0;
      if (!spec || !stepHz) return;
      const hz = (st.lastData.frequency_hz || 0) - stepHz;
      postControl(st, spec, { hz }, (res) => {
        st.lastData.frequency_hz = res.hz !== undefined ? res.hz : hz;
        freqText.textContent = (st.lastData.frequency_hz / 1e6).toFixed(6);
      });
    });
    stepUp.addEventListener("click", () => {
      if (!st.lastData) return;
      const spec = st.lastData.controls && st.lastData.controls.frequency;
      const stepHz = st.lastData.frequency_step_hz || 0;
      if (!spec || !stepHz) return;
      const hz = (st.lastData.frequency_hz || 0) + stepHz;
      postControl(st, spec, { hz }, (res) => {
        st.lastData.frequency_hz = res.hz !== undefined ? res.hz : hz;
        freqText.textContent = (st.lastData.frequency_hz / 1e6).toFixed(6);
      });
    });

    wireRangeControl(gainCtl, () => st.lastData && st.lastData.controls && st.lastData.controls.gain, {
      st, onCommitted: (v) => { st.lastData.gain_db = v; setRangeValue(gainCtl, v, st.lastData.gain_min_db, st.lastData.gain_max_db, st.lastData.gain_step_db, true); },
    });
    wireRangeControl(squelchCtl, () => st.lastData && st.lastData.controls && st.lastData.controls.squelch, {
      st, onCommitted: (v) => { st.lastData.squelch = v; setRangeValue(squelchCtl, v, st.lastData.squelch_min, st.lastData.squelch_max, 1, true); },
    });
    wireRangeControl(volumeCtl, () => st.lastData && st.lastData.controls && st.lastData.controls.volume, {
      st, onCommitted: (v) => { st.lastData.volume = v; setRangeValue(volumeCtl, v, st.lastData.volume_min, st.lastData.volume_max, 1, true); },
    });

    agcInput.addEventListener("change", () => {
      const spec = st.lastData && st.lastData.controls && st.lastData.controls.agc;
      const desired = agcInput.checked;
      if (!spec) { agcInput.checked = !desired; return; }
      postControl(st, spec, { on: desired }, (res) => {
        st.lastData.agc = res.on !== undefined ? res.on : desired;
        agcInput.checked = st.lastData.agc;
      });
    });

    return st;
  }

  function render(el, data) {
    data = data || {};
    const st = el.__mpReceiver || buildSkeleton(el);
    st.lastData = data;

    st.titleEl.textContent = data.app_name || "Receiver";
    st.modeEl.textContent = data.mode_detail ? `${data.mode} · ${data.mode_detail}` : (data.mode || "");

    if (!st.editingFreq) {
      st.freqText.textContent = Number.isFinite(data.frequency_hz) ? (data.frequency_hz / 1e6).toFixed(6) : "–";
    }
    const hasFreqControl = !!(data.controls && data.controls.frequency);
    st.stepDown.disabled = !hasFreqControl || !data.frequency_step_hz;
    st.stepUp.disabled = !hasFreqControl || !data.frequency_step_hz;

    const levelMin = Number.isFinite(data.level_min_db) ? data.level_min_db : -100;
    const levelMax = Number.isFinite(data.level_max_db) ? data.level_max_db : 0;
    const levelFrac = clamp(((data.level_db ?? levelMin) - levelMin) / Math.max(1e-6, levelMax - levelMin), 0, 1);
    st.meterFill.style.width = `${(levelFrac * 100).toFixed(1)}%`;
    st.meterLabel.textContent = Number.isFinite(data.level_db) ? `${data.level_db.toFixed(1)}dB` : "–";
    st.sqlBadge.classList.toggle("mp-open", !!data.squelch_open);
    st.sqlBadge.textContent = data.squelch_open ? "SQL OPEN" : "SQL";

    const hasGain = !!(data.controls && data.controls.gain);
    setRangeValue(st.gainCtl, data.gain_db, data.gain_min_db, data.gain_max_db, data.gain_step_db, hasGain);
    const hasSquelch = !!(data.controls && data.controls.squelch);
    setRangeValue(st.squelchCtl, data.squelch, data.squelch_min, data.squelch_max, 1, hasSquelch);
    const hasVolume = !!(data.controls && data.controls.volume);
    setRangeValue(st.volumeCtl, data.volume, data.volume_min, data.volume_max, 1, hasVolume);

    const hasAgc = !!(data.controls && data.controls.agc);
    st.agcInput.disabled = !hasAgc;
    if (document.activeElement !== st.agcInput) st.agcInput.checked = !!data.agc;
  }

  window.MayhemPanels.registerPanel("receiver", render);
})();
