// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.
//
// Command panels-harness is a dev-only static file server for
// internal/portal/harness.html, so the internal/portal/server/static/panels
// renderers can be exercised against internal/portal/testdata fixtures in a
// real browser -- fetch() of a file:// page is blocked by browser CORS
// rules, so "open harness.html directly" doesn't work, and this is the
// difference between "the panels demonstrably work" and "trust me". See
// internal/portal/PANELS.md.
//
// It serves internal/portal straight off disk (no go:embed, no build step)
// and implements almost none of internal/portal/server's real API
// (/api/apps, /api/panel, ...) -- it is not that server, only a way to load
// its static assets and fixtures over HTTP for manual/visual verification.
// The one exception is the basemap tile proxy (internal/portal/tiles),
// mounted on exactly the same route the real server uses: the ADS-B panel's
// map is not meaningfully verifiable without it, since checking that
// aircraft land on the right piece of coastline is the entire point of the
// Web Mercator projection it draws in.
package main

import (
	"flag"
	"fmt"
	"log"
	"net/http"
	"os"
	"path/filepath"

	"mayhemb200/webgui/internal/portal/tiles"
)

func main() {
	addr := flag.String("addr", "127.0.0.1:8091", "address to serve internal/portal on")
	dir := flag.String("dir", "", "internal/portal directory to serve (default: resolved relative to this source file's module)")
	tilesSpec := flag.String("tiles", tiles.DefaultURLTemplate, `basemap tile URL template with {z}/{x}/{y} placeholders, or "off" to disable the basemap`)
	tilesCache := flag.String("tiles-cache", "", "directory for the on-disk basemap tile cache (default: <user cache dir>/mayhem-b200/tiles)")
	tilesCacheBytes := flag.Int64("tiles-cache-size", tiles.DefaultMaxCacheBytes, "size budget in bytes for the on-disk basemap tile cache")
	flag.Parse()

	root := *dir
	if root == "" {
		var err error
		root, err = defaultPortalDir()
		if err != nil {
			log.Fatalf("panels-harness: %v", err)
		}
	}

	// As in cmd/mayhem-portal: a basemap that cannot be configured is a
	// degraded map, not a reason to refuse to serve the harness.
	tileProxy, err := tiles.FromFlag(*tilesSpec, *tilesCache, *tilesCacheBytes, log.Printf)
	if err != nil {
		log.Printf("panels-harness: basemap tiles disabled: %v", err)
		tileProxy = tiles.NewDisabled("basemap tiles are misconfigured: " + err.Error())
	}

	mux, err := newMuxWithTiles(root, tileProxy)
	if err != nil {
		log.Fatalf("panels-harness: %v", err)
	}

	log.Printf("panels-harness: serving %s on http://%s/harness.html", root, *addr)
	if tileProxy.Enabled() {
		log.Printf("panels-harness: basemap tiles from %s (cache: %s)", tileProxy.URLTemplate(), tileProxy.CacheDir())
	}
	if err := http.ListenAndServe(*addr, mux); err != nil {
		log.Fatalf("panels-harness: %v", err)
	}
}

// newMux builds the handler that serves root (internal/portal) as static
// files, with the basemap disabled. Split out from main so tests can drive
// it with httptest without binding a real listener.
func newMux(root string) (http.Handler, error) {
	return newMuxWithTiles(root, tiles.NewDisabled("basemap tiles are disabled in this harness"))
}

// newMuxWithTiles is newMux plus the basemap tile proxy, on the same route
// the real portal server mounts it on (tiles.Route), so harness.html and
// the shipped UI hit an identical endpoint.
func newMuxWithTiles(root string, tileProxy http.Handler) (http.Handler, error) {
	if _, err := os.Stat(filepath.Join(root, "harness.html")); err != nil {
		return nil, fmt.Errorf("%s does not look like internal/portal (no harness.html found): %w", root, err)
	}
	mux := http.NewServeMux()
	mux.Handle(tiles.Route, tileProxy)
	mux.Handle("/", http.FileServer(http.Dir(root)))
	return mux, nil
}

// defaultPortalDir locates internal/portal relative to the webgui module
// root by walking up from the current working directory looking for
// go.mod, so `go run ./cmd/panels-harness` works whether it's invoked from
// the webgui/ directory or from anywhere inside it.
func defaultPortalDir() (string, error) {
	wd, err := os.Getwd()
	if err != nil {
		return "", err
	}
	dir := wd
	for {
		if _, err := os.Stat(filepath.Join(dir, "go.mod")); err == nil {
			return filepath.Join(dir, "internal", "portal"), nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", os.ErrNotExist
		}
		dir = parent
	}
}
