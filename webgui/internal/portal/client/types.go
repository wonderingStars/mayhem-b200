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
	// PanelKind is the kind of native browser panel this app publishes
	// ("adsb", "console", "geotable", ...), or empty when it has no panel
	// provider at all — which is most apps. It is what lets the grid badge
	// which tiles have a real structured view; app.js's nativePanelKindFor()
	// reads it straight off each app object.
	//
	// The backend never sends "screen" here. Every app can be mirrored as a
	// framebuffer, so a "screen" would badge the whole grid and mean nothing
	// (see to_json(AppSummary) in src/remote/app_data.cpp).
	//
	// This struct is what the portal server RE-ENCODES for the browser, so a
	// field missing here is a field the browser can never see however
	// faithfully the C++ side sends it — that is exactly how can_go_back,
	// version and CurrentApp.PanelKind each went missing before
	// contract_test.go existed. omitempty is deliberate and safe in this one
	// direction: absent and "" both mean "no native panel", so the round trip
	// cannot lose a distinction (unlike Status.CanTransmit, where an explicit
	// false is a real answer and the field has to be a pointer).
	PanelKind string `json:"panel_kind,omitempty"`
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
	// Note this is only ever the ONE app that is open, and unlike App's
	// panel_kind it CAN be "screen" — that is the honest answer for an app
	// with no provider, and the browser mounts the framebuffer mirror on it.
	// App.PanelKind is the per-app capability the grid badges from.
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
	Device string `json:"device,omitempty"`
	// Link is how the connection to the radio is doing: "connected",
	// "reconnecting" or "disconnected". Only a networked backend (sdrlink)
	// ever reports the middle value, while it works through its retry
	// ladder. This struct is re-encoded on the way to the browser, so a
	// field missing here is dropped no matter what the backend sends --
	// which is exactly how can_go_back and version went missing before
	// contract_test.go existed.
	Link string `json:"link,omitempty"`
	// CanTransmit is whether the attached radio is CAPABLE of transmitting,
	// which is not the same question as Transmitting (what it is doing right
	// now). The frontend locks the transmit apps on false and leaves them
	// alone on nil, so nil has to mean "the backend has not said" — with no
	// device open there is no honest answer.
	//
	// A pointer, deliberately. As a plain bool with omitempty, an explicit
	// false would be indistinguishable from an absent field on the way out,
	// so a receive-only dongle would report "unknown" and the ~28 transmit
	// apps would stay unlocked — the exact case this field exists to catch.
	CanTransmit  *bool  `json:"can_transmit,omitempty"`
	Receiving    bool   `json:"receiving"`
	Transmitting bool   `json:"transmitting"`
	Version      string `json:"version,omitempty"`
}
