// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

// Package tiles is a small, strictly demand-driven caching proxy for slippy
// map tiles, so the ADS-B panel can draw a real OpenStreetMap basemap under
// the aircraft it plots instead of a bare graticule.
//
// # Why a proxy at all
//
// The browser could fetch tile.openstreetmap.org directly, but then every
// pan re-requests tiles this machine has already seen, there is no way to
// send the User-Agent the OSM tile policy demands (browsers refuse to let
// script set it), and a portal running on an offline bench would show a
// permanently broken map. Proxying through here gives us one place to hold
// all three: the policy User-Agent, a 7-day on-disk cache, and an honest
// failure mode.
//
// # Endpoint contract
//
//	GET /api/tiles/{z}/{x}/{y}.png
//	  200  image/png, body is the tile
//	       Cache-Control: public, max-age=604800
//	       X-Tile-Cache: hit | miss | revalidated
//	  400  malformed or out-of-range z/x/y (valid: 0 <= z <= 19, 0 <= x,y < 2^z)
//	  503  application/json {"error":"tiles_unavailable","message":"..."}
//	       when tiles are disabled by configuration, or upstream is
//	       unreachable and nothing usable is cached.
//
// The panel treats any non-200 as "no basemap" and falls back to its
// graticule, so a missing or blocked tile server degrades to the map the
// panel drew before this package existed — never to a broken page.
//
// # OSM tile usage policy
//
// https://operations.osmfoundation.org/policies/tiles/ — these are hard
// requirements, not suggestions, and the volunteer-funded servers are
// entitled to block anyone who ignores them:
//
//   - Every upstream request identifies this app and gives a contact URL
//     (UserAgent below). Library defaults such as "Go-http-client/1.1" are
//     explicitly prohibited, which is why Do() sets the header by hand on
//     every request rather than trusting a transport default.
//   - Tiles are cached on disk for at least 7 days (DefaultFreshness), and
//     upstream Cache-Control/Expires/ETag/Last-Modified are honoured:
//     a stale entry is re-validated conditionally, so the usual answer is a
//     304 with no tile bytes at all. Requests never carry no-cache.
//   - The cache is filled ONLY by tiles a browser actually asked to display.
//     Bulk downloading, pre-seeding areas or zoom levels, building tile
//     archives and offline features are prohibited: do not add a prefetch,
//     "download this area", or warm-up feature to this package, however
//     convenient it would be for a field deployment.
//   - Attribution is the panel's job (bottom-right, always visible), not
//     this package's; it is called out here because the two halves of that
//     obligation are easy to lose track of separately.
package tiles

