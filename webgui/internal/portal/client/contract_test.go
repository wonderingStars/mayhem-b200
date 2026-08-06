// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package client

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// The C++/Go wire contract.
//
// This file exists because the seam between mayhem-b200's C++ app-portal API
// and this Go client had NO test at all, and drifted apart in four separate
// places at once while every unit test on both sides stayed green:
//
//	GET /api/apps         C++ sent {"categories":[{name,apps}]}, this decodes {"apps":[...]}
//	                      and the app fields were "name"/"icon_name" vs display_name/icon
//	GET /api/panel        C++ sent {"kind":"adsb","adsb":{...}}, this decodes
//	                      {app_id,panel_kind,title,data} — and Panel.HasData() keys off
//	                      panel_kind, so EVERY app rendered as "no structured view yet"
//	GET /api/apps/current can_go_back was never sent, so the portal's back control was
//	                      permanently disabled
//	GET /api/status       version was never sent
//
// Each side tested its own half against its own idea of the format, which is
// exactly the shape of bug a golden fixture catches and a unit test cannot.
//
// testdata/cpp_*.json is real output captured from a running mayhem-b200
// (0.9.0, portal on :8090, ADS-B RX open, live B200). Re-capture with:
//
//	mayhem-b200.exe --portal=8090
//	curl -s http://127.0.0.1:8090/api/apps > testdata/cpp_apps.json      (etc.)
//
// If a change to the C++ side makes one of these fail, that is the test doing
// its job: fix the mismatch, then re-capture. Do NOT edit the fixture to match
// a new C++ shape without also confirming the browser still renders — the
// fixture is a record of what the C++ actually sends, not of what we wish it
// sent.

func loadFixture(t *testing.T, name string, v any) {
	t.Helper()
	data, err := os.ReadFile(filepath.Join("testdata", name))
	if err != nil {
		t.Fatalf("read fixture %s: %v", name, err)
	}
	if err := json.Unmarshal(data, v); err != nil {
		t.Fatalf("decode fixture %s: %v", name, err)
	}
}

func TestContract_AppsDecodesRealBackendOutput(t *testing.T) {
	var resp AppsResponse
	loadFixture(t, "cpp_apps.json", &resp)

	if len(resp.Apps) == 0 {
		t.Fatal("no apps decoded — the backend's app list is not reaching the portal " +
			"(this is the {\"categories\":[...]} vs {\"apps\":[...]} mismatch)")
	}

	// Every app must carry the fields the grid renders. A silently-empty
	// DisplayName is the "name" vs "display_name" bug: it decodes fine and
	// produces a grid of blank tiles.
	for _, a := range resp.Apps {
		if a.ID == "" {
			t.Errorf("app with empty ID: %+v", a)
		}
		if a.DisplayName == "" {
			t.Errorf("app %q has an empty DisplayName — check the JSON key is display_name", a.ID)
		}
		if a.Category == "" {
			t.Errorf("app %q has an empty Category — grouping would drop it into \"\"", a.ID)
		}
	}

	// Spot-check a known app rather than only aggregate properties, so a
	// wholesale field rename cannot pass by leaving everything non-empty.
	var found *App
	for i := range resp.Apps {
		if resp.Apps[i].ID == "adsbrx" {
			found = &resp.Apps[i]
			break
		}
	}
	if found == nil {
		t.Fatal("adsbrx not present in the real app list")
	}
	if found.DisplayName != "ADS-B" {
		t.Errorf("adsbrx DisplayName = %q, want %q", found.DisplayName, "ADS-B")
	}
	if found.Category != "Receive" {
		t.Errorf("adsbrx Category = %q, want %q", found.Category, "Receive")
	}
}

func TestContract_PanelDecodesRealBackendOutput(t *testing.T) {
	var p Panel
	loadFixture(t, "cpp_panel_adsb.json", &p)

	// HasData() is what the UI branches on; if panel_kind is missing it is
	// false and the app renders as "no structured view yet" no matter how
	// much data the backend actually published.
	if !p.HasData() {
		t.Fatal("Panel.HasData() is false for a real published panel — panel_kind is missing")
	}
	if p.PanelKind != "adsb" {
		t.Errorf("PanelKind = %q, want %q", p.PanelKind, "adsb")
	}
	if p.AppID != "adsbrx" {
		t.Errorf("AppID = %q, want %q", p.AppID, "adsbrx")
	}
	if len(p.Data) == 0 {
		t.Fatal("Data is empty — the payload is not reaching the renderer")
	}

	// Data must be the BARE kind-specific payload the JS renderer expects,
	// not the self-describing {kind, <kind>:{...}} wrapper.
	var payload map[string]json.RawMessage
	if err := json.Unmarshal(p.Data, &payload); err != nil {
		t.Fatalf("Data is not a JSON object: %v", err)
	}
	if _, wrapped := payload["kind"]; wrapped {
		t.Error(`Data contains a "kind" key — the wrapper is being sent instead of the payload`)
	}
	if _, doubled := payload["adsb"]; doubled {
		t.Error(`Data contains an "adsb" key — the payload is double-wrapped`)
	}
	for _, want := range []string{"aircraft", "stats"} {
		if _, ok := payload[want]; !ok {
			t.Errorf("adsb payload is missing %q; got keys %v", want, keysOf(payload))
		}
	}
}

func TestContract_CurrentAppDecodesRealBackendOutput(t *testing.T) {
	var cur CurrentApp
	loadFixture(t, "cpp_current.json", &cur)

	if cur.ID != "adsbrx" {
		t.Errorf("ID = %q, want %q", cur.ID, "adsbrx")
	}
	if cur.Title == "" {
		t.Error("Title is empty")
	}
	// Captured with an app open, so the portal must be able to go back. A
	// missing can_go_back decodes as false and strands the user in the app.
	if !cur.CanGoBack {
		t.Error("CanGoBack is false with an app open — the back control would be dead")
	}
}

func TestContract_StatusDecodesRealBackendOutput(t *testing.T) {
	var st Status
	loadFixture(t, "cpp_status.json", &st)

	if st.Device == "" {
		t.Error("Device is empty")
	}
	if st.Version == "" {
		t.Error("Version is empty — the header would show no firmware version")
	}
}

func keysOf(m map[string]json.RawMessage) []string {
	out := make([]string, 0, len(m))
	for k := range m {
		out = append(out, k)
	}
	return out
}
