// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package tiles

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

// --- test doubles --------------------------------------------------------

// testClock is the injectable clock, so freshness windows can be crossed
// without a test ever sleeping.
type testClock struct {
	mu sync.Mutex
	t  time.Time
}

func newTestClock() *testClock {
	return &testClock{t: time.Date(2024, 6, 1, 12, 0, 0, 0, time.UTC)}
}

func (c *testClock) now() time.Time {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.t
}

func (c *testClock) advance(d time.Duration) {
	c.mu.Lock()
	defer c.mu.Unlock()
	c.t = c.t.Add(d)
}

// fakePNG returns bytes that satisfy the PNG signature check, tagged so a
// test can tell one tile's bytes from another's.
func fakePNG(tag byte) []byte {
	out := make([]byte, 0, len(pngSignature)+4)
	out = append(out, pngSignature...)
	return append(out, tag, 0x0d, 0x0a, tag)
}

// recordedRequest is what a fakeTransport saw, captured so tests can assert
// on the exact User-Agent and conditional headers sent upstream.
type recordedRequest struct {
	url    string
	header http.Header
}

// fakeTransport stands in for the network. Every test in this package uses
// one: nothing here ever reaches tile.openstreetmap.org, which is both a
// test-hygiene point and a tile-policy one.
type fakeTransport struct {
	respond func(req *http.Request, n int) (*http.Response, error)

	mu       sync.Mutex
	requests []recordedRequest
}

func (f *fakeTransport) RoundTrip(req *http.Request) (*http.Response, error) {
	f.mu.Lock()
	f.requests = append(f.requests, recordedRequest{url: req.URL.String(), header: req.Header.Clone()})
	n := len(f.requests)
	f.mu.Unlock()
	return f.respond(req, n)
}

func (f *fakeTransport) count() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return len(f.requests)
}

func (f *fakeTransport) request(i int) recordedRequest {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.requests[i]
}

// pngResponse builds a 200 with the given body and headers.
func pngResponse(req *http.Request, body []byte, header http.Header) *http.Response {
	if header == nil {
		header = http.Header{}
	}
	header.Set("Content-Type", "image/png")
	return &http.Response{
		StatusCode:    http.StatusOK,
		Header:        header,
		Body:          io.NopCloser(bytes.NewReader(body)),
		ContentLength: int64(len(body)),
		Request:       req,
	}
}

func statusResponse(req *http.Request, status int, header http.Header) *http.Response {
	if header == nil {
		header = http.Header{}
	}
	return &http.Response{
		StatusCode: status,
		Header:     header,
		Body:       io.NopCloser(strings.NewReader("")),
		Request:    req,
	}
}

// newTestProxy builds an enabled Proxy wired to ft and clk, with a cache in
// a directory that vanishes with the test.
func newTestProxy(t *testing.T, ft *fakeTransport, clk *testClock, cfg Config) *Proxy {
	t.Helper()
	if cfg.CacheDir == "" {
		cfg.CacheDir = t.TempDir()
	}
	cfg.Transport = ft
	cfg.Now = clk.now
	p, err := New(cfg)
	if err != nil {
		t.Fatalf("New: %v", err)
	}
	return p
}

// serve drives one request through the handler.
func serve(p *Proxy, path string) *httptest.ResponseRecorder {
	rec := httptest.NewRecorder()
	p.ServeHTTP(rec, httptest.NewRequest(http.MethodGet, path, nil))
	return rec
}

// --- serving and caching -------------------------------------------------

func TestServeTileMissThenHit(t *testing.T) {
	body := fakePNG(0xA1)
	ft := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
		return pngResponse(req, body, nil), nil
	}}
	p := newTestProxy(t, ft, newTestClock(), Config{})

	rec := serve(p, "/api/tiles/13/4033/2589.png")
	if rec.Code != http.StatusOK {
		t.Fatalf("first request status = %d, want 200 (body %q)", rec.Code, rec.Body.String())
	}
	if got := rec.Header().Get("X-Tile-Cache"); got != "miss" {
		t.Errorf("first request X-Tile-Cache = %q, want %q", got, "miss")
	}
	if got := rec.Header().Get("Content-Type"); got != "image/png" {
		t.Errorf("Content-Type = %q, want image/png", got)
	}
	if got := rec.Header().Get("Cache-Control"); got != "public, max-age=604800" {
		t.Errorf("Cache-Control = %q, want the contract's 7-day value", got)
	}
	if !bytes.Equal(rec.Body.Bytes(), body) {
		t.Errorf("first request body = %v, want %v", rec.Body.Bytes(), body)
	}

	rec = serve(p, "/api/tiles/13/4033/2589.png")
	if rec.Code != http.StatusOK {
		t.Fatalf("second request status = %d, want 200", rec.Code)
	}
	if got := rec.Header().Get("X-Tile-Cache"); got != "hit" {
		t.Errorf("second request X-Tile-Cache = %q, want %q", got, "hit")
	}
	if !bytes.Equal(rec.Body.Bytes(), body) {
		t.Errorf("second request body = %v, want the cached %v", rec.Body.Bytes(), body)
	}
	if got := ft.count(); got != 1 {
		t.Fatalf("upstream requests = %d, want 1: a cache hit must not reach upstream", got)
	}
}

