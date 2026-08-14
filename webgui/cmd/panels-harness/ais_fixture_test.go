// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

// The `ais` fixtures are the executable half of the panel's wire contract:
// harness.html is driven entirely from them, so a fixture that drifts from the
// shape the C++ side sends turns the harness into a demonstration of something
// that does not exist. These tests pin the properties that make the fixture
// worth having rather than its exact contents — above all that ABSENT FIELDS
// ARE ABSENT, which is the rule the whole panel is built around and the one a
// well-meaning "fill in the blanks" edit would quietly delete.
//
// Own file rather than an addition to main_test.go, which another change-set
// may be editing.

func loadAisFixture(t *testing.T, name string) map[string]json.RawMessage {
	t.Helper()
	root, err := defaultPortalDir()
	if err != nil {
		t.Fatalf("defaultPortalDir: %v", err)
	}
	raw, err := os.ReadFile(filepath.Join(root, "testdata", name))
	if err != nil {
		t.Fatalf("read %s: %v", name, err)
	}
	var top map[string]json.RawMessage
	if err := json.Unmarshal(raw, &top); err != nil {
		t.Fatalf("%s is not a JSON object: %v", name, err)
	}
	return top
}

// aisVessels returns the vessel array as raw key sets, because that is the
// only way to tell "the field is absent" from "the field is zero" — the
// distinction this whole panel turns on, and one a decode into a struct
// destroys.
func aisVessels(t *testing.T, name string) []map[string]json.RawMessage {
	t.Helper()
	top := loadAisFixture(t, name)
	rawVessels, ok := top["vessels"]
	if !ok {
		t.Fatalf("%s has no vessels key", name)
	}
	var vessels []map[string]json.RawMessage
	if err := json.Unmarshal(rawVessels, &vessels); err != nil {
		t.Fatalf("%s: vessels is not an array of objects: %v", name, err)
	}
	return vessels
}

func aisPacketsValid(t *testing.T, name string) float64 {
	t.Helper()
	top := loadAisFixture(t, name)
	rawStats, ok := top["stats"]
	if !ok {
		t.Fatalf("%s has no stats key — the panel's only backend statistic", name)
	}
	var stats struct {
		PacketsValid *float64 `json:"packets_valid"`
	}
	if err := json.Unmarshal(rawStats, &stats); err != nil {
		t.Fatalf("%s: stats does not decode: %v", name, err)
	}
	if stats.PacketsValid == nil {
		t.Fatalf("%s: stats.packets_valid is absent", name)
	}
	return *stats.PacketsValid
}

// TestAisFixtureKeepsAbsentFieldsAbsent is the contract's firmest rule, and
// several bugs in this project have come from breaking it. A vessel that has
// not broadcast its name has NO name key — not "", not null. A vessel with no
// position fix has NEITHER lat NOR lon — not 0, which would put it in the Gulf
// of Guinea and on the chart.
func TestAisFixtureKeepsAbsentFieldsAbsent(t *testing.T) {
	optionalStrings := []string{"name", "callsign", "destination", "time"}

	for _, fixture := range []string{"ais.json", "ais_empty.json"} {
		for i, v := range aisVessels(t, fixture) {
			if _, ok := v["mmsi"]; !ok {
				t.Errorf("%s vessel %d has no mmsi — it is the entry's key on the "+
					"C++ side and can never be absent", fixture, i)
			}
			for _, key := range optionalStrings {
				rawValue, present := v[key]
				if !present {
					continue
				}
				var s string
				if err := json.Unmarshal(rawValue, &s); err != nil {
					t.Errorf("%s vessel %d: %s is %s, want a string", fixture, i, key, rawValue)
					continue
				}
				if s == "" {
					t.Errorf("%s vessel %d carries an empty %q — an absent value must "+
						"be an absent KEY, not an empty string the panel then has to "+
						"treat as absent all over again", fixture, i, key)
				}
			}
			_, hasLat := v["lat"]
			_, hasLon := v["lon"]
			if hasLat != hasLon {
				t.Errorf("%s vessel %d has lat=%v lon=%v — the contract omits BOTH "+
					"unless the app's own validity gate passed", fixture, i, hasLat, hasLon)
			}
			if raw, ok := v["nav_status"]; ok {
				var status int
				if err := json.Unmarshal(raw, &status); err != nil {
					t.Errorf("%s vessel %d: nav_status is %s, want an integer", fixture, i, raw)
				} else if status < 0 || status > 15 {
					t.Errorf("%s vessel %d: nav_status = %d, want 0..15 (the app holds "+
						"-1 for 'never reported', and the contract omits the field for it)",
						fixture, i, status)
				}
			}
		}
	}
}