import (
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	// DefaultURLTemplate is OpenStreetMap's own standard-style tile server.
	// {z}/{x}/{y} are Web Mercator (EPSG:3857) slippy-map coordinates: the
	// panel MUST project in Web Mercator to line its aircraft up with these
	// (an equirectangular plot is wrong by kilometres at UK latitudes).
	DefaultURLTemplate = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"

	// UserAgent identifies this application upstream, as the OSM tile policy
	// requires: a name that can be traced back to the software, plus a
	// contact URL. Do not replace this with a library default.
	//
	// The version here is the remote-UI/portal protocol version this webgui
	// speaks (doc/REMOTE-UI.md), NOT the CMake project version — it is only
	// ever read by a human looking at a tile server's logs, so what matters
	// is that it identifies a build, and that it is changed deliberately
	// rather than drifting. README.md and internal/portal/PANELS.md quote it
	// verbatim; change all three together or not at all.
	UserAgent = "mayhem-b200/0.9.0 (+https://github.com/wonderingStars/mayhem-b200)"

	// PathPrefix and Route are the one definition of this endpoint's path,
	// referenced by every mount point (the portal server and the dev
	// harness) so the two can never drift apart. The route is a prefix
	// rather than "GET /api/tiles/{z}/{x}/{y}.png" because net/http's mux
	// rejects a wildcard with a literal suffix ("bad wildcard segment (must
	// end with '}')"), so this package parses the three coordinates itself.
	PathPrefix = "/api/tiles/"
	Route      = "GET " + PathPrefix

	// MaxZoom is the deepest zoom level accepted. OSM's standard style is
	// rendered to z19; anything beyond that is a client bug, and refusing
	// it here keeps a runaway request loop off the upstream servers.
	MaxZoom = 19

	// DefaultMaxCacheBytes bounds the on-disk cache. A 256px PNG tile is
	// typically 5-60 KiB, so this holds a few thousand of them: enough for
	// the areas one receiver actually watches, which is all a demand-driven
	// cache ever holds.
	DefaultMaxCacheBytes int64 = 256 << 20

	// DefaultTimeout bounds one upstream tile fetch end to end. Tiles are
	// decoration: a browser tab must never sit waiting on a slow tile
	// server, and the panel's graticule fallback is a fine answer.
	DefaultTimeout = 10 * time.Second

	// DefaultFreshness is how long a cached tile is served without asking
	// upstream anything at all, when upstream sends no Cache-Control or
	// Expires of its own. Seven days is the minimum the tile policy asks
	// for; upstream is free to ask for longer, and OSM does.
	DefaultFreshness = 7 * 24 * time.Hour

	// browserMaxAge is the Cache-Control this proxy sends to the browser.
	// Same seven days, so a tab that revisits a tile does not even reach
	// this process, let alone the OSM servers.
	browserMaxAge = "public, max-age=604800"

	// maxUpstreamConcurrency caps parallel upstream fetches. A pan at z13
	// can uncover ~20 new tiles at once; letting all of them out
	// simultaneously is exactly the "heavy load" the policy warns about,
	// and a browser opens its own connections per tile anyway.
	maxUpstreamConcurrency = 3

	// maxTileBytes caps how much of an upstream response is ever read into
	// memory. Two orders of magnitude above a real tile, so it only ever
	// trips on a misbehaving or hijacked endpoint.
	maxTileBytes = 1 << 20

	// logInterval throttles upstream-failure logging. Offline, every tile
	// in a pan fails; one line every half minute is a diagnosis, twenty
	// lines a second is noise that buries everything else.
	logInterval = 30 * time.Second

	// blockBackoff is how long upstream is left completely alone after it
	// has told us to go away — 429 Too Many Requests, or the 403 OSM answers
	// a blocked client with. The policy is explicit that availability is
	// best-effort and that being blocked without notice is possible, and
	// continuing to knock is exactly what turns a rate limit into a
	// permanent block: every fresh tile scrolled into view would otherwise
	// produce another request, since the panel only gives up when NOTHING
	// has ever loaded. A basemap that stops for a minute costs nothing.
	blockBackoff = time.Minute

	// maxBlockBackoff bounds the doubling. Half an hour is long enough to
	// stop being a nuisance and short enough that an operator who fixes the
	// problem is not left staring at a graticule for the rest of the day.
	maxBlockBackoff = 30 * time.Minute
)

// pngSignature is the 8-byte PNG magic. Checked on every upstream body: a
// captive portal or an error page served with status 200 would otherwise be
// cached for a week and handed to the browser as a tile.
var pngSignature = []byte{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a}

// Config configures a Proxy. The zero value is not usable — CacheDir is
// required — but every other field has a sensible default.
type Config struct {
	// URLTemplate is the tile URL with {z}, {x} and {y} placeholders.
	// Empty means DefaultURLTemplate.
	URLTemplate string

	// CacheDir is the root of the on-disk cache. Required. Tiles live in a
	// per-template subdirectory of it (see cache.prefix), so pointing two
	// differently-configured proxies at one directory is safe.
	CacheDir string

	// MaxCacheBytes is the size budget for CacheDir. <= 0 means
	// DefaultMaxCacheBytes.
	MaxCacheBytes int64

	// Transport is the http.RoundTripper used for upstream requests. Nil
	// means http.DefaultTransport. Tests inject a fake here so no test in
	// this package ever touches the network.
	Transport http.RoundTripper

	// Timeout bounds one upstream fetch. <= 0 means DefaultTimeout.
	Timeout time.Duration

	// Now is the clock, injectable so tests can age a cache entry past its
	// freshness window without sleeping. Nil means time.Now.
	Now func() time.Time

	// Logf, if set, receives one line per upstream failure (throttled — see
	// logInterval). Nil discards them.
	Logf func(format string, args ...any)
}

