// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package server

import (
	"regexp"
	"strings"
	"testing"
)

// Source-level guards for the "ais" panel, in the same spirit — and for the
// same reason — as marker_contract_test.go: the C++ and JS halves of this
// panel are written from one contract but built separately, and both sides'
// tests stay green while the field names disagree in the middle. Go cannot run
// the JS, so these are deliberately narrow checks that pin the identifiers a
// drift would turn on, not a pretend renderer test.
//
// Own file rather than an addition to assets_test.go, which another change-set
// may be editing.

// stripLineComments drops everything from a `//` to the end of its line.
//
// It is deliberately naive — it does not know about strings, so a `//` inside
// a URL literal truncates that line too. That is acceptable here because the
// only thing scanned through it is a pattern that would appear in expression
// code, and it is much better than the alternative: without it, a file cannot
// document the idiom it is forbidden to use.
func stripLineComments(js string) string {
	var b strings.Builder
	for _, line := range strings.Split(js, "\n") {
		if i := strings.Index(line, "//"); i >= 0 {
			line = line[:i]
		}
		b.WriteString(line)
		b.WriteByte('\n')
	}
	return b.String()
}

// TestAisPanelRegistersItsOwnKind pins the kind string itself. The panels
// wiring check (TestEmbeddedAssets_PanelScriptsAreWiredUp) only proves the
// FILE is loaded; a file that loads but registers under the wrong name leaves
// the AIS app falling back to the "screen" card with every test still green.
func TestAisPanelRegistersItsOwnKind(t *testing.T) {
	js := readAsset(t, "panels/ais.js")
	if !strings.Contains(js, `registerPanel("ais"`) {
		t.Error(`panels/ais.js does not call registerPanel("ais", ...) — the AIS ` +
			`app's panel_kind has no renderer and falls back to the "screen" card`)
	}
}

// TestGeoTablePanelStillRegistersItsKind is the other half of the same change:
// AIS moved off `geotable`, but APRS RX did not. Deleting or renaming the
// geotable renderer while AIS was being given its own would take the APRS map
// down silently.
func TestGeoTablePanelStillRegistersItsKind(t *testing.T) {
	js := readAsset(t, "panels/geotable.js")
	if !strings.Contains(js, `registerPanel("geotable"`) {
		t.Error("panels/geotable.js no longer registers the \"geotable\" kind — " +
			"APRS RX still publishes it and would fall back to the \"screen\" card")
	}
}

// TestAisPanelReadsEveryContractField pins every field name on the wire, as
// the accessor that reads it. A backend field renamed on one side only is the
// exact failure this project has hit repeatedly (map.js reading `m.heading`
// for a wire that says `heading_deg`), and for absent-by-design fields it is
// invisible: the panel simply draws nothing, which is also what a vessel that
// never reported the value looks like.
func TestAisPanelReadsEveryContractField(t *testing.T) {
	js := readAsset(t, "panels/ais.js")

	for _, want := range []string{
		"d.vessels",
		"raw.mmsi",
		"raw.name",
		"raw.callsign",
		"raw.destination",
		"raw.lat",
		"raw.lon",
		"raw.sog_kn",
		"raw.cog_deg",
		"raw.heading_deg",
		"raw.nav_status",
		"raw.msgs",
		"raw.time",
		"stats.packets_valid",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("panels/ais.js never reads %s — that field of the /api/panel "+
				"payload is silently ignored, which looks exactly like a vessel "+
				"that never reported it", want)
		}
	}
}

// TestAisPanelNeverCoercesAnAbsentFieldToZero is the absent-stays-absent rule
// as an automated check.
//
// `Number(raw.x) || 0` is the idiomatic-looking line that breaks it: it turns
// a heading that was never broadcast into 000 (due north), a speed that was
// never broadcast into "stopped", and a navigational status that was never
// broadcast into 0, "under way using engine". Every one of those is a
// confident claim about a vessel, made up by the browser. The panel's num()
// helper exists precisely so the normaliser has a reader that yields NaN
// instead, and every consumer tests Number.isFinite.
func TestAisPanelNeverCoercesAnAbsentFieldToZero(t *testing.T) {
	js := readAsset(t, "panels/ais.js")

	if !strings.Contains(js, "function num(") {
		t.Fatal("panels/ais.js has no num() reader — the normaliser is reading " +
			"payload fields some other way, and the absent-stays-absent rule is " +
			"no longer enforced in one place")
	}

	// `raw.<field> || 0`, in any spacing, with or without a Number() wrapper.
	// Scanned over the CODE only: the file's own comments name the forbidden
	// idiom in order to explain why it is forbidden, and a check that cannot
	// tell those apart would force the explanation out of the file.
	coerce := regexp.MustCompile(`raw\.[A-Za-z_]+\s*\)?\s*\|\|\s*0`)
	if m := coerce.FindString(stripLineComments(js)); m != "" {
		t.Errorf("panels/ais.js contains %q — an absent payload field must stay "+
			"absent (NaN), never become 0: a missing heading would be drawn as "+
			"due north and a missing SOG would read as stopped", m)
	}
}