// TestServeOnlyFetchesTheRequestedTile is the demand-driven assertion the
// tile policy's ban on pre-seeding turns on: one displayed tile, one
// upstream request, for exactly that URL and no neighbour of it.
func TestServeOnlyFetchesTheRequestedTile(t *testing.T) {
	ft := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
		return pngResponse(req, fakePNG(0xA2), nil), nil
	}}
	p := newTestProxy(t, ft, newTestClock(), Config{})

	if rec := serve(p, "/api/tiles/7/63/42.png"); rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rec.Code)
	}
	if got := ft.count(); got != 1 {
		t.Fatalf("upstream requests = %d, want exactly 1", got)
	}
	want := "https://tile.openstreetmap.org/7/63/42.png"
	if got := ft.request(0).url; got != want {
		t.Fatalf("upstream URL = %q, want %q", got, want)
	}
}

// TestUpstreamRequestFollowsTilePolicy checks the headers the OSM tile
// usage policy is explicit about: a User-Agent naming the app with a
// contact URL, and never a no-cache request.
func TestUpstreamRequestFollowsTilePolicy(t *testing.T) {
	ft := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
		return pngResponse(req, fakePNG(0xA3), nil), nil
	}}
	p := newTestProxy(t, ft, newTestClock(), Config{})
	serve(p, "/api/tiles/3/2/1.png")

	if ft.count() != 1 {
		t.Fatalf("upstream requests = %d, want 1", ft.count())
	}
	got := ft.request(0).header

	const want = "mayhem-b200/0.9.0 (+https://github.com/wonderingStars/mayhem-b200)"
	if UserAgent != want {
		t.Fatalf("UserAgent constant = %q, want %q", UserAgent, want)
	}
	if got := got.Get("User-Agent"); got != want {
		t.Errorf("upstream User-Agent = %q, want exactly %q", got, want)
	}
	if cc := got.Get("Cache-Control"); strings.Contains(strings.ToLower(cc), "no-cache") {
		t.Errorf("upstream Cache-Control = %q, must never ask for no-cache", cc)
	}
	if pragma := got.Get("Pragma"); pragma != "" {
		t.Errorf("upstream Pragma = %q, want none", pragma)
	}
	// Nothing to revalidate on a first fetch.
	if v := got.Get("If-None-Match"); v != "" {
		t.Errorf("first fetch sent If-None-Match %q", v)
	}
}

// TestRevalidationServesCachedBytesOn304 covers the conditional-request
// half of the policy: a stale entry is re-validated, not re-downloaded.
func TestRevalidationServesCachedBytesOn304(t *testing.T) {
	body := fakePNG(0xB1)
	clk := newTestClock()
	ft := &fakeTransport{respond: func(req *http.Request, n int) (*http.Response, error) {
		switch n {
		case 1:
			h := http.Header{}
			h.Set("ETag", `"tile-v1"`)
			h.Set("Last-Modified", "Sat, 01 Jun 2024 10:00:00 GMT")
			h.Set("Cache-Control", "max-age=60")
			return pngResponse(req, body, h), nil
		default:
			if got := req.Header.Get("If-None-Match"); got != `"tile-v1"` {
				return nil, fmt.Errorf("revalidation sent If-None-Match %q, want %q", got, `"tile-v1"`)
			}
			if got := req.Header.Get("If-Modified-Since"); got != "Sat, 01 Jun 2024 10:00:00 GMT" {
				return nil, fmt.Errorf("revalidation sent If-Modified-Since %q", got)
			}
			h := http.Header{}
			h.Set("Cache-Control", "max-age=3600")
			return statusResponse(req, http.StatusNotModified, h), nil
		}
	}}
	p := newTestProxy(t, ft, clk, Config{})

	if rec := serve(p, "/api/tiles/5/9/9.png"); rec.Header().Get("X-Tile-Cache") != "miss" {
		t.Fatalf("first request X-Tile-Cache = %q, want miss", rec.Header().Get("X-Tile-Cache"))
	}

	// Past the max-age the upstream asked for.
	clk.advance(61 * time.Second)
	rec := serve(p, "/api/tiles/5/9/9.png")
	if rec.Code != http.StatusOK {
		t.Fatalf("revalidated request status = %d, want 200 (body %q)", rec.Code, rec.Body.String())
	}
	if got := rec.Header().Get("X-Tile-Cache"); got != "revalidated" {
		t.Errorf("X-Tile-Cache = %q, want %q", got, "revalidated")
	}
	if !bytes.Equal(rec.Body.Bytes(), body) {
		t.Errorf("revalidated body = %v, want the cached %v", rec.Body.Bytes(), body)
	}
	if got := ft.count(); got != 2 {
		t.Fatalf("upstream requests = %d, want 2", got)
	}

	// The 304's own max-age must re-arm the freshness window, or every
	// later request would revalidate again.
	clk.advance(time.Minute)
	rec = serve(p, "/api/tiles/5/9/9.png")
	if got := rec.Header().Get("X-Tile-Cache"); got != "hit" {
		t.Errorf("after revalidation X-Tile-Cache = %q, want hit", got)
	}
	if got := ft.count(); got != 2 {
		t.Errorf("upstream requests = %d, want 2: the 304 should have re-armed freshness", got)
	}
}

