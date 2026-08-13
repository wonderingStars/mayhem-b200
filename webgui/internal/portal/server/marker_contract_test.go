// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package server

import (
	"io/fs"
	"strings"
	"testing"
)

// Guards for the C++/JS drift that has bitten this project repeatedly: the two
// halves are written from one contract but built separately, and both sides'
// tests stay green while the field names disagree in the middle. Go cannot run
// the JS, so these are source-level checks — deliberately narrow, pinning the
// one identifier each bug turned on rather than pretending to validate the
// renderer.
//
// The live bug they were written for: src/remote/app_data.cpp's
// to_json(MapData) emits {lat, lon, label, heading_deg?} with no id, while
// panels/map.js read `m.heading` and `m.id`. Every WardriveMap marker therefore
// compared equal to every other, so one click highlighted all of them and the
// tooltip printed the FIRST marker's name and coordinates — a real value
// belonging to a different capture.
//
// Own file rather than an addition to assets_test.go, which another change-set
// may be editing.

func readAsset(t *testing.T, name string) string {
	t.Helper()
	data, err := fs.ReadFile(Assets(), name)
	if err != nil {
		t.Fatalf("ReadFile(%q): %v", name, err)
	}
	return string(data)
}

// TestMapPanelReadsTheContractsMarkerFields pins that the map renderer still
// understands the marker shape the C++ backend actually sends.
func TestMapPanelReadsTheContractsMarkerFields(t *testing.T) {
	js := readAsset(t, "panels/map.js")

	// The wire spells the heading for its unit; the renderer's own field is
	// `heading`. Reading only one of them silently drops the other.
	if !strings.Contains(js, "heading_deg") {
		t.Error("panels/map.js never mentions heading_deg — the field name " +
			"src/remote/app_data.cpp's to_json(MapData) emits. A marker that " +
			"reports its heading will be drawn as a plain dot.")
	}
	if !strings.Contains(js, "m.heading") && !strings.Contains(js, "heading:") {
		t.Error("panels/map.js no longer reads its own `heading` field")
	}

	// Marker selection is keyed on `id`, which the contract has no field for,
	// so the renderer has to synthesize one.
	if !strings.Contains(js, "normalizeMarkers") {
		t.Fatal("panels/map.js has no normalizeMarkers() — markers reach the " +
			"draw path unadapted, and one click selects every marker at once")
	}
}

// TestGeoTablePanelDoesNotDuplicateTheMarkerAdaptation keeps the adaptation in
// exactly one place. It used to live in geotable.js alone, which is precisely
// why the direct `map` kind (WardriveMap) went without any.
func TestGeoTablePanelDoesNotDuplicateTheMarkerAdaptation(t *testing.T) {
	js := readAsset(t, "panels/geotable.js")
	if strings.Contains(js, "adaptMarkers") {
		t.Error("panels/geotable.js has its own marker adapter again — " +
			"normalizeMarkers() in panels/map.js covers every host of that " +
			"renderer, and a second copy is how the two drift apart")
	}
}

// TestEmbeddedAssets_TopLevelScriptsAreWiredUp is the missing half of
// TestEmbeddedAssets_PanelScriptsAreWiredUp, which walks panels/*.js only.
// app.js and screenview.js are loaded by the same kind of <script src> tag and
// had no check at all: a rename would 404 on every page load, and the live
// screen — the headline feature — would simply never mount.
func TestEmbeddedAssets_TopLevelScriptsAreWiredUp(t *testing.T) {
	html := readAsset(t, "index.html")

	const prefix = `<script src="`
	for rest := html; ; {
		i := strings.Index(rest, prefix)
		if i < 0 {
			break
		}
		rest = rest[i+len(prefix):]
		j := strings.Index(rest, `"`)
		if j < 0 {
			break
		}
		src := rest[:j]
		rest = rest[j:]
		if strings.HasPrefix(src, "panels/") {
			continue // covered by TestEmbeddedAssets_PanelScriptsAreWiredUp
		}
		if strings.Contains(src, "://") {
			continue // not ours to resolve
		}
		if _, err := fs.ReadFile(Assets(), src); err != nil {
			t.Errorf("index.html loads %q but it is not embedded: %v", src, err)
		}
	}

	// And the two that must be there, named explicitly: absence of a script
	// tag is exactly as silent as a broken one.
	for _, want := range []string{`<script src="app.js">`, `<script src="screenview.js">`} {
		if !strings.Contains(html, want) {
			t.Errorf("index.html has no %s", want)
		}
	}
}

// TestEmbeddedAssets_NoOrphanedTopLevelAsset walks the OTHER direction: every
// top-level static/*.js and *.css must actually be referenced by index.html.
//
// The check above, and the panels one, both start from index.html and confirm
// the file it names exists. Neither can see a file that nothing names — and
// that is exactly how the whole animated icon set shipped dead: icons.js and
// icons.css were embedded and served 200, but index.html had no <script> and
// no <link> for either, so every one of the 94 tiles rendered the neutral
// fallback letter while the full suite stayed green.
//
// A deliberately unreferenced asset is fine; it just has to say so here.
func TestEmbeddedAssets_NoOrphanedTopLevelAsset(t *testing.T) {
	html := readAsset(t, "index.html")

	// Assets that exist on purpose without a tag in index.html. Empty today.
	// Add a name here WITH a reason rather than deleting this test.
	intentionallyUnreferenced := map[string]string{}

	// Assets() is already rooted at static/, so "." is that directory.
	entries, err := fs.ReadDir(Assets(), ".")
	if err != nil {
		t.Fatalf("ReadDir(.): %v", err)
	}
	for _, e := range entries {
		if e.IsDir() {
			continue // panels/ and fonts/ have their own contracts
		}
		name := e.Name()
		if !strings.HasSuffix(name, ".js") && !strings.HasSuffix(name, ".css") {
			continue
		}
		if why, ok := intentionallyUnreferenced[name]; ok {
			t.Logf("%s is unreferenced on purpose: %s", name, why)
			continue
		}
		// Quoted so a substring cannot match by accident: "app.js" is a
		// suffix of nothing here, but "icons.css" would match a future
		// "portal-icons.css" and hide the same bug again.
		if !strings.Contains(html, `"`+name+`"`) {
			t.Errorf("static/%s is embedded but index.html never references it — "+
				"it is shipped in the binary, served, and dead. Add the <script> "+
				"or <link>, delete the file, or list it in "+
				"intentionallyUnreferenced with a reason.", name)
		}
	}
}
