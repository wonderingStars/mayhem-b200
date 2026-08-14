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
// cpp_apps.json was re-captured at 0.11.3 when GET /api/apps grew panel_kind.
// That one endpoint reads app::AppRegistry and nothing else, so it needs no
// radio: the capture ran against a backend pointed at an unreachable sdrlink
// address (--driver=sdrlink --args=127.0.0.1:9), which opens no device at
// all, and every pre-existing field in the file is byte-identical to the
// 0.9.0 capture. The other three fixtures still need an app open on real
// hardware and were left alone.
//
// Re-captured again by the same no-radio procedure when the AIS provider moved
// from panel_kind "geotable" to its own "ais" kind, and once more when EPIRB
// RX (table -> geotable) and Radiosonde (no key -> geotable) were given map
// panels; the current fixture was captured from the merged build carrying all
// three changes. aprsrx still declares "geotable" alongside them.
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
	// The backend has always sent panel_kind here; this struct had no field
	// for it, so the portal server re-encoded the response without it and the
	// browser never saw it. A dropped field looks exactly like a field the
	// backend does not send, which is how it went unnoticed.
	if cur.PanelKind != "adsb" {
		t.Errorf("PanelKind = %q, want %q — the fixture carries it and this "+
			"struct is what gets re-encoded for the browser", cur.PanelKind, "adsb")
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

// can_transmit exists so a receive-only SDR does not present ~28 transmit
// apps that cannot work. Its whole value depends on three states staying
// distinguishable end to end -- true, false, and "the backend has not said"
// -- so each is pinned here rather than only the happy one.
//
// The false case is the one with a trap in it. As a plain bool with
// omitempty, an explicit false marshals away to nothing, arrives at the
// browser as absent, and reads as unknown -- which unlocks exactly the apps
// this field exists to lock. That is why Status.CanTransmit is a *bool, and
// why this test round-trips through Marshal rather than only decoding.
func TestContract_CanTransmitSurvivesTheRoundTrip(t *testing.T) {
	for _, tc := range []struct {
		name string
		in   string
		want *bool
	}{
		{"receive-only radio", `{"device":"RTL-SDR","can_transmit":false}`, boolPtr(false)},
		{"full radio", `{"device":"B200","can_transmit":true}`, boolPtr(true)},
		{"no device open", `{"device":"no device"}`, nil},
	} {
		t.Run(tc.name, func(t *testing.T) {
			var st Status
			if err := json.Unmarshal([]byte(tc.in), &st); err != nil {
				t.Fatalf("decode: %v", err)
			}
			if (st.CanTransmit == nil) != (tc.want == nil) {
				t.Fatalf("CanTransmit nil-ness = %v, want %v", st.CanTransmit, tc.want)
			}
			if st.CanTransmit != nil && *st.CanTransmit != *tc.want {
				t.Errorf("CanTransmit = %v, want %v", *st.CanTransmit, *tc.want)
			}

			// The portal re-encodes this struct on the way to the browser, so
			// the value has to survive that too, not just the decode.
			out, err := json.Marshal(st)
			if err != nil {
				t.Fatalf("encode: %v", err)
			}
			var back Status
			if err := json.Unmarshal(out, &back); err != nil {
				t.Fatalf("re-decode: %v", err)
			}
			if (back.CanTransmit == nil) != (tc.want == nil) {
				t.Fatalf("after re-encode CanTransmit nil-ness = %v, want %v (this is "+
					"the omitempty trap: an explicit false must not become absent)",
					back.CanTransmit, tc.want)
			}
			if back.CanTransmit != nil && *back.CanTransmit != *tc.want {
				t.Errorf("after re-encode CanTransmit = %v, want %v", *back.CanTransmit, *tc.want)
			}
		})
	}
}

func boolPtr(b bool) *bool { return &b }

// GET /api/apps carries panel_kind per app, which is what lets the browser's
// grid badge the tiles that have a real native view (app.js's
// nativePanelKindFor). Before it existed a panel kind was published only for
// the ONE app that happened to be open, so the badge could not be drawn
// truthfully for anything else and was not drawn at all.
//
// The rules below are pinned against the raw JSON as well as the struct,
// because the struct alone cannot tell "the backend omitted it" from "the Go
// type dropped it" — the failure this whole file exists for.

func rawApps(t *testing.T) []map[string]json.RawMessage {
	t.Helper()
	var raw struct {
		Apps []map[string]json.RawMessage `json:"apps"`
	}
	loadFixture(t, "cpp_apps.json", &raw)
	if len(raw.Apps) == 0 {
		t.Fatal("no apps in the fixture")
	}
	return raw.Apps
}

func findRawApp(t *testing.T, apps []map[string]json.RawMessage, id string) map[string]json.RawMessage {
	t.Helper()
	for _, a := range apps {
		var got string
		if err := json.Unmarshal(a["id"], &got); err != nil {
			continue
		}
		if got == id {
			return a
		}
	}
	t.Fatalf("app %q not present in the real app list", id)
	return nil
}

func TestContract_AppsCarryPanelKindForAppsThatHaveAProvider(t *testing.T) {
	var resp AppsResponse
	loadFixture(t, "cpp_apps.json", &resp)

	// One app per distinct kind a provider registers, so a single kind wired
	// up wrongly cannot hide behind the others — plus the two apps whose kind
	// CHANGED when they were given a map: epirb_rx published "table" and
	// radiosonde had no provider at all (no panel_kind key). The browser's app
	// grid badges tiles from this and nothing else here would notice a
	// regression to the old answers.
	want := map[string]string{
		"adsbrx":       "adsb",
		"ais":          "ais",
		"pocsag":       "console",
		"aprsrx":       "geotable",
		"noaaapt_rx":   "image",
		"wardrivemap":  "map",
		"ert":          "table",
		"audio":        "receiver",
		"lookingglass": "spectrum",
		"epirb_rx":     "geotable",
		"radiosonde":   "geotable",
		"morseradio":   "morse",
	}
	seen := map[string]bool{}
	for _, a := range resp.Apps {
		if k, ok := want[a.ID]; ok {
			seen[a.ID] = true
			if a.PanelKind != k {
				t.Errorf("app %q PanelKind = %q, want %q — check client.App has the "+
					"panel_kind field and the backend still declares it", a.ID, a.PanelKind, k)
			}
		}
	}
	for id := range want {
		if !seen[id] {
			t.Errorf("app %q is not in the captured app list", id)
		}
	}
}

func TestContract_AppsOmitPanelKindEntirelyForAppsWithoutOne(t *testing.T) {
	// Most apps have no panel provider. Absent has to stay absent: the badge
	// is drawn from the key's presence, so an empty string would claim a
	// native view that does not exist.
	apps := rawApps(t)
	for _, id := range []string{"fmradio", "afsk_rx"} {
		a := findRawApp(t, apps, id)
		if v, present := a["panel_kind"]; present {
			t.Errorf("app %q carries panel_kind %s — apps with no provider must omit "+
				"the key, not send an empty one", id, string(v))
		}
		// The rest of the entry is untouched, so this is a real entry and not
		// an artifact of the lookup.
		if _, present := a["hardware_limited"]; !present {
			t.Errorf("app %q is missing hardware_limited — fixture looks wrong", id)
		}
	}

	// And at least one app in the list really is keyless, so the case above
	// cannot quietly stop being exercised if those two ids ever gain
	// providers.
	keyless := 0
	for _, a := range apps {
		if _, present := a["panel_kind"]; !present {
			keyless++
		}
	}
	if keyless == 0 {
		t.Error("every app carries panel_kind — the omit path is not being exercised at all")
	}
}

func TestContract_AppsNeverAdvertiseTheScreenPanelKind(t *testing.T) {
	// "screen" is the framebuffer mirror EVERY app has. It is a legitimate
	// answer on GET /api/panel and GET /api/apps/current, and never one here:
	// publishing it would badge all 104 tiles as having a native view.
	var resp AppsResponse
	loadFixture(t, "cpp_apps.json", &resp)
	for _, a := range resp.Apps {
		if a.PanelKind == "screen" {
			t.Errorf("app %q advertises panel_kind \"screen\"", a.ID)
		}
	}
}

func TestContract_AppPanelKindSurvivesTheReEncode(t *testing.T) {
	// The portal server re-encodes []client.App on the way to the browser
	// (internal/portal/server's appsResponse), so decoding it is only half
	// the hop — a field this struct lacks is dropped on the way OUT even
	// when the backend sent it. That is exactly how can_go_back, version and
	// CurrentApp.panel_kind each went missing.
	var resp AppsResponse
	loadFixture(t, "cpp_apps.json", &resp)

	out, err := json.Marshal(resp)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	var back struct {
		Apps []map[string]json.RawMessage `json:"apps"`
	}
	if err := json.Unmarshal(out, &back); err != nil {
		t.Fatalf("re-decode: %v", err)
	}
	if len(back.Apps) != len(resp.Apps) {
		t.Fatalf("re-encoded %d apps, want %d", len(back.Apps), len(resp.Apps))
	}

	adsb := findRawApp(t, back.Apps, "adsbrx")
	var kind string
	if err := json.Unmarshal(adsb["panel_kind"], &kind); err != nil {
		t.Fatalf("adsbrx lost panel_kind through the re-encode: %v", err)
	}
	if kind != "adsb" {
		t.Errorf("after re-encode adsbrx panel_kind = %q, want %q", kind, "adsb")
	}

	// omitempty in the other direction: an app that arrived without the key
	// must not leave with an empty one.
	fm := findRawApp(t, back.Apps, "fmradio")
	if v, present := fm["panel_kind"]; present {
		t.Errorf("after re-encode fmradio carries panel_kind %s — absent must stay absent",
			string(v))
	}
}

// Every panel kind's PAYLOAD field names are wire contract, exactly like the
// envelope above — and they drifted the same way the envelope once did. The
// spectrum emitter spoke its own dialect (centre_hz/span_hz, no type, no
// floor/ceil) while spectrum.js read the names PANELS.md documents, so every
// spectrum panel showed an empty axis reading 0.000 MHz while 1024 real bins
// flowed underneath — and the harness, whose fixtures were hand-written to
// the DOCUMENTED shape, rendered beautifully. Both halves green, seam broken.
// The receiver payload lost its meter the same way (level_db absent).
//
// testdata/cpp_panel_<kind>.json are REAL captured backend output (0.12.2+,
// live B200, one representative app per kind). This test asserts each carries
// the keys its renderer actually reads. If a C++ change fails this, fix the
// emission or PANELS.md FIRST, then re-capture — never hand-edit a fixture to
// match wishes, and never let the harness fixtures be the only shape the JS
// is tested against, because those are written by hand and prove nothing
// about the wire.
func TestContract_PanelPayloadsCarryTheKeysTheirRenderersRead(t *testing.T) {
	cases := []struct {
		kind string
		keys []string // required top-level data keys, per PANELS.md + panels/<kind>.js
	}{
		{"spectrum", []string{"type", "center_hz", "sample_rate_hz", "bins_db", "floor_db", "ceil_db"}},
		{"receiver", []string{"mode", "frequency_hz", "gain_db", "level_db", "level_min_db", "level_max_db", "squelch", "squelch_open", "volume"}},
		{"console", []string{"lines"}},
		{"table", []string{"columns", "rows"}},
		{"map", []string{"markers"}},
		{"geotable", []string{"map", "table"}},
		{"image", []string{"rev", "format", "width", "height"}},
	}
	for _, tc := range cases {
		t.Run(tc.kind, func(t *testing.T) {
			var p Panel
			loadFixture(t, "cpp_panel_"+tc.kind+".json", &p)
			if p.PanelKind != tc.kind {
				t.Fatalf("fixture kind = %q, want %q", p.PanelKind, tc.kind)
			}
			var payload map[string]json.RawMessage
			if err := json.Unmarshal(p.Data, &payload); err != nil {
				t.Fatalf("data not an object: %v", err)
			}
			for _, k := range tc.keys {
				if _, ok := payload[k]; !ok {
					t.Errorf("%s payload missing %q (renderer reads it); got keys %v",
						tc.kind, k, keysOf(payload))
				}
			}
		})
	}
}