// TestRevalidationReplacesChangedTile is the other branch: upstream says
// the tile really did change, so the new bytes replace the cached ones.
func TestRevalidationReplacesChangedTile(t *testing.T) {
	oldBody, newBody := fakePNG(0xC1), fakePNG(0xC2)
	clk := newTestClock()
	ft := &fakeTransport{respond: func(req *http.Request, n int) (*http.Response, error) {
		h := http.Header{}
		h.Set("Cache-Control", "max-age=60")
		if n == 1 {
			h.Set("ETag", `"v1"`)
			return pngResponse(req, oldBody, h), nil
		}
		h.Set("ETag", `"v2"`)
		return pngResponse(req, newBody, h), nil
	}}
	p := newTestProxy(t, ft, clk, Config{})

	serve(p, "/api/tiles/6/33/22.png")
	clk.advance(2 * time.Minute)

	rec := serve(p, "/api/tiles/6/33/22.png")
	if got := rec.Header().Get("X-Tile-Cache"); got != "miss" {
		t.Errorf("X-Tile-Cache = %q, want miss for replaced bytes", got)
	}
	if !bytes.Equal(rec.Body.Bytes(), newBody) {
		t.Errorf("body = %v, want the replacement %v", rec.Body.Bytes(), newBody)
	}
	// And the replacement is what is cached from now on.
	rec = serve(p, "/api/tiles/6/33/22.png")
	if !bytes.Equal(rec.Body.Bytes(), newBody) {
		t.Errorf("cached body = %v, want %v", rec.Body.Bytes(), newBody)
	}
}

// --- failure modes -------------------------------------------------------

func TestUpstreamFailureServesStaleTile(t *testing.T) {
	body := fakePNG(0xD1)
	clk := newTestClock()
	ft := &fakeTransport{respond: func(req *http.Request, n int) (*http.Response, error) {
		if n == 1 {
			h := http.Header{}
			h.Set("Cache-Control", "max-age=60")
			return pngResponse(req, body, h), nil
		}
		return nil, errors.New("dial tcp: no route to host")
	}}
	p := newTestProxy(t, ft, clk, Config{})

	serve(p, "/api/tiles/8/128/85.png")
	clk.advance(time.Hour)

	rec := serve(p, "/api/tiles/8/128/85.png")
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200: a stale tile beats a hole in the map", rec.Code)
	}
	if !bytes.Equal(rec.Body.Bytes(), body) {
		t.Errorf("body = %v, want the stale cached %v", rec.Body.Bytes(), body)
	}
	if got := rec.Header().Get("X-Tile-Stale"); got != "1" {
		t.Errorf("X-Tile-Stale = %q, want %q", got, "1")
	}
}

func TestUpstreamFailureWithoutCacheReturns503(t *testing.T) {
	ft := &fakeTransport{respond: func(*http.Request, int) (*http.Response, error) {
		return nil, errors.New("dial tcp: connection refused")
	}}
	p := newTestProxy(t, ft, newTestClock(), Config{})

	rec := serve(p, "/api/tiles/8/128/85.png")
	assertUnavailable(t, rec)
}

func TestUpstreamErrorStatusReturns503(t *testing.T) {
	for _, status := range []int{http.StatusForbidden, http.StatusTooManyRequests, http.StatusInternalServerError, http.StatusNotFound} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			ft := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
				return statusResponse(req, status, nil), nil
			}}
			p := newTestProxy(t, ft, newTestClock(), Config{})
			assertUnavailable(t, serve(p, "/api/tiles/4/8/5.png"))
		})
	}
}

