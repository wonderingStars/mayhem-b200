// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package server

import (
	"encoding/json"
	"math"
	"os/exec"
	"strings"
	"testing"

	"mayhemb200/webgui/internal/portal/tiles"
)

// The map panel's OpenStreetMap basemap, from both directions.
//
// The source-level guards below pin the two things that cannot be recovered
// once they are lost: the licence condition (the credit and its link) and the
// rule that the browser only ever asks THIS server for a tile, so the portal
// still works on an air-gapped machine. Those are string checks because Go
// cannot run JS on its own.
//
// The rest of the basemap contract is behaviour, not text — which URLs are
// requested, what is still drawn when none of them answer, when the credit
// appears, whether a dead endpoint gets hammered forever — so
// TestMapPanel_BasemapBehaviour runs the real renderer under node against a
// stub DOM (testdata/map_basemap_probe.js) and asserts on what it did. That
// test skips where node is unavailable; the source guards do not.
//
// Own file rather than an addition to assets_test.go, which another change-set
// may be editing.

// mapPanelJS is panels/map.js as it is actually embedded and served.
func mapPanelJS(t *testing.T) string {
	t.Helper()
	return readAsset(t, "panels/map.js")
}

// TestMapPanelCarriesOSMAttribution pins a licence condition, not a style
// choice, exactly as its adsb sibling does (see
// TestEmbeddedAssets_AdsbPanelCarriesOSMAttribution). The OSM tile usage policy
// and the ODbL attribution guidelines both require "© OpenStreetMap
// contributors" to be visible whenever the basemap is; map.js is now a second
// place in this repo that draws those tiles, and so a second place that owes
// the credit. Without this a refactor could delete it — putting the project in
// breach of the terms that let it use those servers at all — with every other
// test still green.
func TestMapPanelCarriesOSMAttribution(t *testing.T) {
	js := mapPanelJS(t)
	for _, want := range []string{
		"© OpenStreetMap contributors",
		"https://www.openstreetmap.org/copyright",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("panels/map.js no longer contains %q — the OSM basemap may not be "+
				"attributed, which is a licence violation, not a cosmetic regression", want)
		}
	}
}

// TestMapPanelsFetchTilesFromThisServerOnly is the air-gap guard. Both map
// renderers must name the portal's own route and nothing else: the caching,
// revalidation, back-off and User-Agent the OSM policy requires all live in
// internal/portal/tiles, and a page that fetched tile.openstreetmap.org
// directly would bypass every one of them AND break on a machine with no
// route to the internet — which is a normal way to run this portal.
//
// Pinned against tiles.PathPrefix rather than a literal so renaming the route
// in Go fails here instead of silently 404ing in a browser.
func TestMapPanelsFetchTilesFromThisServerOnly(t *testing.T) {
	for _, name := range []string{"panels/map.js", "panels/adsb.js"} {
		js := readAsset(t, name)
		if !strings.Contains(js, `"`+tiles.PathPrefix) {
			t.Errorf("%s does not request tiles from %q — the panel either has no basemap "+
				"or is fetching one from somewhere this server does not control",
				name, tiles.PathPrefix)
		}
		// The upstream host belongs in internal/portal/tiles and nowhere else.
		// (The copyright link is www.openstreetmap.org, a different host, and is
		// required — this only rejects the tile server itself.)
		if strings.Contains(js, "tile.openstreetmap.org") {
			t.Errorf("%s names the upstream tile server directly. Tiles must go through %q "+
				"so the cache, the revalidation, the back-off and the User-Agent the OSM "+
				"usage policy requires are not bypassed — and so the portal still works "+
				"air-gapped", name, tiles.PathPrefix)
		}
	}
}

// ---------------------------------------------------------------------------
// Behavioural: the renderer, actually run
// ---------------------------------------------------------------------------