// TestAisPanelNeverFabricatesAnOrientation pins the two-field resolution the
// contract forces. `heading_deg` (true heading) and `cog_deg` (course over
// ground) are separately optional, they measure different things, and a vessel
// that has broadcast neither must be drawn as a glyph with no direction at all
// rather than a hull pointing north.
func TestAisPanelNeverFabricatesAnOrientation(t *testing.T) {
	js := readAsset(t, "panels/ais.js")

	if !strings.Contains(js, "function orientationOf(") {
		t.Fatal("panels/ais.js has no orientationOf() — the heading/COG fallback " +
			"is no longer in one place, and a marker with neither field can be " +
			"drawn on a fabricated course")
	}
	// The glyph is chosen by whether an orientation exists, not by rotating a
	// default. Losing this branch is what turns "unknown" into "due north".
	if !strings.Contains(js, "Number.isFinite(orientDeg)") {
		t.Error("panels/ais.js no longer branches on Number.isFinite(orientDeg) — " +
			"a vessel that has never reported a heading or a course would be " +
			"drawn as a hull pointing somewhere it never claimed to point")
	}
}

// TestAisPanelCountsWithPositionInTheBrowserAndSaysSo. The vessels-with-
// position count is not on the wire: it is counted here from which vessels
// arrived with coordinates. Printed unlabelled beside packets_valid — which IS
// a backend statistic — it would pass as one, so the label is part of the
// number's honesty rather than decoration.
func TestAisPanelCountsWithPositionInTheBrowserAndSaysSo(t *testing.T) {
	js := readAsset(t, "panels/ais.js")
	if !strings.Contains(js, "counted in browser") {
		t.Error("panels/ais.js no longer labels the with-position count as the " +
			"browser's own — it is derived here, not sent by the backend, and " +
			"an unlabelled figure next to packets_valid reads as a backend stat")
	}
}

// TestMapPanelsCarryOSMAttributionAsRenderedMarkup pins a licence condition,
// not a style choice: the OSM tile usage policy and the ODbL attribution
// guidelines both require "© OpenStreetMap contributors" to be visible
// whenever the basemap is. There is no JS test infrastructure here, so without
// this a refactor could delete the attribution — putting the project in breach
// of the terms that let it use those servers at all — with every Go test green.
//
// It checks BOTH tile-drawing panels, and it checks the MARKUP rather than the
// bare phrase. That is the whole point of writing it this way: both files
// explain the obligation in a header comment that quotes the phrase verbatim,
// so a check for the phrase alone is satisfied by the prose and stays green
// with the attribution element deleted — measured, not assumed
// (TestEmbeddedAssets_AdsbPanelCarriesOSMAttribution has exactly that hole).
// The closing </a>, the element class and the display toggle together can only
// be satisfied by something that actually reaches the page.
func TestMapPanelsCarryOSMAttributionAsRenderedMarkup(t *testing.T) {
	for _, panel := range []struct{ file, class string }{
		{"panels/ais.js", "mp-ais-attrib"},
		{"panels/adsb.js", "mp-adsb-attrib"},
	} {
		js := readAsset(t, panel.file)
		for _, want := range []string{
			">© OpenStreetMap contributors</a>",
			"https://www.openstreetmap.org/copyright",
			panel.class,
			"attribEl.style.display",
		} {
			if !strings.Contains(js, want) {
				t.Errorf("%s no longer contains %q — the OSM basemap may not be "+
					"attributed on screen, which is a licence violation, not a "+
					"cosmetic regression", panel.file, want)
			}
		}
	}
}

// TestAisPanelFetchesTilesOnlyThroughThePortalProxy. The caching, the 7-day
// minimum retention, the revalidation and the identifying User-Agent that the
// OSM tile usage policy demands all live in internal/portal/tiles, on the
// server. A panel that pointed an <img> straight at a tile server would bypass
// every one of them — and would also break on an air-gapped machine, where the
// portal answers 503 and the panel is supposed to fall back to its graticule.
func TestAisPanelFetchesTilesOnlyThroughThePortalProxy(t *testing.T) {
	js := readAsset(t, "panels/ais.js")
	if !strings.Contains(js, `"/api/tiles/"`) {
		t.Error(`panels/ais.js does not fetch tiles from "/api/tiles/" — the ` +
			`same-origin proxy route the portal and the harness both serve`)
	}
	for _, forbidden := range []string{
		"tile.openstreetmap.org",
		"tile.opentopomap.org",
		"basemaps.cartocdn.com",
	} {
		if strings.Contains(js, forbidden) {
			t.Errorf("panels/ais.js names the upstream tile host %q — the browser "+
				"must never contact a tile server directly; that path has no "+
				"cache, no revalidation and no identifying User-Agent", forbidden)
		}
	}
}