// TestBlockedUpstreamIsLeftAlone is the policy's "you may be blocked without
// notice" clause with teeth: after a 429 (or a 403), nothing else may reach
// the tile server until the back-off expires. Without it every tile a pan
// uncovers is another request into a rate limit, which is how a temporary
// block becomes a permanent one — and the panel cannot save us, because it
// only gives up when NOTHING has ever loaded.
func TestBlockedUpstreamIsLeftAlone(t *testing.T) {
	for _, status := range []int{http.StatusTooManyRequests, http.StatusForbidden} {
		t.Run(fmt.Sprint(status), func(t *testing.T) {
			clk := newTestClock()
			ft := &fakeTransport{respond: func(req *http.Request, n int) (*http.Response, error) {
				if n == 1 {
					return statusResponse(req, status, nil), nil
				}
				return pngResponse(req, fakePNG(0x7A), nil), nil
			}}
			p := newTestProxy(t, ft, clk, Config{})

			assertUnavailable(t, serve(p, "/api/tiles/9/255/170.png"))
			if got := ft.count(); got != 1 {
				t.Fatalf("upstream requests = %d, want 1", got)
			}

			// A whole screenful of other tiles, all while blocked.
			for i := 0; i < 12; i++ {
				assertUnavailable(t, serve(p, fmt.Sprintf("/api/tiles/9/255/%d.png", 200+i)))
			}
			if got := ft.count(); got != 1 {
				t.Fatalf("upstream requests = %d after 12 more tiles, want 1: a blocked "+
					"tile server must be left alone, not asked again for every square", got)
			}

			// Once the back-off is over, one request is allowed through again
			// and a success clears the block entirely.
			clk.advance(blockBackoff + time.Second)
			if rec := serve(p, "/api/tiles/9/255/171.png"); rec.Code != http.StatusOK {
				t.Fatalf("after the back-off: status = %d, want 200 (body %q)", rec.Code, rec.Body.String())
			}
			if rec := serve(p, "/api/tiles/9/255/172.png"); rec.Code != http.StatusOK {
				t.Fatalf("after a success: status = %d, want 200", rec.Code)
			}
			if got := ft.count(); got != 3 {
				t.Fatalf("upstream requests = %d, want 3", got)
			}
		})
	}
}

// TestRepeatedBlocksBackOffFurther: resuming full-rate requests the instant a
// limit expires is what earns a permanent block, so each refusal doubles the
// window.
func TestRepeatedBlocksBackOffFurther(t *testing.T) {
	clk := newTestClock()
	ft := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
		return statusResponse(req, http.StatusTooManyRequests, nil), nil
	}}
	p := newTestProxy(t, ft, clk, Config{})

	assertUnavailable(t, serve(p, "/api/tiles/9/255/170.png")) // first 429
	clk.advance(blockBackoff + time.Second)
	assertUnavailable(t, serve(p, "/api/tiles/9/255/171.png")) // retried, 429 again
	if got := ft.count(); got != 2 {
		t.Fatalf("upstream requests = %d, want 2", got)
	}

	// The first window has already been served; a second one of the same
	// length would let this through.
	clk.advance(blockBackoff + time.Second)
	assertUnavailable(t, serve(p, "/api/tiles/9/255/172.png"))
	if got := ft.count(); got != 2 {
		t.Errorf("upstream requests = %d, want 2: the second block should last longer than the first", got)
	}

	clk.advance(2 * blockBackoff)
	assertUnavailable(t, serve(p, "/api/tiles/9/255/173.png"))
	if got := ft.count(); got != 3 {
		t.Errorf("upstream requests = %d, want 3: the doubled window has expired", got)
	}
}

// TestBlockedUpstreamStillServesCachedTiles: backing off must not blank the
// map. Anything already on disk is still served (stale if need be), exactly
// as when the machine is offline.
func TestBlockedUpstreamStillServesCachedTiles(t *testing.T) {
	body := fakePNG(0x7B)
	clk := newTestClock()
	ft := &fakeTransport{respond: func(req *http.Request, n int) (*http.Response, error) {
		if n == 1 {
			h := http.Header{}
			h.Set("Cache-Control", "max-age=60")
			return pngResponse(req, body, h), nil
		}
		return statusResponse(req, http.StatusTooManyRequests, nil), nil
	}}
	p := newTestProxy(t, ft, clk, Config{})

	serve(p, "/api/tiles/9/255/170.png") // cached
	clk.advance(2 * time.Minute)         // now stale, so it revalidates -> 429
	assertUnavailable(t, serve(p, "/api/tiles/9/255/171.png"))

	rec := serve(p, "/api/tiles/9/255/170.png")
	if rec.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200: a cached tile is still good while blocked", rec.Code)
	}
	if !bytes.Equal(rec.Body.Bytes(), body) {
		t.Errorf("body = %v, want the cached %v", rec.Body.Bytes(), body)
	}
	if got := rec.Header().Get("X-Tile-Stale"); got != "1" {
		t.Errorf("X-Tile-Stale = %q, want %q", got, "1")
	}
	if got := ft.count(); got != 2 {
		t.Errorf("upstream requests = %d, want 2: serving the cached tile must not have dialled", got)
	}
}

