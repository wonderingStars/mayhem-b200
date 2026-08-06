// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package server

import (
	"io/fs"
	"net/http"
	"net/http/httptest"
	"path"
	"strings"
	"testing"
	"testing/fstest"
)

// minimalAssets is a tiny synthetic frontend used by handler tests that
// don't care about the real embedded UI, so they don't depend on
// static/index.html's exact contents.
func minimalAssets(t *testing.T) fs.FS {
	t.Helper()
	return fstest.MapFS{
		"index.html": {Data: []byte("<html><body>test shell</body></html>")},
	}
}

// TestEmbeddedAssets_Present makes sure go:embed actually picked up real,
// non-empty files — a typo in the embed pattern would otherwise only ever
// surface as a blank page in a browser.
func TestEmbeddedAssets_Present(t *testing.T) {
	assets := Assets()
	for _, name := range []string{"index.html", "app.js", "app.css"} {
		data, err := fs.ReadFile(assets, name)
		if err != nil {
			t.Fatalf("ReadFile(%q): %v", name, err)
		}
		if len(data) == 0 {
			t.Fatalf("%q is embedded but empty", name)
		}
	}
}

// TestEmbeddedAssets_PanelContractDocumented guards against the JS contract
// comment silently rotting out of app.js — the panel registry is the one
// piece of this UI another agent's code depends on, so its presence (not
// full correctness, that's for a human/JS-level review) is worth a cheap
// automated check.
func TestEmbeddedAssets_PanelContractDocumented(t *testing.T) {
	data, err := fs.ReadFile(Assets(), "app.js")
	if err != nil {
		t.Fatalf("ReadFile(app.js): %v", err)
	}
	js := string(data)
	for _, want := range []string{"MayhemPortal", "registerPanel", "panel_kind"} {
		if !strings.Contains(js, want) {
			t.Fatalf("app.js does not mention %q — panel registry contract may have been removed", want)
		}
	}
}

// TestEmbeddedAssets_PanelScriptsAreWiredUp keeps static/panels/ and
// index.html's <script> tags in agreement, in both directions.
//
// This is a wiring check, not a style check, and it exists because both
// failure modes are silent: a new panels/<kind>.js that nobody added a script
// tag for never calls registerPanel(), so its panel kind quietly falls back to
// the "screen" card in production while every unit test still passes; and a
// script tag left behind for a deleted file is a 404 on every page load that
// no Go test would otherwise notice. registry.js and bridge.js are ordinary
// entries here — they are loaded like any other panel file.
func TestEmbeddedAssets_PanelScriptsAreWiredUp(t *testing.T) {
	assets := Assets()

	index, err := fs.ReadFile(assets, "index.html")
	if err != nil {
		t.Fatalf("ReadFile(index.html): %v", err)
	}
	html := string(index)

	entries, err := fs.ReadDir(assets, "panels")
	if err != nil {
		t.Fatalf("ReadDir(panels): %v", err)
	}

	onDisk := map[string]bool{}
	for _, e := range entries {
		if e.IsDir() || path.Ext(e.Name()) != ".js" {
			continue
		}
		onDisk[e.Name()] = true
		tag := `<script src="panels/` + e.Name() + `">`
		if !strings.Contains(html, tag) {
			t.Errorf("panels/%s is embedded but index.html has no %s — "+
				"its panel kind will never register and will silently fall back to \"screen\"",
				e.Name(), tag)
		}
	}

	// And the reverse: every panels/*.js index.html asks for must exist.
	const prefix = `<script src="panels/`
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
		name := rest[:j]
		if !onDisk[name] {
			t.Errorf("index.html loads panels/%s but no such file is embedded (404 on every page load)", name)
		}
	}

	// A guard on the guard: if the panels directory ever stops being found,
	// both loops above pass vacuously.
	if len(onDisk) == 0 {
		t.Fatal("no panels/*.js files embedded at all — the embed pattern or the directory moved")
	}
}

// TestEmbeddedAssets_AdsbPanelCarriesOSMAttribution pins a licence condition,
// not a style choice. The OSM tile usage policy and the ODbL attribution
// guidelines both require "© OpenStreetMap contributors" to be visible
// whenever the basemap is, and panels/adsb.js is the only place in this repo
// that text exists. There is no JS test infrastructure here, so without this
// a refactor could delete the attribution — putting the project in breach of
// the terms that let it use those servers at all — with every Go test still
// green. The link to the copyright page is part of the same obligation.
func TestEmbeddedAssets_AdsbPanelCarriesOSMAttribution(t *testing.T) {
	data, err := fs.ReadFile(Assets(), "panels/adsb.js")
	if err != nil {
		t.Fatalf("ReadFile(panels/adsb.js): %v", err)
	}
	js := string(data)
	for _, want := range []string{
		"© OpenStreetMap contributors",
		"https://www.openstreetmap.org/copyright",
	} {
		if !strings.Contains(js, want) {
			t.Errorf("panels/adsb.js no longer contains %q — the OSM basemap may not be attributed, "+
				"which is a licence violation, not a cosmetic regression", want)
		}
	}
}

// TestEmbeddedAssets_AllServedWithRightContentType walks every real
// embedded asset and confirms the server answers it with a sane,
// non-empty, extension-appropriate Content-Type — not just the couple of
// files spot-checked elsewhere.
func TestEmbeddedAssets_AllServedWithRightContentType(t *testing.T) {
	assets := Assets()
	s := New(&fakeBackend{})
	s.assets = assets // exercise the real embedded build, not minimalAssets

	err := fs.WalkDir(assets, ".", func(p string, d fs.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		req := httptest.NewRequest(http.MethodGet, "/"+p, nil)
		rr := httptest.NewRecorder()
		s.ServeHTTP(rr, req)
		if rr.Code != http.StatusOK {
			t.Errorf("%s: status = %d, want 200", p, rr.Code)
			return nil
		}
		ct := rr.Header().Get("Content-Type")
		if ct == "" {
			t.Errorf("%s: Content-Type is empty", p)
			return nil
		}
		ext := path.Ext(p)
		if want, ok := staticContentTypes[ext]; ok && ct != want {
			t.Errorf("%s: Content-Type = %q, want %q", p, ct, want)
		}
		return nil
	})
	if err != nil {
		t.Fatalf("WalkDir: %v", err)
	}
}

func TestServeAsset_IndexAtRoot(t *testing.T) {
	s := New(&fakeBackend{}, withAssets(minimalAssets(t)))
	rr := doReq(t, s, http.MethodGet, "/")
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	if ct := rr.Header().Get("Content-Type"); ct != "text/html; charset=utf-8" {
		t.Fatalf("Content-Type = %q, want text/html; charset=utf-8", ct)
	}
}

func TestServeAsset_UnknownPathFallsBackToIndex(t *testing.T) {
	s := New(&fakeBackend{}, withAssets(minimalAssets(t)))
	rr := doReq(t, s, http.MethodGet, "/this/route/does/not/exist")
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (SPA fallback)", rr.Code)
	}
}

func TestServeAsset_MissingIndexIs404(t *testing.T) {
	fsys := fstest.MapFS{"other.txt": {Data: []byte("hi")}}
	s := New(&fakeBackend{}, withAssets(fsys))
	rr := doReq(t, s, http.MethodGet, "/")
	if rr.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404 when even index.html is missing", rr.Code)
	}
}
