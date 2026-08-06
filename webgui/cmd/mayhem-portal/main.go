// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.
//
// Command mayhem-portal serves the browser app portal: a grid of all of
// mayhem-b200's ~103 apps, grouped and searchable, that launches any of
// them and renders their live structured data. It is a JSON proxy in front
// of mayhem-b200's own app-portal HTTP API (default
// http://127.0.0.1:8090 — see doc/REMOTE-UI.md's neighbouring, separate
// framebuffer-mirror protocol for context) plus the embedded UI in
// internal/portal/server/static.
//
// This is a separate binary from mayhem-webgui (the existing sdrlink radio
// GUI) because it drives a completely different backend: mayhem-webgui
// speaks the sdrlink TCP control protocol to drive one open SDR device;
// mayhem-portal speaks HTTP/JSON to mayhem-b200's own process to drive its
// app registry and navigation stack. Neither needs the other's backend, so
// there is nothing to gain by merging them into one process — run both,
// on different -http ports, side by side against the same running
// mayhem-b200 instance.
package main

import (
	"context"
	"flag"
	"log"
	"net"
	"net/http"
	"os"
	"os/signal"
	"time"

	"mayhemb200/webgui/internal/portal/client"
	"mayhemb200/webgui/internal/portal/server"
	"mayhemb200/webgui/internal/portal/tiles"
)

func main() {
	backendAddr := flag.String("backend", "http://127.0.0.1:8090", "base URL of mayhem-b200's app-portal HTTP API")
	httpAddr := flag.String("http", ":8081", "address this portal's own HTTP server listens on")
	backendTimeout := flag.Duration("backend-timeout", client.DefaultTimeout, "timeout for each request to the app-portal API")
	tilesSpec := flag.String("tiles", tiles.DefaultURLTemplate, `basemap tile URL template with {z}/{x}/{y} placeholders, or "off" to disable the basemap`)
	tilesCache := flag.String("tiles-cache", "", "directory for the on-disk basemap tile cache (default: <user cache dir>/mayhem-b200/tiles)")
	tilesCacheBytes := flag.Int64("tiles-cache-size", tiles.DefaultMaxCacheBytes, "size budget in bytes for the on-disk basemap tile cache")
	flag.Parse()

	logger := log.New(os.Stderr, "mayhem-portal ", log.LstdFlags)

	// A misconfigured basemap must not stop the portal from starting: the
	// ADS-B panel falls back to its graticule, which is what it drew before
	// tiles existed at all.
	tileProxy, err := tiles.FromFlag(*tilesSpec, *tilesCache, *tilesCacheBytes, logger.Printf)
	if err != nil {
		logger.Printf("basemap tiles disabled: %v", err)
		tileProxy = tiles.NewDisabled("basemap tiles are misconfigured: " + err.Error())
	}
	if tileProxy.Enabled() {
		logger.Printf("basemap tiles from %s (cache: %s)", tileProxy.URLTemplate(), tileProxy.CacheDir())
	} else {
		logger.Print("basemap tiles disabled")
	}

	c := client.New(*backendAddr, *backendTimeout)
	handler := server.New(c, server.WithTiles(tileProxy))

	httpSrv := &http.Server{Addr: *httpAddr, Handler: handler}

	httpErrCh := make(chan error, 1)
	go func() {
		if err := httpSrv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			httpErrCh <- err
		}
	}()
	logger.Printf("app portal on http://%s/ (backend: %s)", displayAddr(*httpAddr), *backendAddr)

	// Surface a bind failure (e.g. port already in use) promptly rather
	// than only discovering it when a browser fails to connect.
	select {
	case err := <-httpErrCh:
		logger.Fatalf("app portal: %v", err)
	case <-time.After(200 * time.Millisecond):
	}

	sig := make(chan os.Signal, 1)
	signal.Notify(sig, os.Interrupt)
	<-sig
	logger.Print("shutting down")

	shutdownCtx, shutdownCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer shutdownCancel()
	if err := httpSrv.Shutdown(shutdownCtx); err != nil {
		logger.Printf("app portal shutdown: %v", err)
	}
}

// displayAddr turns a listen address like ":8081" into something a user can
// paste into a browser, e.g. "127.0.0.1:8081".
func displayAddr(addr string) string {
	host, port, err := net.SplitHostPort(addr)
	if err != nil {
		return addr
	}
	if host == "" {
		host = "127.0.0.1"
	}
	return net.JoinHostPort(host, port)
}