// TestUpstreamConcurrencyIsCapped guards maxUpstreamConcurrency, which is the
// "do not put heavy load on the servers" half of the policy. Counting
// requests (as the coalescing tests do) cannot see it: deleting the semaphore
// would leave every other test green.
func TestUpstreamConcurrencyIsCapped(t *testing.T) {
	const tiles = 8

	arrived := make(chan struct{}, tiles)
	release := make(chan struct{})
	var mu sync.Mutex
	inflight, peak := 0, 0

	ft := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
		mu.Lock()
		inflight++
		if inflight > peak {
			peak = inflight
		}
		mu.Unlock()
		arrived <- struct{}{}
		<-release
		mu.Lock()
		inflight--
		mu.Unlock()
		return pngResponse(req, fakePNG(0x5A), nil), nil
	}}
	p := newTestProxy(t, ft, newTestClock(), Config{})

	var wg sync.WaitGroup
	for i := 0; i < tiles; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			serve(p, fmt.Sprintf("/api/tiles/9/255/%d.png", i))
		}(i)
	}

	// Wait until the cap is genuinely saturated, then check nothing more
	// starts while those fetches are held.
	for i := 0; i < maxUpstreamConcurrency; i++ {
		select {
		case <-arrived:
		case <-time.After(5 * time.Second):
			t.Fatalf("only %d of %d permitted fetches started", i, maxUpstreamConcurrency)
		}
	}
	select {
	case <-arrived:
		t.Errorf("fetch %d started while %d were already in flight (cap is %d)",
			maxUpstreamConcurrency+1, maxUpstreamConcurrency, maxUpstreamConcurrency)
	case <-time.After(100 * time.Millisecond):
	}

	close(release)
	wg.Wait()

	mu.Lock()
	got := peak
	mu.Unlock()
	if got > maxUpstreamConcurrency {
		t.Errorf("peak concurrent upstream fetches = %d, want at most %d", got, maxUpstreamConcurrency)
	}
	if ft.count() != tiles {
		t.Errorf("upstream requests = %d, want %d (all tiles eventually fetched)", ft.count(), tiles)
	}
}

// TestSlowUpstreamTimesOut: a browser tab must never sit waiting on a stalled
// tile server. The timeout is what makes the 503 fallback reachable at all.
func TestSlowUpstreamTimesOut(t *testing.T) {
	release := make(chan struct{})
	defer close(release)

	ft := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
		select {
		case <-req.Context().Done():
			return nil, req.Context().Err()
		case <-release:
			return pngResponse(req, fakePNG(0x5B), nil), nil
		}
	}}
	p := newTestProxy(t, ft, newTestClock(), Config{Timeout: 50 * time.Millisecond})

	done := make(chan *httptest.ResponseRecorder, 1)
	go func() { done <- serve(p, "/api/tiles/4/8/5.png") }()
	select {
	case rec := <-done:
		assertUnavailable(t, rec)
	case <-time.After(5 * time.Second):
		t.Fatal("a stalled upstream never timed out: the request is still waiting")
	}
}

// TestNonPNGUpstreamBodyIsRejected: a captive portal or an error page served
// with a 200 must not be cached for a week and handed over as a tile.
func TestNonPNGUpstreamBodyIsRejected(t *testing.T) {
	ft := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
		return pngResponse(req, []byte("<html>sign in to continue</html>"), nil), nil
	}}
	p := newTestProxy(t, ft, newTestClock(), Config{})

	assertUnavailable(t, serve(p, "/api/tiles/4/8/5.png"))

	// And nothing was written to the cache: the next request tries again.
	assertUnavailable(t, serve(p, "/api/tiles/4/8/5.png"))
	if got := ft.count(); got != 2 {
		t.Errorf("upstream requests = %d, want 2 (nothing should have been cached)", got)
	}
}

func TestOversizedUpstreamBodyIsRejected(t *testing.T) {
	huge := make([]byte, maxTileBytes+16)
	copy(huge, pngSignature)
	ft := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
		return pngResponse(req, huge, nil), nil
	}}
	p := newTestProxy(t, ft, newTestClock(), Config{})
	assertUnavailable(t, serve(p, "/api/tiles/4/8/5.png"))
}

// --- rejection before anything reaches the network -----------------------

