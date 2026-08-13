// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

// Package server is the portal HTTP server: the embedded browser UI for
// mayhem-b200's ~103 apps, plus a thin JSON proxy in front of the C++
// app-portal API (internal/portal/client) that adds nothing of its own
// except honest error translation (see writeBackendError) and the
// category grouping in internal/portal/appindex.
//
// This is deliberately a separate process/port from internal/web (the
// existing sdrlink radio-control GUI): the two talk to entirely different
// backends (an sdrlink TCP server vs. mayhem-b200's own HTTP API) and have
// no shared state. See cmd/mayhem-portal and the webgui README for how
// they're meant to be run — typically side by side, on different ports.
package server

import (
	"context"
	"encoding/json"
	"errors"
	"io/fs"
	"mime"
	"net/http"
	"path"
	"strings"

	"mayhemb200/webgui/internal/portal/appindex"
	"mayhemb200/webgui/internal/portal/client"
	"mayhemb200/webgui/internal/portal/tiles"
)

// Backend is the subset of *client.Client this server needs. An interface
// so tests can substitute a fake without spinning up a real HTTP server for
// every handler test (though handlers_test.go also covers the real
// *client.Client path end to end against an httptest backend, since the
// error-translation logic specifically depends on *client.Error).
type Backend interface {
	Apps(ctx context.Context) ([]client.App, error)
	CurrentApp(ctx context.Context) (client.CurrentApp, error)
	Launch(ctx context.Context, id string) (client.CurrentApp, error)
	Home(ctx context.Context) (client.CurrentApp, error)
	// Panel takes contract 4's image-cache token, passed through verbatim
	// (empty = omit the parameter). See client.Client.Panel.
	Panel(ctx context.Context, haveImageRev string) (client.Panel, error)
	Status(ctx context.Context) (client.Status, error)
	// Screen and Input are the framebuffer mirror, contracts 1 and 2; the
	// live screen view is built on them (see screen.go). They are part of
	// this interface rather than an optional one a Backend may or may not
	// implement, so a backend that forgets them fails to compile instead of
	// silently serving a portal with a dead screen.
	Screen(ctx context.Context, after uint32, waitMS int) (client.ScreenFrame, bool, error)
	Input(ctx context.Context, events []json.RawMessage) (client.InputResult, error)
}

var _ Backend = (*client.Client)(nil)

// Server implements http.Handler for the portal: the embedded SPA plus its
// /api/... JSON surface.
type Server struct {
	backend Backend
	assets  fs.FS
	tiles   http.Handler
	screen  *screenHub
	mux     *http.ServeMux
}

// Option configures optional Server parameters.
type Option func(*Server)

// withAssets overrides the embedded frontend filesystem. Unexported: real
// callers always get the embedded build; this exists so assets_test.go can
// exercise serveAsset against a small synthetic fs.FS.
func withAssets(fsys fs.FS) Option { return func(s *Server) { s.assets = fsys } }

// WithTiles supplies the basemap tile proxy served under
// tiles.PathPrefix (see internal/portal/tiles for the endpoint contract).
// Exported, unlike withAssets, because cmd/mayhem-portal has to build the
// proxy itself: only it knows the cache directory and -tiles flag.
//
// Without this option the route is still mounted, backed by a disabled
// proxy that answers the contract's 503 — see routes() for why that matters.
func WithTiles(h http.Handler) Option {
	return func(s *Server) {
		if h != nil {
			s.tiles = h
		}
	}
}

// New builds a Server backed by the given Backend.
func New(backend Backend, opts ...Option) *Server {
	s := &Server{
		backend: backend,
		assets:  Assets(),
		tiles:   tiles.NewDisabled("basemap tiles are not configured for this portal"),
		screen:  newScreenHub(backend),
	}
	for _, opt := range opts {
		opt(s)
	}
	s.routes()
	return s
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	s.mux.ServeHTTP(w, r)
}

func (s *Server) routes() {
	mux := http.NewServeMux()

	mux.HandleFunc("GET /api/apps", s.handleApps)
	mux.HandleFunc("GET /api/apps/current", s.handleCurrentApp)
	mux.HandleFunc("POST /api/apps/{id}/launch", s.handleLaunch)
	mux.HandleFunc("POST /api/apps/home", s.handleHome)
	mux.HandleFunc("GET /api/panel", s.handlePanel)
	mux.HandleFunc("GET /api/status", s.handleStatus)

	// The live screen: a WebSocket for browsers (contract 3) and the plain
	// HTTP pair (contracts 1 and 2) proxied straight through for anything
	// that can't hold a socket open. See screen.go.
	mux.HandleFunc("GET /api/screen", s.handleScreen)
	mux.HandleFunc("GET /api/screen/ws", s.handleScreenWS)
	mux.HandleFunc("POST /api/input", s.handleInput)

	// The basemap tile proxy is mounted unconditionally, even when tiles
	// are disabled: a disabled proxy answers 503 JSON, whereas an absent
	// route would fall through to the SPA catch-all below and hand the ADS-B
	// panel index.html with a 200 for every tile it asked for.
	mux.Handle(tiles.Route, s.tiles)

	// Catch-all: embedded SPA assets, including "/" -> index.html.
	mux.HandleFunc("GET /", s.serveAsset)

	s.mux = mux
}