type probeReport struct {
	Pending struct {
		TileRequests   []string `json:"tileRequests"`
		LabelsDrawn    []string `json:"labelsDrawn"`
		DrawImages     int      `json:"drawImages"`
		AttribDisplay  string   `json:"attribDisplay"`
		OfflineDisplay string   `json:"offlineDisplay"`
	} `json:"pending"`
	Online struct {
		TileRequests    []string `json:"tileRequests"`
		DrawImages      int      `json:"drawImages"`
		LabelsDrawn     []string `json:"labelsDrawn"`
		AttribDisplay   string   `json:"attribDisplay"`
		OfflineDisplay  string   `json:"offlineDisplay"`
		TilesUnderMarks bool     `json:"lastDrawImageBeforeFirstMarkerLabel"`
	} `json:"online"`
	Offline struct {
		FirstFrameRequests      int      `json:"firstFrameRequests"`
		RequestsAfterGivingUp   int      `json:"requestsAfterGivingUp"`
		RequestsAfterFurtherUse int      `json:"requestsAfterFurtherUse"`
		GaveUp                  bool     `json:"gaveUp"`
		LabelsDrawn             []string `json:"labelsDrawn"`
		Strokes                 int      `json:"strokes"`
		DrawImages              int      `json:"drawImages"`
		AttribDisplay           string   `json:"attribDisplay"`
		OfflineDisplay          string   `json:"offlineDisplay"`
		OfflineText             string   `json:"offlineText"`
	} `json:"offline"`
	Partial struct {
		Requested      int      `json:"requested"`
		DrawImages     int      `json:"drawImages"`
		LabelsDrawn    []string `json:"labelsDrawn"`
		Strokes        int      `json:"strokes"`
		GaveUp         bool     `json:"gaveUp"`
		AttribDisplay  string   `json:"attribDisplay"`
		OfflineDisplay string   `json:"offlineDisplay"`
	} `json:"partial"`
	Markers struct {
		MarkerCount    int        `json:"markerCount"`
		IDs            []string   `json:"ids"`
		Headings       []*float64 `json:"headings"`
		Rotations      []float64  `json:"rotations"`
		Arcs           int        `json:"arcs"`
		ClickedAt      []float64  `json:"clickedAt"`
		SelectedID     string     `json:"selectedId"`
		TooltipText    string     `json:"tooltipText"`
		TooltipDisplay string     `json:"tooltipDisplay"`
		CountText      string     `json:"countText"`
	} `json:"markers"`
	Extremes struct {
		ZoomedOut     float64  `json:"zoomedOut"`
		ZoomedIn      float64  `json:"zoomedIn"`
		URLsAtMinZoom int      `json:"urlsAtMinZoom"`
		TileRequests  []string `json:"tileRequests"`
	} `json:"extremes"`
	Fit struct {
		Zoom      float64 `json:"zoom"`
		CenterLat float64 `json:"centerLat"`
		CenterLon float64 `json:"centerLon"`
		Viewport  []int   `json:"viewport"`
		Positions []struct {
			Label string  `json:"label"`
			X     float64 `json:"x"`
			Y     float64 `json:"y"`
		} `json:"positions"`
	} `json:"fit"`
}

// runProbe executes testdata/map_basemap_probe.js against the served map.js.
func runProbe(t *testing.T) probeReport {
	t.Helper()
	node, err := exec.LookPath("node")
	if err != nil {
		t.Skip("node not on PATH: skipping the behavioural basemap probe " +
			"(the source-level guards in this file still ran)")
	}
	// The probe reads the file from disk rather than through Assets(): it runs
	// outside the Go process. static/ is the same tree go:embed picks up, and
	// TestEmbeddedAssets_Present already covers the two agreeing.
	out, err := exec.Command(node, "testdata/map_basemap_probe.js", "static/panels/map.js").Output()
	if err != nil {
		if ee, ok := err.(*exec.ExitError); ok {
			t.Fatalf("probe failed: %v\n%s", err, ee.Stderr)
		}
		t.Fatalf("probe failed: %v", err)
	}
	var rep probeReport
	if err := json.Unmarshal(out, &rep); err != nil {
		t.Fatalf("probe output is not the expected JSON: %v\n%s", err, out)
	}
	return rep
}

func hasAll(got, want []string) []string {
	seen := map[string]bool{}
	for _, g := range got {
		seen[g] = true
	}
	var missing []string
	for _, w := range want {
		if !seen[w] {
			missing = append(missing, w)
		}
	}
	return missing
}