func TestBadRequestsAreRejectedBeforeUpstream(t *testing.T) {
	paths := []string{
		"/api/tiles/20/0/0.png",
		"/api/tiles/2/4/0.png",
		"/api/tiles/2/0/4.png",
		"/api/tiles/-1/0/0.png",
		"/api/tiles/1/-1/0.png",
		"/api/tiles/01/0/0.png",
		"/api/tiles/a/b/c.png",
		"/api/tiles/2/1/1.jpg",
		"/api/tiles/2/1.png",
		"/api/tiles/2/1/1/1.png",
		"/api/tiles/../../etc/passwd.png",
		"/api/tiles/1/../../../secret.png",
		`/api/tiles/2/1/..\..\secret.png`,
		"/api/tiles/",
	}
	ft := &fakeTransport{respond: func(*http.Request, int) (*http.Response, error) {
		return nil, errors.New("upstream must not be contacted for a malformed request")
	}}
	p := newTestProxy(t, ft, newTestClock(), Config{})

	for _, path := range paths {
		t.Run(path, func(t *testing.T) {
			rec := serve(p, path)
			if rec.Code != http.StatusBadRequest {
				t.Fatalf("status = %d, want 400 (body %q)", rec.Code, rec.Body.String())
			}
			var body errorBody
			if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
				t.Fatalf("body %q is not JSON: %v", rec.Body.String(), err)
			}
			if body.Error != "bad_tile_request" {
				t.Errorf("error = %q, want %q", body.Error, "bad_tile_request")
			}
		})
	}
	if got := ft.count(); got != 0 {
		t.Fatalf("upstream requests = %d, want 0: nothing invalid may reach the network", got)
	}
}

// TestRejectedRequestWritesNothingToTheCache is the traversal assertion with
// teeth: after every hostile path, the cache directory is still empty.
func TestRejectedRequestWritesNothingToTheCache(t *testing.T) {
	dir := t.TempDir()
	ft := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
		return pngResponse(req, fakePNG(0xE1), nil), nil
	}}
	p := newTestProxy(t, ft, newTestClock(), Config{CacheDir: dir})

	for _, path := range []string{
		"/api/tiles/../../etc/passwd.png",
		"/api/tiles/1/../../../secret.png",
		"/api/tiles/2/1/../../secret.png",
		`/api/tiles/2/1/..\..\secret.png`,
	} {
		if rec := serve(p, path); rec.Code != http.StatusBadRequest {
			t.Fatalf("%s: status = %d, want 400", path, rec.Code)
		}
	}

	var files []string
	err := filepath.WalkDir(dir, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if !d.IsDir() {
			files = append(files, path)
		}
		return nil
	})
	if err != nil {
		t.Fatalf("walk: %v", err)
	}
	if len(files) != 0 {
		t.Fatalf("cache directory contains %v after rejected requests, want nothing", files)
	}
}

// --- coalescing ----------------------------------------------------------

// TestConcurrentRequestsForOneTileFetchOnce: a fresh pan can ask for the
// same square from several overlapping requests, and the policy is explicit
// about not loading the servers needlessly.
func TestConcurrentRequestsForOneTileFetchOnce(t *testing.T) {
	const followers = 7

	body := fakePNG(0xF1)
	release := make(chan struct{})
	ft := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
		<-release // hold the leader inside the round trip
		return pngResponse(req, body, nil), nil
	}}
	p := newTestProxy(t, ft, newTestClock(), Config{})

	// Every request that attaches to the in-flight fetch reports in, so the
	// leader can be released only once all of them are genuinely waiting —
	// no sleeps, no flakiness.
	var joined sync.WaitGroup
	joined.Add(followers)
	p.joined = joined.Done

	results := make(chan *httptest.ResponseRecorder, followers+1)
	var wg sync.WaitGroup
	for i := 0; i < followers+1; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			results <- serve(p, "/api/tiles/9/255/170.png")
		}()
	}

	// Bounded, so that a proxy which does NOT coalesce fails here with a
	// readable message instead of hanging until the test binary's timeout.
	allJoined := make(chan struct{})
	go func() {
		joined.Wait()
		close(allJoined)
	}()
	select {
	case <-allJoined:
	case <-time.After(5 * time.Second):
		t.Error("no request joined the in-flight fetch: requests for one tile are not being coalesced")
	}
	close(release)
	wg.Wait()
	close(results)

	for rec := range results {
		if rec.Code != http.StatusOK {
			t.Errorf("status = %d, want 200 (body %q)", rec.Code, rec.Body.String())
			continue
		}
		if !bytes.Equal(rec.Body.Bytes(), body) {
			t.Errorf("body = %v, want %v", rec.Body.Bytes(), body)
		}
	}
	if got := ft.count(); got != 1 {
		t.Fatalf("upstream requests = %d, want exactly 1 for %d concurrent requests", got, followers+1)
	}
}

func TestConcurrentRequestsForDifferentTilesEachFetch(t *testing.T) {
	ft := &fakeTransport{respond: func(req *http.Request, n int) (*http.Response, error) {
		return pngResponse(req, fakePNG(byte(n)), nil), nil
	}}
	p := newTestProxy(t, ft, newTestClock(), Config{})

	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			if rec := serve(p, fmt.Sprintf("/api/tiles/9/255/%d.png", i)); rec.Code != http.StatusOK {
				t.Errorf("tile %d: status = %d, want 200", i, rec.Code)
			}
		}(i)
	}
	wg.Wait()

	if got := ft.count(); got != 8 {
		t.Fatalf("upstream requests = %d, want 8 (one per distinct tile)", got)
	}
}

// --- disabled ------------------------------------------------------------