// Proxy serves the tile endpoint described in the package doc. It is safe
// for concurrent use, and coalesces concurrent requests for the same tile
// into a single upstream fetch.
type Proxy struct {
	tmpl    string
	client  *http.Client
	timeout time.Duration
	now     func() time.Time
	logf    func(format string, args ...any)
	cache   *cache

	// sem bounds upstream concurrency; see maxUpstreamConcurrency.
	sem chan struct{}

	mu       sync.Mutex
	inflight map[string]*call
	lastLog  time.Time

	// blockedUntil/blockFor are the back-off imposed by a 429 or 403; see
	// blockBackoff. Zero blockedUntil means upstream is not in the dog house.
	blockedUntil time.Time
	blockFor     time.Duration

	// joined, when set, is called by each request that attaches itself to
	// an already-in-flight fetch instead of starting its own. A test seam,
	// and the only one in this package: coalescing is otherwise observable
	// only as an absence — an upstream request that did not happen — which
	// a test can never deterministically wait for.
	joined func()

	// disabled proxies answer every request with the 503 contract body and
	// never touch the network or the disk. This is a whole Proxy rather
	// than a nil handler so that the route is always mounted: without it
	// the portal's SPA catch-all would answer /api/tiles/... with
	// index.html and a 200, which the panel would try to decode as a PNG.
	disabled bool
	reason   string
}

// New builds an enabled Proxy from cfg.
func New(cfg Config) (*Proxy, error) {
	tmpl := cfg.URLTemplate
	if tmpl == "" {
		tmpl = DefaultURLTemplate
	}
	if err := validateTemplate(tmpl); err != nil {
		return nil, err
	}
	if cfg.CacheDir == "" {
		return nil, errors.New("tiles: no cache directory configured")
	}
	if err := os.MkdirAll(cfg.CacheDir, 0o755); err != nil {
		return nil, fmt.Errorf("tiles: cache directory %s: %w", cfg.CacheDir, err)
	}

	maxBytes := cfg.MaxCacheBytes
	if maxBytes <= 0 {
		maxBytes = DefaultMaxCacheBytes
	}
	timeout := cfg.Timeout
	if timeout <= 0 {
		timeout = DefaultTimeout
	}
	now := cfg.Now
	if now == nil {
		now = time.Now
	}

	p := &Proxy{
		tmpl:     tmpl,
		client:   &http.Client{Transport: cfg.Transport, Timeout: timeout},
		timeout:  timeout,
		now:      now,
		logf:     cfg.Logf,
		cache:    newCache(cfg.CacheDir, templateHash(tmpl), maxBytes, now),
		sem:      make(chan struct{}, maxUpstreamConcurrency),
		inflight: make(map[string]*call),
	}
	return p, nil
}

// NewDisabled returns a Proxy that answers every request with the contract's
// 503 body and never contacts upstream. reason is shown to the user; empty
// picks a generic one.
func NewDisabled(reason string) *Proxy {
	if reason == "" {
		reason = "basemap tiles are disabled"
	}
	return &Proxy{disabled: true, reason: reason}
}

// FromFlag builds the Proxy described by a -tiles flag value: "off" (also
// "none"/"disabled"/"false") disables tiles entirely, empty means
// DefaultURLTemplate, anything else is used as the URL template. Shared by
// both binaries that mount this endpoint so their flags behave identically.
func FromFlag(spec, cacheDir string, maxBytes int64, logf func(string, ...any)) (*Proxy, error) {
	switch strings.ToLower(strings.TrimSpace(spec)) {
	case "off", "none", "disabled", "false":
		return NewDisabled("basemap tiles are disabled (-tiles=off)"), nil
	}
	if cacheDir == "" {
		dir, err := DefaultCacheDir()
		if err != nil {
			return nil, err
		}
		cacheDir = dir
	}
	return New(Config{
		URLTemplate:   spec,
		CacheDir:      cacheDir,
		MaxCacheBytes: maxBytes,
		Logf:          logf,
	})
}

// DefaultCacheDir is where tiles are cached when no directory is given: the
// per-user cache location, which is exactly what an OS-managed cache
// directory is for (it may be cleared behind our back, and a demand-driven
// cache can always refill itself).
func DefaultCacheDir() (string, error) {
	base, err := os.UserCacheDir()
	if err != nil {
		// No HOME/LOCALAPPDATA: fall back rather than refusing to start,
		// since a tile cache is never worth failing a launch over.
		base = os.TempDir()
	}
	return filepath.Join(base, "mayhem-b200", "tiles"), nil
}

// Enabled reports whether this proxy will actually serve tiles, for the
// benefit of a caller that wants to say so in its startup log.
func (p *Proxy) Enabled() bool { return !p.disabled }

// URLTemplate returns the configured upstream template, or "" when disabled.
func (p *Proxy) URLTemplate() string { return p.tmpl }

// CacheDir returns the on-disk cache root, or "" when disabled.
func (p *Proxy) CacheDir() string {
	if p.cache == nil {
		return ""
	}
	return p.cache.root
}