// TestAisFixtureCoversTheThreeCasesThePanelExistsFor. A fixture of six
// well-behaved ships would demonstrate nothing: every interesting rule in this
// panel is about a vessel that has NOT sent something. If any of these three
// disappears, the harness stops being able to show the behaviour and the next
// person to touch the renderer has no way to see they broke it.
func TestAisFixtureCoversTheThreeCasesThePanelExistsFor(t *testing.T) {
	vessels := aisVessels(t, "ais.json")
	if len(vessels) == 0 {
		t.Fatal("ais.json has no vessels")
	}

	var named, mmsiOnly, unlocated int
	for _, v := range vessels {
		_, hasPos := v["lat"]
		_, hasName := v["name"]
		_, hasHeading := v["heading_deg"]
		_, hasSog := v["sog_kn"]

		if hasPos && hasName && hasHeading && hasSog {
			named++
		}
		// The glyph rule: a position but no orientation at all, which must be
		// drawn as a ringed dot rather than a hull pointing due north.
		if _, hasCog := v["cog_deg"]; hasPos && !hasName && !hasHeading && !hasCog {
			mmsiOnly++
		}
		if !hasPos {
			unlocated++
		}
	}

	if named == 0 {
		t.Error("ais.json has no named vessel with both a heading and a SOG — " +
			"the case that exercises hull rotation and the speed vector")
	}
	if mmsiOnly == 0 {
		t.Error("ais.json has no MMSI-only vessel that has a position but has " +
			"never reported an orientation — the case that must be drawn as an " +
			"unrotated dot, and the one a fabricated heading would silently ruin")
	}
	if unlocated == 0 {
		t.Error("ais.json has no vessel without a position — the case that must " +
			"appear in the table and NOT on the chart, which is the rule the " +
			"whole absent-stays-absent contract exists to protect")
	}
}

// TestAisEmptyFixtureIsAnHonestNothingHeard. The empty case is not a
// throwaway: it is what the panel looks like for the first minute of every
// session, and it has to carry the same shape (an empty array and a real
// statistic) rather than a null or a missing key that the renderer would have
// to guess at.
func TestAisEmptyFixtureIsAnHonestNothingHeard(t *testing.T) {
	if got := len(aisVessels(t, "ais_empty.json")); got != 0 {
		t.Errorf("ais_empty.json has %d vessels, want 0", got)
	}
	if got := aisPacketsValid(t, "ais_empty.json"); got != 0 {
		t.Errorf("ais_empty.json packets_valid = %v, want 0", got)
	}
	if got := aisPacketsValid(t, "ais.json"); got <= 0 {
		t.Errorf("ais.json packets_valid = %v, want a real count", got)
	}
}

// TestAisFixturesAreServed keeps harness.html's two AIS buttons honest: they
// fetch these files over HTTP, and a fixture that exists on disk but is not
// reachable through the harness mux is a button that silently does nothing.
func TestAisFixturesAreServed(t *testing.T) {
	root, err := defaultPortalDir()
	if err != nil {
		t.Fatalf("defaultPortalDir: %v", err)
	}
	mux, err := newMux(root)
	if err != nil {
		t.Fatalf("newMux(%q): %v", root, err)
	}
	ts := httptest.NewServer(mux)
	defer ts.Close()

	for _, path := range []string{
		"/testdata/ais.json",
		"/testdata/ais_empty.json",
		"/server/static/panels/ais.js",
	} {
		resp, err := http.Get(ts.URL + path)
		if err != nil {
			t.Fatalf("GET %s: %v", path, err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			t.Errorf("GET %s: status = %d, want 200", path, resp.StatusCode)
		}
	}
}