func TestDisabledProxyNeverContactsUpstream(t *testing.T) {
	p := NewDisabled("")
	for _, path := range []string{"/api/tiles/2/1/1.png", "/api/tiles/nonsense", "/api/tiles/"} {
		rec := serve(p, path)
		assertUnavailable(t, rec)
	}
	if p.Enabled() {
		t.Error("Enabled() = true for a disabled proxy")
	}
	if p.CacheDir() != "" {
		t.Errorf("CacheDir() = %q for a disabled proxy, want empty", p.CacheDir())
	}
}

func TestFromFlag(t *testing.T) {
	dir := t.TempDir()

	for _, spec := range []string{"off", "OFF", " off ", "none", "disabled", "false"} {
		p, err := FromFlag(spec, dir, DefaultMaxCacheBytes, nil)
		if err != nil {
			t.Fatalf("FromFlag(%q): %v", spec, err)
		}
		if p.Enabled() {
			t.Errorf("FromFlag(%q) is enabled, want disabled", spec)
		}
		assertUnavailable(t, serve(p, "/api/tiles/2/1/1.png"))
	}

	p, err := FromFlag("", dir, DefaultMaxCacheBytes, nil)
	if err != nil {
		t.Fatalf("FromFlag(\"\"): %v", err)
	}
	if !p.Enabled() || p.URLTemplate() != DefaultURLTemplate {
		t.Errorf("FromFlag(\"\") template = %q, want %q", p.URLTemplate(), DefaultURLTemplate)
	}

	custom := "https://tiles.example.invalid/{z}/{x}/{y}.png"
	p, err = FromFlag(custom, dir, DefaultMaxCacheBytes, nil)
	if err != nil {
		t.Fatalf("FromFlag(%q): %v", custom, err)
	}
	if p.URLTemplate() != custom {
		t.Errorf("FromFlag(%q) template = %q", custom, p.URLTemplate())
	}

	if _, err := FromFlag("not a url", dir, DefaultMaxCacheBytes, nil); err == nil {
		t.Error("FromFlag with a template that has no placeholders: want an error")
	}
}

// --- configuration -------------------------------------------------------

func TestNewRejectsBadConfig(t *testing.T) {
	dir := t.TempDir()
	tests := []struct {
		name string
		cfg  Config
	}{
		{name: "no cache dir", cfg: Config{URLTemplate: DefaultURLTemplate}},
		{name: "no placeholders", cfg: Config{URLTemplate: "https://example.invalid/tile.png", CacheDir: dir}},
		{name: "missing y", cfg: Config{URLTemplate: "https://example.invalid/{z}/{x}.png", CacheDir: dir}},
		{name: "not absolute", cfg: Config{URLTemplate: "/{z}/{x}/{y}.png", CacheDir: dir}},
		{name: "wrong scheme", cfg: Config{URLTemplate: "ftp://example.invalid/{z}/{x}/{y}.png", CacheDir: dir}},
		// The tile policy requires HTTPS to OSM's own servers, and a
		// hand-typed template is the one way plain http could get there.
		{name: "plain http to OSM", cfg: Config{URLTemplate: "http://tile.openstreetmap.org/{z}/{x}/{y}.png", CacheDir: dir}},
		{name: "plain http to an OSM subdomain", cfg: Config{URLTemplate: "http://a.tile.openstreetmap.org/{z}/{x}/{y}.png", CacheDir: dir}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if _, err := New(tt.cfg); err == nil {
				t.Fatalf("New(%+v) = nil error, want a rejection", tt.cfg)
			}
		})
	}

	// ...but plain http to anything else is still fine: a tile server on the
	// same bench or a local mirror is a legitimate configuration, and this
	// package's own manual verification depends on it.
	for _, tmpl := range []string{
		"http://127.0.0.1:8098/{z}/{x}/{y}.png",
		"http://tiles.example.invalid/{z}/{x}/{y}.png",
		"https://tile.openstreetmap.org/{z}/{x}/{y}.png",
	} {
		if _, err := New(Config{URLTemplate: tmpl, CacheDir: dir}); err != nil {
			t.Errorf("New(%q) = %v, want it accepted", tmpl, err)
		}
	}
}