func (p *Proxy) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if p.disabled {
		writeUnavailable(w, p.reason)
		return
	}

	t, err := ParsePath(r.URL.Path)
	if err != nil {
		// 400 before anything else: nothing unvalidated is allowed to reach
		// a filesystem path or an upstream URL.
		writeError(w, http.StatusBadRequest, "bad_tile_request", err.Error())
		return
	}

	res, err := p.get(r.Context(), t)
	if err != nil {
		p.logFailure("tiles: %s: %v", t.key(), err)
		writeUnavailable(w, "tile server is unreachable and this tile is not cached")
		return
	}

	w.Header().Set("Content-Type", "image/png")
	w.Header().Set("Cache-Control", browserMaxAge)
	w.Header().Set("X-Tile-Cache", res.cache)
	if res.stale {
		// Diagnostic only, and additive to the contract: these bytes are
		// past their freshness window and were served because upstream
		// could not be reached. Better a week-old coastline than a hole.
		w.Header().Set("X-Tile-Stale", "1")
	}
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(res.body)
}

// result is one answer from the cache/upstream pipeline.
type result struct {
	body  []byte
	cache string // X-Tile-Cache: "hit", "miss" or "revalidated"
	stale bool   // served from cache because upstream failed
}

// call is one in-flight get for a single tile, so that a pan which uncovers
// the same tile in two overlapping requests fetches it once.
type call struct {
	done chan struct{}
	res  result
	err  error
}

// get returns the tile, coalescing concurrent requests for the same one.
func (p *Proxy) get(ctx context.Context, t Tile) (result, error) {
	key := t.key()

	p.mu.Lock()
	if c, ok := p.inflight[key]; ok {
		joined := p.joined
		p.mu.Unlock()
		if joined != nil {
			joined()
		}
		select {
		case <-c.done:
			return c.res, c.err
		case <-ctx.Done():
			// This caller gave up; the leader carries on and still fills
			// the cache, which is the whole point of coalescing.
			return result{}, ctx.Err()
		}
	}
	c := &call{done: make(chan struct{})}
	p.inflight[key] = c
	p.mu.Unlock()

	c.res, c.err = p.fetchOrLoad(ctx, t)

	p.mu.Lock()
	delete(p.inflight, key)
	p.mu.Unlock()
	close(c.done)

	return c.res, c.err
}

// fetchOrLoad implements the cache policy for one tile: serve fresh bytes
// from disk, else revalidate conditionally, else download, and on upstream
// failure fall back to whatever stale copy exists.
func (p *Proxy) fetchOrLoad(ctx context.Context, t Tile) (result, error) {
	body, m, cached := p.cache.load(t)
	now := p.now()
	if cached && now.Before(m.FreshUntil) {
		p.cache.touch(t)
		return result{body: body, cache: "hit"}, nil
	}

	var conditional *entryMeta
	if cached {
		conditional = &m
	}
	fr, err := p.fetchUpstream(ctx, t, conditional)
	if err != nil {
		if cached {
			// Upstream is down but we have bytes for this square. A stale
			// tile is a far better answer than a hole in the map, and the
			// browser is told about it with X-Tile-Stale.
			p.cache.touch(t)
			return result{body: body, cache: "hit", stale: true}, nil
		}
		return result{}, err
	}

	if fr.notModified {
		// 304: upstream confirmed our bytes are still current. Re-arm the
		// freshness window (and pick up a rotated ETag, if it sent one) so
		// the next week of requests never leave this machine.
		m.FreshUntil = freshUntil(fr.header, p.now())
		if etag := fr.header.Get("ETag"); etag != "" {
			m.ETag = etag
		}
		if lm := fr.header.Get("Last-Modified"); lm != "" {
			m.LastModified = lm
		}
		p.cache.storeMeta(t, m)
		return result{body: body, cache: "revalidated"}, nil
	}

	stored := entryMeta{
		URLTemplate:  p.tmpl,
		ETag:         fr.header.Get("ETag"),
		LastModified: fr.header.Get("Last-Modified"),
		FetchedAt:    p.now(),
		FreshUntil:   freshUntil(fr.header, p.now()),
		Size:         int64(len(fr.body)),
	}
	if err := p.cache.store(t, fr.body, stored); err != nil {
		// A cache we cannot write to is a performance problem, not a
		// correctness one: the tile in hand is still good.
		p.logFailure("tiles: caching %s: %v", t.key(), err)
	}
	return result{body: fr.body, cache: "miss"}, nil
}

// fetchResult is one upstream round trip.
type fetchResult struct {
	body        []byte
	notModified bool
	header      http.Header
}

