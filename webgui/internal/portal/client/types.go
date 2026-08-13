// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

// Package client is a Go client for mayhem-b200's app-portal HTTP API,
// served by the C++ process itself at (by default) http://127.0.0.1:8090.
// That API is a thin JSON window onto the same state the 240x320 framebuffer
// UI draws from — app::AppRegistry, ui::NavigationView and each app's own
// panel data — so a browser can render it properly instead of just mirroring
// pixels. The screen mirror this package also speaks (screen.go) is the
// other half of the same API — see ../../../doc/REMOTE-UI.md, which now
// documents both hops as built.
//
// Wire contract assumed by this client (the C++ side is built by another
// agent against this same contract):
//
//	GET  /api/apps            -> {"apps":[App...]}
//	GET  /api/apps/current    -> CurrentApp
//	POST /api/apps/{id}/launch -> CurrentApp
//	POST /api/apps/home       -> CurrentApp
//	GET  /api/panel           -> Panel        (?have_image_rev=N, contract 4)
//	GET  /api/status          -> Status
//	GET  /api/screen          -> binary frame (contract 1, see screen.go)
//	POST /api/input           -> InputResult  (contract 2, see screen.go)
//
// Every struct here only reads fields it knows about via encoding/json, so
// the backend is free to add fields later without breaking this client.
package client

import "encoding/json"

// App is one entry from app::AppRegistry, as published over the API.
// Field names mirror app::AppEntry (app_registry.hpp).
type App struct {
	ID              string `json:"id"`
	DisplayName     string `json:"display_name"`
	Category        string `json:"category"`
	HardwareLimited bool   `json:"hardware_limited"`
	// Icon is an opaque hint the backend may supply (e.g. a glyph name or
	// short code); empty when the app has none. The UI falls back to a
	// generic per-category icon when this is empty.
	Icon string `json:"icon,omitempty"`
}

// AppsResponse is the payload of GET /api/apps.
type AppsResponse struct {
	Apps []App `json:"apps"`
}

// CurrentApp describes what ui::NavigationView currently has on top of its
// stack: either a running app or the home menu (ID == "" means home).
// The C++ side derives this from the navigation stack on every refresh, so
// it follows the device even when the operator navigated with the keys
// rather than through the launch route.
type CurrentApp struct {
	ID    string `json:"id,omitempty"`
	Title string `json:"title"`
	// PanelKind is the panel kind the active app publishes, as
	// AppBridge::current_app_json() sends it (app_bridge.cpp). This struct is
	// what the portal server re-encodes for the browser, so a field missing
	// here is a field the browser can never see, however faithfully the C++
	// side sends it — and this one WAS missing: testdata/cpp_current.json,
	// captured from a real backend, reads
	// {"id":"adsbrx",...,"panel_kind":"adsb",...} and the browser got no
	// panel_kind at all.
	//
	// Note this is only ever the ONE app that is open. GET /api/apps carries
	// no panel kind per app (see to_json(AppSummary) in app_data.cpp), which
	// is why the grid cannot draw a truthful NATIVE badge yet.
	PanelKind string `json:"panel_kind,omitempty"`
	CanGoBack bool   `json:"can_go_back"`
}

// AtHome reports whether the navigation stack is at its root.
func (c CurrentApp) AtHome() bool { return c.ID == "" }

// Panel is the structured-data payload of GET /api/panel: whatever the
// currently active app has chosen to publish for a browser to render, if
// anything. PanelKind is empty when the active app has not published a
// panel view at all — the UI must show an honest "no structured view yet"
// state rather than guessing at Data's shape.
//
// Data is left as raw JSON: its shape is entirely determined by PanelKind
// and is interpreted by the matching entry in the browser-side panel
// registry (see internal/portal/server/static/app.js), never by this
// package.
type Panel struct {
	AppID     string          `json:"app_id,omitempty"`
	PanelKind string          `json:"panel_kind,omitempty"`
	Title     string          `json:"title,omitempty"`
	Data      json.RawMessage `json:"data,omitempty"`
}

// HasData reports whether the backend published anything to draw.
func (p Panel) HasData() bool { return p.PanelKind != "" }

// Status is the payload of GET /api/status: the header-bar state, plus a
// version string for display.
type Status struct {
	// Device is a human-readable device label (e.g. "B200 EDR04ZDB2"), or
	// empty if no radio is attached/opened.
	Device       string `json:"device,omitempty"`
	Receiving    bool   `json:"receiving"`
	Transmitting bool   `json:"transmitting"`
	Version      string `json:"version,omitempty"`
}
