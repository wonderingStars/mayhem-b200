// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

// This file is the wiring test: it lives in the external test package so it
// can import internal/portal/server (which imports this package) without an
// import cycle. What it pins down is that the endpoint contract holds
// through the real portal mux — including the case that would otherwise be
// silent, where an unmounted route falls through to the SPA catch-all and
// answers a tile request with index.html and a 200.
package tiles_test

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"mayhemb200/webgui/internal/portal/client"
	"mayhemb200/webgui/internal/portal/server"
	"mayhemb200/webgui/internal/portal/tiles"
)

// stubBackend is the minimum server.Backend: these tests never touch an
// /api/apps route.
type stubBackend struct{}

func (stubBackend) Apps(context.Context) ([]client.App, error) { return nil, nil }
func (stubBackend) CurrentApp(context.Context) (client.CurrentApp, error) {
	return client.CurrentApp{}, nil
}
func (stubBackend) Launch(context.Context, string) (client.CurrentApp, error) {
	return client.CurrentApp{}, nil
}
func (stubBackend) Home(context.Context) (client.CurrentApp, error)     { return client.CurrentApp{}, nil }
func (stubBackend) Panel(context.Context, string) (client.Panel, error) { return client.Panel{}, nil }
func (stubBackend) Status(context.Context) (client.Status, error)       { return client.Status{}, nil }
func (stubBackend) MorseTransmit(context.Context, string, int) (client.MorseTransmitResult, error) {
	return client.MorseTransmitResult{}, nil
}

// The framebuffer mirror (server.Backend's Screen/Input, contracts 1 and 2)
// is not exercised by the tile route tests: this stub answers "no frame yet"
// and accepts no input, which is exactly what a backend with nothing drawn
// does.
func (stubBackend) Screen(context.Context, uint32, int) (client.ScreenFrame, bool, error) {
	return client.ScreenFrame{}, false, nil
}
func (stubBackend) Input(context.Context, []json.RawMessage) (client.InputResult, error) {
	return client.InputResult{}, nil
}

// stubTransport answers every upstream request with the same tile.
type stubTransport struct {
	body  []byte
	calls int
}

func (s *stubTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	s.calls++
	return &http.Response{
		StatusCode: http.StatusOK,
		Header:     http.Header{"Content-Type": {"image/png"}},
		Body:       io.NopCloser(bytes.NewReader(s.body)),
		Request:    req,
	}, nil
}

func TestPortalServesTilesOnTheContractRoute(t *testing.T) {
	body := []byte{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a, 0x99}
	tr := &stubTransport{body: body}
	proxy, err := tiles.New(tiles.Config{CacheDir: t.TempDir(), Transport: tr})
	if err != nil {
		t.Fatalf("tiles.New: %v", err)
	}

	ts := httptest.NewServer(server.New(stubBackend{}, server.WithTiles(proxy)))
	defer ts.Close()

	resp, err := http.Get(ts.URL + "/api/tiles/13/4033/2589.png")
	if err != nil {
		t.Fatalf("GET tile: %v", err)
	}
	defer resp.Body.Close()
	got, _ := io.ReadAll(resp.Body)

	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200 (body %q)", resp.StatusCode, got)
	}
	if ct := resp.Header.Get("Content-Type"); ct != "image/png" {
		t.Errorf("Content-Type = %q, want image/png", ct)
	}
	if cc := resp.Header.Get("Cache-Control"); cc != "public, max-age=604800" {
		t.Errorf("Cache-Control = %q, want the contract's 7-day value", cc)
	}
	if xc := resp.Header.Get("X-Tile-Cache"); xc != "miss" {
		t.Errorf("X-Tile-Cache = %q, want miss", xc)
	}
	if !bytes.Equal(got, body) {
		t.Errorf("body = %v, want %v", got, body)
	}

	// Malformed coordinates are still rejected through the mux.
	bad, err := http.Get(ts.URL + "/api/tiles/20/0/0.png")
	if err != nil {
		t.Fatalf("GET out-of-range tile: %v", err)
	}
	bad.Body.Close()
	if bad.StatusCode != http.StatusBadRequest {
		t.Errorf("out-of-range tile status = %d, want 400", bad.StatusCode)
	}
	if tr.calls != 1 {
		t.Errorf("upstream requests = %d, want 1", tr.calls)
	}
}

// TestPortalWithoutTilesOptionAnswersTheContract503 is the regression this
// route exists to prevent: with no tiles configured the panel must get the
// documented 503 JSON, never a 200 with the SPA's index.html in it.
func TestPortalWithoutTilesOptionAnswersTheContract503(t *testing.T) {
	ts := httptest.NewServer(server.New(stubBackend{}))
	defer ts.Close()

	resp, err := http.Get(ts.URL + "/api/tiles/13/4033/2589.png")
	if err != nil {
		t.Fatalf("GET tile: %v", err)
	}
	defer resp.Body.Close()
	raw, _ := io.ReadAll(resp.Body)

	if resp.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503 (body %q)", resp.StatusCode, raw)
	}
	if ct := resp.Header.Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		t.Fatalf("Content-Type = %q, want application/json", ct)
	}
	var body struct {
		Error   string `json:"error"`
		Message string `json:"message"`
	}
	if err := json.Unmarshal(raw, &body); err != nil {
		t.Fatalf("body %q is not JSON: %v", raw, err)
	}
	if body.Error != "tiles_unavailable" {
		t.Errorf("error = %q, want %q", body.Error, "tiles_unavailable")
	}
	if body.Message == "" {
		t.Error("message is empty")
	}
}

// TestRouteConstantMatchesTheContract keeps the one string both mount points
// and the panel's fetch() agree on from drifting.
func TestRouteConstantMatchesTheContract(t *testing.T) {
	if tiles.PathPrefix != "/api/tiles/" {
		t.Fatalf("PathPrefix = %q, want %q", tiles.PathPrefix, "/api/tiles/")
	}
	if tiles.Route != "GET /api/tiles/" {
		t.Fatalf("Route = %q, want %q", tiles.Route, "GET /api/tiles/")
	}
}