// fetchUpstream performs the (possibly conditional) upstream GET. prev, when
// non-nil, supplies the validators for a conditional request.
func (p *Proxy) fetchUpstream(ctx context.Context, t Tile, prev *entryMeta) (fetchResult, error) {
	// Deliberately detached from the caller's context: if the browser tab
	// that triggered this navigates away, we still want the tile in the
	// cache for the next request, and any caller waiting on this same tile
	// still gets an answer. p.timeout bounds it regardless.
	ctx, cancel := context.WithTimeout(context.WithoutCancel(ctx), p.timeout)
	defer cancel()

	// Refuse before dialling, not after: an upstream that has just answered
	// 429 or 403 must not be contacted again at all until the back-off is
	// over. fetchOrLoad turns this error into a stale cached tile where it
	// has one, and the contract's 503 otherwise — the same as being offline,
	// which is exactly what this is.
	if left, blocked := p.blockedFor(); blocked {
		return fetchResult{}, fmt.Errorf("upstream asked us to back off; not retrying for another %v", left.Round(time.Second))
	}

	select {
	case p.sem <- struct{}{}:
		defer func() { <-p.sem }()
	case <-ctx.Done():
		return fetchResult{}, fmt.Errorf("waiting for an upstream slot: %w", ctx.Err())
	}

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, p.tileURL(t), nil)
	if err != nil {
		return fetchResult{}, err
	}
	// The policy's headline requirement. Set explicitly on every request:
	// a transport-level default would be silently lost the moment someone
	// injects a different http.RoundTripper.
	req.Header.Set("User-Agent", UserAgent)
	req.Header.Set("Accept", "image/png,image/*;q=0.8")
	if prev != nil {
		// Conditional revalidation, never no-cache: the usual answer is a
		// 304 with no tile bytes at all, which is what the policy asks for.
		if prev.ETag != "" {
			req.Header.Set("If-None-Match", prev.ETag)
		}
		if prev.LastModified != "" {
			req.Header.Set("If-Modified-Since", prev.LastModified)
		}
	}

	resp, err := p.client.Do(req)
	if err != nil {
		return fetchResult{}, err
	}
	defer resp.Body.Close()

	switch resp.StatusCode {
	case http.StatusNotModified:
		p.noteReachable()
		return fetchResult{notModified: true, header: resp.Header}, nil
	case http.StatusOK:
		p.noteReachable()
	case http.StatusTooManyRequests, http.StatusForbidden:
		p.noteBlocked()
		return fetchResult{}, fmt.Errorf("upstream returned status %d; backing off", resp.StatusCode)
	default:
		return fetchResult{}, fmt.Errorf("upstream returned status %d", resp.StatusCode)
	}

	body, err := io.ReadAll(io.LimitReader(resp.Body, maxTileBytes+1))
	if err != nil {
		return fetchResult{}, fmt.Errorf("reading tile: %w", err)
	}
	if len(body) > maxTileBytes {
		return fetchResult{}, fmt.Errorf("tile larger than %d bytes", maxTileBytes)
	}
	if !bytes.HasPrefix(body, pngSignature) {
		// A captive portal, a rate-limit notice or an HTML error page served
		// with a 200. Caching it would poison this square for a week.
		return fetchResult{}, errors.New("upstream response is not a PNG")
	}
	return fetchResult{body: body, header: resp.Header}, nil
}

// blockedFor reports how much of a 429/403 back-off is left, and whether one
// is in force at all.
func (p *Proxy) blockedFor() (time.Duration, bool) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.blockedUntil.IsZero() {
		return 0, false
	}
	left := p.blockedUntil.Sub(p.now())
	if left <= 0 {
		return 0, false
	}
	return left, true
}

// noteBlocked starts (or doubles) the back-off after upstream refused us.
// Doubling matters because the thing that gets a client blocked for good is
// resuming full-rate requests the moment a temporary limit expires.
func (p *Proxy) noteBlocked() {
	p.mu.Lock()
	defer p.mu.Unlock()
	next := p.blockFor * 2
	if next < blockBackoff {
		next = blockBackoff
	}
	if next > maxBlockBackoff {
		next = maxBlockBackoff
	}
	p.blockFor = next
	p.blockedUntil = p.now().Add(next)
}

// noteReachable clears the back-off: upstream served us, so whatever it was
// objecting to is over and the next limit starts from the shortest window.
func (p *Proxy) noteReachable() {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.blockedUntil = time.Time{}
	p.blockFor = 0
}