// --- API handlers ------------------------------------------------------

func (s *Server) handleApps(w http.ResponseWriter, r *http.Request) {
	apps, err := s.backend.Apps(r.Context())
	if err != nil {
		s.writeBackendError(w, err)
		return
	}
	groups := appindex.Group(apps)
	if groups == nil {
		groups = []appindex.CategoryGroup{}
	}
	// The flat list is filtered the same way as the groups; see
	// appindex.Filter for why they must not disagree.
	writeJSON(w, http.StatusOK, appsResponse{Apps: appindex.Filter(apps), Groups: groups})
}

func (s *Server) handleCurrentApp(w http.ResponseWriter, r *http.Request) {
	cur, err := s.backend.CurrentApp(r.Context())
	if err != nil {
		s.writeBackendError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, cur)
}

func (s *Server) handleLaunch(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	if id == "" {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "missing app id"})
		return
	}
	cur, err := s.backend.Launch(r.Context(), id)
	if err != nil {
		s.writeBackendError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, cur)
}

func (s *Server) handleHome(w http.ResponseWriter, r *http.Request) {
	cur, err := s.backend.Home(r.Context())
	if err != nil {
		s.writeBackendError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, cur)
}

// handlePanel proxies GET /api/panel, forwarding contract 4's
// have_image_rev cache token. The token is relayed exactly as the browser
// sent it, never synthesized here: it says what the BROWSER already holds,
// and the portal has no idea. Getting that wrong would make the backend omit
// an image the browser has never seen.
func (s *Server) handlePanel(w http.ResponseWriter, r *http.Request) {
	p, err := s.backend.Panel(r.Context(), r.URL.Query().Get("have_image_rev"))
	if err != nil {
		s.writeBackendError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, p)
}

func (s *Server) handleStatus(w http.ResponseWriter, r *http.Request) {
	st, err := s.backend.Status(r.Context())
	if err != nil {
		s.writeBackendError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, st)
}

// writeBackendError translates a *client.Error (or, defensively, any other
// error) into an honest HTTP response:
//
//   - unreachable mayhem-b200 (no response at all)      -> 503, code
//     "backend_unavailable", so app.js can show the exact "mayhem-b200 is
//     not running" empty state the task calls for, not a generic failure.
//   - mayhem-b200 answered but with a body this client couldn't parse -> 502.
//   - mayhem-b200 answered with its own application error (e.g. launching
//     an unknown app id) -> that same status and message, passed through
//     unchanged, so the browser sees exactly what the backend said.
func (s *Server) writeBackendError(w http.ResponseWriter, err error) {
	var cerr *client.Error
	if !errors.As(err, &cerr) {
		writeJSON(w, http.StatusInternalServerError, errorResponse{Error: err.Error()})
		return
	}
	switch {
	case cerr.Unavailable():
		writeJSON(w, http.StatusServiceUnavailable, errorResponse{
			Error: "mayhem-b200 is not running",
			Code:  "backend_unavailable",
		})
	case cerr.DecodeFailed:
		writeJSON(w, http.StatusBadGateway, errorResponse{
			Error: "mayhem-b200 sent an unexpected response",
			Code:  "backend_bad_response",
		})
	case cerr.Status >= 400:
		writeJSON(w, cerr.Status, errorResponse{Error: cerr.Message})
	default:
		// Defensive: a *client.Error should always have Status==0,
		// DecodeFailed, or Status>=400 (see client.go's do()). Anything
		// else is unexpected shape, not a specific failure to relay as-is.
		writeJSON(w, http.StatusBadGateway, errorResponse{Error: cerr.Message})
	}
}

// --- static assets -------------------------------------------------------

// staticContentTypes pins Content-Type by extension instead of relying on
// mime.TypeByExtension, whose answer can depend on OS mime registrations.
var staticContentTypes = map[string]string{
	".html": "text/html; charset=utf-8",
	".css":  "text/css; charset=utf-8",
	".js":   "text/javascript; charset=utf-8",
	".mjs":  "text/javascript; charset=utf-8",
	".json": "application/json; charset=utf-8",
	".svg":  "image/svg+xml",
	".ico":  "image/x-icon",
	".png":  "image/png",
	".txt":  "text/plain; charset=utf-8",
}

// serveAsset serves the embedded SPA. Any unknown path also falls back to
// index.html so client-side navigation never 404s on a refresh.
func (s *Server) serveAsset(w http.ResponseWriter, r *http.Request) {
	p := strings.TrimPrefix(path.Clean(r.URL.Path), "/")
	if p == "" || p == "." {
		p = "index.html"
	}

	data, err := fs.ReadFile(s.assets, p)
	if err != nil {
		data, err = fs.ReadFile(s.assets, "index.html")
		if err != nil {
			http.NotFound(w, r)
			return
		}
		p = "index.html"
	}

	ext := path.Ext(p)
	ct, ok := staticContentTypes[ext]
	if !ok {
		if guess := mime.TypeByExtension(ext); guess != "" {
			ct = guess
		} else {
			ct = http.DetectContentType(data)
		}
	}
	w.Header().Set("Content-Type", ct)
	w.Header().Set("Cache-Control", "no-cache")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(data)
}
