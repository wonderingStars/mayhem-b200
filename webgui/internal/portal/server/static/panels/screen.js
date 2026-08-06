// SPDX-License-Identifier: GPL-2.0-or-later
//
// Part of mayhem-b200.
//
// The "screen" (fallback) panel: an honest "this app has no structured web
// view yet" card, naming the app. This is also what MayhemPanels.renderPanel
// falls back to for any unregistered panel_kind (see registry.js), with a
// synthesized reason -- so it doubles as the answer to "what does an app
// without a JSON panel look like on this portal" and "what does a typo in
// panel_kind look like during development".

(() => {
  "use strict";

  const ICON_SVG = `
    <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="currentColor"
         stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round">
      <rect x="2.5" y="4" width="19" height="13" rx="1.5"></rect>
      <path d="M8 20.5h8"></path>
      <path d="M12 17v3.5"></path>
    </svg>`;

  function buildSkeleton(el) {
    el.innerHTML = "";
    el.classList.add("mp-panel", "mp-screen-panel");

    const card = document.createElement("div");
    card.className = "mp-screen-card";

    const icon = document.createElement("div");
    icon.className = "mp-screen-icon";
    icon.innerHTML = ICON_SVG; // static, no data interpolation

    const name = document.createElement("div");
    name.className = "mp-screen-name";

    const category = document.createElement("div");
    category.className = "mp-screen-category";

    const reason = document.createElement("div");
    reason.className = "mp-screen-reason";

    card.appendChild(icon);
    card.appendChild(name);
    card.appendChild(category);
    card.appendChild(reason);
    el.appendChild(card);

    const st = { nameEl: name, categoryEl: category, reasonEl: reason };
    el.__mpScreen = st;
    return st;
  }

  function render(el, data) {
    data = data || {};
    const st = el.__mpScreen || buildSkeleton(el);
    st.nameEl.textContent = data.app_name || data.title || "This app";
    if (data.category) {
      st.categoryEl.textContent = data.category;
      st.categoryEl.style.display = "";
    } else {
      st.categoryEl.style.display = "none";
    }
    st.reasonEl.textContent = data.reason || "This app doesn't have a structured web view yet.";
  }

  window.MayhemPanels.registerPanel("screen", render);
})();