// TestDifferentTemplatesDoNotShareCacheEntries: same z/x/y, different
// provider, entirely different pixels.
func TestDifferentTemplatesDoNotShareCacheEntries(t *testing.T) {
	dir := t.TempDir()
	clk := newTestClock()

	osmBody, otherBody := fakePNG(0x01), fakePNG(0x02)
	osmFT := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
		return pngResponse(req, osmBody, nil), nil
	}}
	otherFT := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
		return pngResponse(req, otherBody, nil), nil
	}}

	osm := newTestProxy(t, osmFT, clk, Config{CacheDir: dir})
	other := newTestProxy(t, otherFT, clk, Config{CacheDir: dir, URLTemplate: "https://tiles.example.invalid/{z}/{x}/{y}.png"})

	if rec := serve(osm, "/api/tiles/4/8/5.png"); !bytes.Equal(rec.Body.Bytes(), osmBody) {
		t.Fatalf("osm body = %v, want %v", rec.Body.Bytes(), osmBody)
	}
	rec := serve(other, "/api/tiles/4/8/5.png")
	if !bytes.Equal(rec.Body.Bytes(), otherBody) {
		t.Fatalf("second provider served %v, want its own %v", rec.Body.Bytes(), otherBody)
	}
	if got := rec.Header().Get("X-Tile-Cache"); got != "miss" {
		t.Errorf("second provider X-Tile-Cache = %q, want miss", got)
	}
	if got := otherFT.count(); got != 1 {
		t.Errorf("second provider upstream requests = %d, want 1", got)
	}
}

// TestCacheStaysUnderBudget drives eviction through the handler, the way it
// actually happens: enough distinct tiles to overflow a small budget.
func TestCacheStaysUnderBudget(t *testing.T) {
	dir := t.TempDir()
	body := make([]byte, 800)
	copy(body, pngSignature)

	clk := newTestClock()
	ft := &fakeTransport{respond: func(req *http.Request, _ int) (*http.Response, error) {
		return pngResponse(req, body, nil), nil
	}}
	const budget = 4000
	p := newTestProxy(t, ft, clk, Config{CacheDir: dir, MaxCacheBytes: budget})

	for i := 0; i < 20; i++ {
		if rec := serve(p, fmt.Sprintf("/api/tiles/10/512/%d.png", i)); rec.Code != http.StatusOK {
			t.Fatalf("tile %d: status = %d, want 200", i, rec.Code)
		}
		clk.advance(time.Second)
	}

	if got := dirSize(t, dir); got > budget {
		t.Fatalf("cache size = %d bytes after 20 tiles, want <= %d", got, budget)
	}
	// The most recent tile is still there — eviction takes the oldest.
	if rec := serve(p, "/api/tiles/10/512/19.png"); rec.Header().Get("X-Tile-Cache") != "hit" {
		t.Errorf("newest tile X-Tile-Cache = %q, want hit", rec.Header().Get("X-Tile-Cache"))
	}
}

// --- header parsing ------------------------------------------------------

func TestFreshUntilHonoursUpstreamHeaders(t *testing.T) {
	now := time.Date(2024, 6, 1, 12, 0, 0, 0, time.UTC)
	tests := []struct {
		name   string
		header http.Header
		want   time.Time
	}{
		{
			name:   "max-age",
			header: http.Header{"Cache-Control": {"public, max-age=3600"}},
			want:   now.Add(time.Hour),
		},
		{
			name:   "max-age with other directives",
			header: http.Header{"Cache-Control": {"must-revalidate, max-age=120, public"}},
			want:   now.Add(2 * time.Minute),
		},
		{
			name:   "expires when no max-age",
			header: http.Header{"Expires": {"Sun, 02 Jun 2024 12:00:00 GMT"}},
			want:   now.Add(24 * time.Hour),
		},
		{
			name:   "nothing falls back to the policy minimum",
			header: http.Header{},
			want:   now.Add(DefaultFreshness),
		},
		{
			name:   "unparseable max-age falls back",
			header: http.Header{"Cache-Control": {"max-age=soon"}},
			want:   now.Add(DefaultFreshness),
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := freshUntil(tt.header, now); !got.Equal(tt.want) {
				t.Fatalf("freshUntil = %v, want %v", got, tt.want)
			}
		})
	}
}

// TestDefaultFreshnessMeetsPolicyMinimum guards the one number the tile
// policy states outright.
func TestDefaultFreshnessMeetsPolicyMinimum(t *testing.T) {
	if DefaultFreshness < 7*24*time.Hour {
		t.Fatalf("DefaultFreshness = %v, the tile policy requires at least 7 days", DefaultFreshness)
	}
	if browserMaxAge != "public, max-age=604800" {
		t.Fatalf("browserMaxAge = %q, want the contract's 7-day value", browserMaxAge)
	}
}

// assertUnavailable checks the contract's 503 body.
func assertUnavailable(t *testing.T, rec *httptest.ResponseRecorder) {
	t.Helper()
	if rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503 (body %q)", rec.Code, rec.Body.String())
	}
	if ct := rec.Header().Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		t.Errorf("Content-Type = %q, want application/json", ct)
	}
	var body errorBody
	if err := json.Unmarshal(rec.Body.Bytes(), &body); err != nil {
		t.Fatalf("body %q is not JSON: %v", rec.Body.String(), err)
	}
	if body.Error != "tiles_unavailable" {
		t.Errorf("error = %q, want %q", body.Error, "tiles_unavailable")
	}
	if body.Message == "" {
		t.Error("message is empty, want something a user can act on")
	}
}
