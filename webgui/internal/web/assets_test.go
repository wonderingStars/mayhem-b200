// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200. Originally written for sdrlink (MIT) and relicensed
// here by its author/copyright holder into this GPL-2.0-or-later project.

package web

import (
	"io/fs"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"testing/fstest"
)

// TestEmbeddedAssets_Present makes sure the go:embed directive actually
// picked up real, non-empty files — a typo in the embed pattern or a build
// that silently embedded an empty static/ would otherwise only surface as a
// blank page in a browser.
func TestEmbeddedAssets_Present(t *testing.T) {
	assets := Assets()
	for _, name := range []string{"index.html", "app.js", "style.css"} {
		data, err := fs.ReadFile(assets, name)
		if err != nil {
			t.Fatalf("ReadFile(%q): %v", name, err)
		}
		if len(data) == 0 {
			t.Fatalf("%q is embedded but empty", name)
		}
	}
}

func TestServeAsset_IndexAtRoot(t *testing.T) {
	s := NewServer(newFakeBackend())
	rr := doJSON(t, s, http.MethodGet, "/", nil)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	if ct := rr.Header().Get("Content-Type"); ct != "text/html; charset=utf-8" {
		t.Fatalf("Content-Type = %q, want text/html; charset=utf-8", ct)
	}
	if !strings.Contains(rr.Body.String(), "sdrlink") {
		t.Fatalf("index.html body does not mention sdrlink")
	}
}

func TestServeAsset_AppJS(t *testing.T) {
	s := NewServer(newFakeBackend())
	rr := doJSON(t, s, http.MethodGet, "/app.js", nil)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	if ct := rr.Header().Get("Content-Type"); ct != "text/javascript; charset=utf-8" {
		t.Fatalf("Content-Type = %q, want text/javascript; charset=utf-8", ct)
	}
	if !strings.Contains(rr.Body.String(), "WebSocket") {
		t.Fatalf("app.js does not reference WebSocket")
	}
}

func TestServeAsset_StyleCSS(t *testing.T) {
	s := NewServer(newFakeBackend())
	rr := doJSON(t, s, http.MethodGet, "/style.css", nil)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	if ct := rr.Header().Get("Content-Type"); ct != "text/css; charset=utf-8" {
		t.Fatalf("Content-Type = %q, want text/css; charset=utf-8", ct)
	}
}

func TestServeAsset_UnknownPathFallsBackToIndex(t *testing.T) {
	s := NewServer(newFakeBackend())
	rr := doJSON(t, s, http.MethodGet, "/this/route/does/not/exist", nil)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (SPA fallback)", rr.Code)
	}
	if ct := rr.Header().Get("Content-Type"); ct != "text/html; charset=utf-8" {
		t.Fatalf("Content-Type = %q, want text/html; charset=utf-8", ct)
	}
}

// TestServeAsset_ContentTypesByExtension exercises serveAsset's content-type
// logic in isolation against a synthetic filesystem, independent of what
// static/ actually contains, including the sniffing fallback for an
// extension with no pinned mapping.
func TestServeAsset_ContentTypesByExtension(t *testing.T) {
	fsys := fstest.MapFS{
		"index.html": {Data: []byte("<html></html>")},
		"a.css":      {Data: []byte("body{}")},
		"a.js":       {Data: []byte("console.log(1)")},
		"a.svg":      {Data: []byte("<svg></svg>")},
		// PNG magic bytes so http.DetectContentType has something concrete
		// to sniff for the "no pinned mapping" fallback path.
		"a.unknownext": {Data: []byte{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}},
	}
	s := NewServer(newFakeBackend(), withAssets(fsys))

	cases := []struct {
		path      string
		wantCT    string
		exactMime bool
	}{
		{"/a.css", "text/css; charset=utf-8", true},
		{"/a.js", "text/javascript; charset=utf-8", true},
		{"/a.svg", "image/svg+xml", true},
	}
	for _, c := range cases {
		rr := doJSON(t, s, http.MethodGet, c.path, nil)
		if rr.Code != http.StatusOK {
			t.Fatalf("%s: status = %d, want 200", c.path, rr.Code)
		}
		if got := rr.Header().Get("Content-Type"); got != c.wantCT {
			t.Fatalf("%s: Content-Type = %q, want %q", c.path, got, c.wantCT)
		}
	}

	// Unpinned extension still gets SOME sane, non-empty content type via
	// sniffing rather than an empty header.
	rr := doJSON(t, s, http.MethodGet, "/a.unknownext", nil)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	if ct := rr.Header().Get("Content-Type"); ct == "" {
		t.Fatalf("Content-Type is empty for an unmapped extension, want a sniffed value")
	}
}

func TestServeAsset_MissingIndexIs404(t *testing.T) {
	fsys := fstest.MapFS{"other.txt": {Data: []byte("hi")}}
	s := NewServer(newFakeBackend(), withAssets(fsys))
	rr := doJSON(t, s, http.MethodGet, "/", nil)
	if rr.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404 when even index.html is missing", rr.Code)
	}
}

// Sanity check that the embedded assets are wired through a real
// net/http.Server too, not just Server.ServeHTTP called directly.
func TestServeAsset_RealHTTPServer(t *testing.T) {
	s := NewServer(newFakeBackend())
	ts := httptest.NewServer(s)
	defer ts.Close()

	res, err := http.Get(ts.URL + "/")
	if err != nil {
		t.Fatalf("GET /: %v", err)
	}
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200", res.StatusCode)
	}
}