// wardriveLabels are the marker labels the probe's fixture carries.
var wardriveLabels = []string{"UAL123", "BAW29X", "DLH4YB", "Vessel 235887"}

// TestMapPanel_BasemapBehaviour drives panels/map.js for real. Each subtest is
// one clause of the basemap contract in PANELS.md's `map` section.
func TestMapPanel_BasemapBehaviour(t *testing.T) {
	rep := runProbe(t)

	// Every tile the renderer asks for must be a path THIS server's own parser
	// accepts. Checking it with tiles.ParsePath rather than a regexp closes the
	// loop: the URL the JS builds and the URL the Go proxy validates are the
	// same contract, and a z beyond MaxZoom (or an x/y off the edge of the
	// world at that z) is a 400 rather than a basemap.
	t.Run("tiles are requested from this server's own tile route", func(t *testing.T) {
		if len(rep.Pending.TileRequests) == 0 {
			t.Fatal("the map panel requested no tiles at all — it has no basemap")
		}
		for _, u := range rep.Pending.TileRequests {
			if strings.Contains(u, "://") {
				t.Errorf("tile URL %q names a host; it must be root-relative so the "+
					"portal works air-gapped and the proxy's caching/User-Agent apply", u)
				continue
			}
			if _, err := tiles.ParsePath(u); err != nil {
				t.Errorf("tile URL %q is not a path this server would serve: %v", u, err)
			}
		}
	})

	// Zooming to both stops is where a tile index goes out of range: at the
	// bottom the world wraps east-west under a canvas wider than it, at the top
	// the map runs past the deepest z the endpoint serves. Either produces a 400
	// per tile instead of a basemap.
	t.Run("tile URLs stay in range at both ends of the zoom", func(t *testing.T) {
		if rep.Extremes.ZoomedOut >= rep.Extremes.ZoomedIn {
			t.Fatalf("zoom did not move: out %v, in %v", rep.Extremes.ZoomedOut, rep.Extremes.ZoomedIn)
		}
		if rep.Extremes.URLsAtMinZoom <= len(rep.Pending.TileRequests) {
			t.Fatalf("zooming out requested no new tiles (%d, was %d) — the probe "+
				"never reached the low-zoom wrap it is here to cover",
				rep.Extremes.URLsAtMinZoom, len(rep.Pending.TileRequests))
		}
		if rep.Extremes.ZoomedIn > float64(tiles.MaxZoom) {
			t.Errorf("zoomed to %v, past the deepest zoom the endpoint serves (%d)",
				rep.Extremes.ZoomedIn, tiles.MaxZoom)
		}
		bad := 0
		for _, u := range rep.Extremes.TileRequests {
			if _, err := tiles.ParsePath(u); err != nil {
				if bad < 5 {
					t.Errorf("tile URL %q would be answered 400, not a tile: %v", u, err)
				}
				bad++
			}
		}
		if bad > 5 {
			t.Errorf("...and %d more out-of-range tile URLs", bad-5)
		}
	})

	// Markers first, streets when they arrive. The first frame happens before
	// any tile can possibly have loaded, and every marker must already be on it.
	t.Run("markers are drawn before any tile has answered", func(t *testing.T) {
		if rep.Pending.DrawImages != 0 {
			t.Fatalf("drew %d tiles before any had loaded — the probe is wrong",
				rep.Pending.DrawImages)
		}
		if missing := hasAll(rep.Pending.LabelsDrawn, wardriveLabels); len(missing) > 0 {
			t.Errorf("markers %v were not drawn on the first frame — the renderer is "+
				"waiting for the basemap before showing the data", missing)
		}
	})

	// The credit is owed for tiles, and only for tiles.
	t.Run("the OSM credit appears only once a tile is painted", func(t *testing.T) {
		if rep.Pending.AttribDisplay != "none" {
			t.Errorf("attribution display = %q with no tile drawn yet, want %q: "+
				"crediting OpenStreetMap for a map it did not supply is a false claim "+
				"about where the picture came from", rep.Pending.AttribDisplay, "none")
		}
		if rep.Online.AttribDisplay != "block" {
			t.Errorf("attribution display = %q with tiles on screen, want %q — "+
				"the OSM tile usage policy requires the credit to be visible whenever "+
				"the tiles are", rep.Online.AttribDisplay, "block")
		}
		if rep.Offline.AttribDisplay != "none" {
			t.Errorf("attribution display = %q with the basemap unavailable, want %q",
				rep.Offline.AttribDisplay, "none")
		}
	})

	t.Run("tiles are painted under the markers, not over them", func(t *testing.T) {
		if rep.Online.DrawImages == 0 {
			t.Fatal("no tile was ever painted even though every request succeeded")
		}
		if missing := hasAll(rep.Online.LabelsDrawn, wardriveLabels); len(missing) > 0 {
			t.Errorf("markers %v vanished from the frame that drew tiles", missing)
		}
		if !rep.Online.TilesUnderMarks {
			t.Error("a marker label was drawn before the last tile of the same frame — " +
				"the basemap would cover the data it is supposed to sit under")
		}
	})

	// The honest degrade: -tiles off, an air-gapped box, a blocked proxy.
	t.Run("an unreachable basemap falls back to the graticule and stops asking", func(t *testing.T) {
		if !rep.Offline.GaveUp {
			t.Error("the tile layer never gave up after every request failed — " +
				"it will keep asking a dead endpoint for as long as the panel is open")
		}
		if rep.Offline.RequestsAfterFurtherUse != rep.Offline.RequestsAfterGivingUp {
			t.Errorf("re-rendering, panning and zooming after giving up issued %d more "+
				"tile requests (%d -> %d)",
				rep.Offline.RequestsAfterFurtherUse-rep.Offline.RequestsAfterGivingUp,
				rep.Offline.RequestsAfterGivingUp, rep.Offline.RequestsAfterFurtherUse)
		}
		if rep.Offline.DrawImages != 0 {
			t.Errorf("drew %d tiles with none available", rep.Offline.DrawImages)
		}
		if rep.Offline.Strokes == 0 {
			t.Error("no graticule was drawn with the basemap unavailable — the panel " +
				"has no spatial reference at all, which is worse than it was before " +
				"tiles existed")
		}
		if missing := hasAll(rep.Offline.LabelsDrawn, wardriveLabels); len(missing) > 0 {
			t.Errorf("markers %v are missing with no basemap — an offline map must "+
				"still show its data", missing)
		}
		if rep.Offline.OfflineDisplay != "block" {
			t.Errorf("offline note display = %q, want %q: the degrade is reported once, "+
				"quietly, rather than left to look like a broken panel",
				rep.Offline.OfflineDisplay, "block")
		}
		if !strings.Contains(rep.Offline.OfflineText, "Basemap unavailable") {
			t.Errorf("offline note reads %q", rep.Offline.OfflineText)
		}
	})

	// Half the tiles answer and half never will: the fallback shows through the
	// gaps, so there is no permanent half-loaded checkerboard.
	t.Run("a partly answered basemap keeps the fallback under the gaps", func(t *testing.T) {
		if rep.Partial.GaveUp {
			t.Fatal("gave the basemap up while tiles were still loading successfully — " +
				"the give-up rule is for an endpoint that has never answered at all, " +
				"not for a few failures among working tiles")
		}
		if rep.Partial.DrawImages == 0 || rep.Partial.DrawImages >= rep.Partial.Requested {
			t.Fatalf("drew %d of %d requested tiles — the probe did not produce a "+
				"partial basemap", rep.Partial.DrawImages, rep.Partial.Requested)
		}
		if rep.Partial.Strokes == 0 {
			t.Error("no graticule under a partly loaded basemap: the tiles that never " +
				"arrive leave holes with nothing in them")
		}
		if missing := hasAll(rep.Partial.LabelsDrawn, wardriveLabels); len(missing) > 0 {
			t.Errorf("markers %v are missing from a partly loaded basemap", missing)
		}
		if rep.Partial.AttribDisplay != "block" {
			t.Errorf("attribution display = %q with some tiles on screen, want %q",
				rep.Partial.AttribDisplay, "block")
		}
	})

	// Requirement: presentation only. The wire's marker spelling must still
	// reach the drawing code exactly as it did before there was a basemap.
	t.Run("the contract's marker shape still reaches the drawing code", func(t *testing.T) {
		if rep.Markers.MarkerCount != 2 {
			t.Fatalf("normalized %d markers, want 2", rep.Markers.MarkerCount)
		}
		if rep.Markers.Headings[0] == nil || *rep.Markers.Headings[0] != 272 {
			t.Errorf("heading_deg 272 did not become heading: got %v", rep.Markers.Headings[0])
		}
		if rep.Markers.Headings[1] != nil {
			t.Errorf("a marker with no heading in either spelling got heading %v — "+
				"a defaulted 0 reads as 'due north'", *rep.Markers.Headings[1])
		}
		if len(rep.Markers.Rotations) != 1 {
			t.Fatalf("%d marker glyphs were rotated, want exactly 1 (the one with a heading)",
				len(rep.Markers.Rotations))
		}
		if want := 272 * math.Pi / 180; math.Abs(rep.Markers.Rotations[0]-want) > 1e-9 {
			t.Errorf("marker rotated %v rad, want %v (272°)", rep.Markers.Rotations[0], want)
		}
		if rep.Markers.Arcs != 1 {
			t.Errorf("%d markers were drawn as plain dots, want exactly 1 (the one with "+
				"no heading)", rep.Markers.Arcs)
		}
		if rep.Markers.IDs[0] == rep.Markers.IDs[1] {
			t.Errorf("both markers got id %q — with ids that compare equal one click "+
				"selects every marker at once", rep.Markers.IDs[0])
		}
	})

	t.Run("clicking a marker selects that marker", func(t *testing.T) {
		if rep.Markers.SelectedID != rep.Markers.IDs[1] {
			t.Fatalf("clicked the second marker, selected %q (ids: %v)",
				rep.Markers.SelectedID, rep.Markers.IDs)
		}
		if rep.Markers.TooltipDisplay != "block" {
			t.Errorf("tooltip display = %q after a click on a marker", rep.Markers.TooltipDisplay)
		}
		if !strings.HasPrefix(rep.Markers.TooltipText, "VM-9F2C") {
			t.Errorf("tooltip reads %q — it must describe the marker that was clicked, "+
				"not another one", rep.Markers.TooltipText)
		}
	})

	// Fit-to-markers survives the projection change: in Mercator a bounding box
	// fitted in degrees is both mis-scaled and centred north of its markers.
	t.Run("fit to markers frames every marker", func(t *testing.T) {
		w, h := float64(rep.Fit.Viewport[0]), float64(rep.Fit.Viewport[1])
		if len(rep.Fit.Positions) != len(wardriveLabels) {
			t.Fatalf("%d markers landed on the canvas, want %d",
				len(rep.Fit.Positions), len(wardriveLabels))
		}
		for _, p := range rep.Fit.Positions {
			if p.X < 0 || p.X > w || p.Y < 0 || p.Y > h {
				t.Errorf("marker %s sits at (%.1f, %.1f), outside the %.0fx%.0f canvas "+
					"after fit-to-markers", p.Label, p.X, p.Y, w, h)
			}
		}
		// A fit that framed nothing (or everything) would still pass the bounds
		// check above; the markers have to fill a useful part of the view.
		minX, maxX := math.Inf(1), math.Inf(-1)
		minY, maxY := math.Inf(1), math.Inf(-1)
		for _, p := range rep.Fit.Positions {
			minX, maxX = math.Min(minX, p.X), math.Max(maxX, p.X)
			minY, maxY = math.Min(minY, p.Y), math.Max(maxY, p.Y)
		}
		if (maxX-minX) < w*0.5 && (maxY-minY) < h*0.5 {
			t.Errorf("markers span %.0fx%.0f px in a %.0fx%.0f canvas — fit-to-markers "+
				"zoomed out far more than the spread needs", maxX-minX, maxY-minY, w, h)
		}
		if rep.Fit.Zoom < 2 || rep.Fit.Zoom > float64(tiles.MaxZoom) {
			t.Errorf("fit chose zoom %v, outside the range the tile endpoint serves (2..%d)",
				rep.Fit.Zoom, tiles.MaxZoom)
		}
	})
}