// tileURL substitutes validated (and therefore purely numeric) coordinates
// into the template.
func (p *Proxy) tileURL(t Tile) string {
	return strings.NewReplacer(
		"{z}", strconv.Itoa(t.Z),
		"{x}", strconv.Itoa(t.X),
		"{y}", strconv.Itoa(t.Y),
	).Replace(p.tmpl)
}

// logFailure emits at most one line per logInterval, so an offline bench
// does not bury the rest of the log under one message per tile.
func (p *Proxy) logFailure(format string, args ...any) {
	if p.logf == nil {
		return
	}
	now := p.now()
	p.mu.Lock()
	quiet := !p.lastLog.IsZero() && now.Sub(p.lastLog) < logInterval
	if !quiet {
		p.lastLog = now
	}
	p.mu.Unlock()
	if !quiet {
		p.logf(format, args...)
	}
}

// validateTemplate rejects a template that cannot produce a tile URL, at
// construction time rather than on the first request.
func validateTemplate(tmpl string) error {
	for _, ph := range []string{"{z}", "{x}", "{y}"} {
		if !strings.Contains(tmpl, ph) {
			return fmt.Errorf("tiles: URL template %q is missing %s", tmpl, ph)
		}
	}
	// Substitute a real tile so the placeholders do not upset the parser.
	probe := strings.NewReplacer("{z}", "0", "{x}", "0", "{y}", "0").Replace(tmpl)
	u, err := url.Parse(probe)
	if err != nil {
		return fmt.Errorf("tiles: URL template %q: %w", tmpl, err)
	}
	if (u.Scheme != "http" && u.Scheme != "https") || u.Host == "" {
		return fmt.Errorf("tiles: URL template %q must be an absolute http(s) URL", tmpl)
	}
	// Plain http stays allowed in general — a tile server on the same bench,
	// or a local mirror, is a legitimate setup and tests need it — but not
	// for OSM's own servers, where the tile usage policy requires HTTPS. That
	// is a violation an operator could configure by hand without noticing, so
	// it is refused at startup rather than sent 40 times a pan.
	if u.Scheme == "http" && isOSMHost(u.Hostname()) {
		return fmt.Errorf("tiles: URL template %q must use https for OpenStreetMap's servers (their tile usage policy requires it)", tmpl)
	}
	return nil
}

// isOSMHost reports whether host is one of the OpenStreetMap Foundation's own
// tile hosts, which the tile usage policy governs directly.
func isOSMHost(host string) bool {
	host = strings.ToLower(host)
	return host == "openstreetmap.org" || strings.HasSuffix(host, ".openstreetmap.org")
}

// templateHash keys the on-disk cache by provider. Without it, switching
// -tiles to another server would serve that server's squares from the
// previous one's files — the same z/x/y, entirely different pixels.
func templateHash(tmpl string) string {
	sum := sha256.Sum256([]byte(tmpl))
	return hex.EncodeToString(sum[:8])
}

// freshUntil applies the upstream caching headers, falling back to the
// policy's seven days when it sends none.
func freshUntil(h http.Header, now time.Time) time.Time {
	if d, ok := maxAge(h.Get("Cache-Control")); ok {
		return now.Add(d)
	}
	if exp := h.Get("Expires"); exp != "" {
		if t, err := http.ParseTime(exp); err == nil {
			return t
		}
	}
	return now.Add(DefaultFreshness)
}

// maxAge extracts max-age from a Cache-Control header value.
func maxAge(cc string) (time.Duration, bool) {
	for _, part := range strings.Split(cc, ",") {
		part = strings.TrimSpace(part)
		v, ok := strings.CutPrefix(strings.ToLower(part), "max-age=")
		if !ok {
			continue
		}
		secs, err := strconv.Atoi(strings.TrimSpace(v))
		if err != nil || secs < 0 {
			return 0, false
		}
		return time.Duration(secs) * time.Second, true
	}
	return 0, false
}

// errorBody is the JSON shape of every non-200 this endpoint sends.
type errorBody struct {
	Error   string `json:"error"`
	Message string `json:"message"`
}

func writeUnavailable(w http.ResponseWriter, message string) {
	writeError(w, http.StatusServiceUnavailable, "tiles_unavailable", message)
}

func writeError(w http.ResponseWriter, status int, code, message string) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	// Our own failure must not be cached by the browser: the tile server
	// coming back should not need a reload. (Unrelated to the policy's
	// "never send no-cache", which is about our requests to upstream.)
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(errorBody{Error: code, Message: message})
}
