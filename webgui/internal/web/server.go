// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200. Originally written for sdrlink (MIT) and relicensed
// here by its author/copyright holder into this GPL-2.0-or-later project.

package web

import (
	"io/fs"
	"mime"
	"net/http"
	"path"
	"strings"
	"time"
)

// Default spectrum engine parameters. fftSize is deliberately modest: 4096
// points at a typical B200 RX rate (say 2 MHz) is a ~2 ms window and a
// ~490 Hz bin spacing before reduction, which is plenty for a waterfall the
// browser then downsamples visually anyway. targetBins is the number of
// bins actually sent over the wire per frame, independent of fftSize, so the
// UI's canvas width doesn't dictate DSP resolution.
const (
	defaultFFTSize          = 4096
	defaultTargetBins       = 1024
	defaultSpectrumInterval = 50 * time.Millisecond
)

// Server implements http.Handler for sdrlink's web GUI: the embedded SPA,
// the /api/... JSON surface, and the /ws/spectrum WebSocket feed. It holds
// no hardware state itself — everything device-related is asked of the
// Backend it was built with.
type Server struct {
	backend Backend
	assets  fs.FS
	mux     *http.ServeMux

	fftSize          int
	targetBins       int
	spectrumInterval time.Duration
}

// Option configures optional Server parameters. Most callers only need
// NewServer(backend); the options exist mainly so tests can shrink the FFT
// size and tick interval for fast, deterministic runs.
type Option func(*Server)

// WithFFTSize sets the number of samples the spectrum engine transforms per
// frame. Rounded up to a power of two.
func WithFFTSize(n int) Option { return func(s *Server) { s.fftSize = n } }

// WithTargetBins sets how many bins are sent to the browser per spectrum
// frame (after reduction from fftSize).
func WithTargetBins(n int) Option { return func(s *Server) { s.targetBins = n } }

// WithSpectrumInterval sets how often each connected /ws/spectrum client is
// considered for a new frame.
func WithSpectrumInterval(d time.Duration) Option { return func(s *Server) { s.spectrumInterval = d } }

// withAssets overrides the embedded frontend filesystem. Unexported: real
// callers always get the embedded build; this exists so assets_test.go can
// exercise serveAsset against a small synthetic fs.FS without depending on
// the exact contents of static/.
func withAssets(fsys fs.FS) Option { return func(s *Server) { s.assets = fsys } }

// NewServer builds a Server backed by the given Backend.
func NewServer(backend Backend, opts ...Option) *Server {
	s := &Server{
		backend:          backend,
		assets:           Assets(),
		fftSize:          defaultFFTSize,
		targetBins:       defaultTargetBins,
		spectrumInterval: defaultSpectrumInterval,
	}
	for _, opt := range opts {
		opt(s)
	}
	if s.fftSize <= 0 {
		s.fftSize = defaultFFTSize
	}
	s.fftSize = nextPow2(s.fftSize)
	if s.targetBins <= 0 {
		s.targetBins = defaultTargetBins
	}
	s.routes()
	return s
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	s.mux.ServeHTTP(w, r)
}

func (s *Server) routes() {
	mux := http.NewServeMux()

	mux.HandleFunc("GET /api/devices", s.handleListDevices)
	mux.HandleFunc("POST /api/devices/open", s.handleOpenDevice)
	mux.HandleFunc("POST /api/devices/close", s.handleCloseDevice)
	mux.HandleFunc("GET /api/caps", s.handleCaps)
	mux.HandleFunc("GET /api/state", s.handleState)
	mux.HandleFunc("GET /api/stats", s.handleStats)
	mux.HandleFunc("GET /api/status", s.handleStatus)

	mux.HandleFunc("POST /api/rx/freq", s.handleHzSetter(func(d Device, hz float64) (float64, error) { return d.SetRxFreq(hz) }))
	mux.HandleFunc("POST /api/tx/freq", s.handleHzSetter(func(d Device, hz float64) (float64, error) { return d.SetTxFreq(hz) }))
	mux.HandleFunc("POST /api/rx/rate", s.handleHzSetter(func(d Device, hz float64) (float64, error) { return d.SetRxRate(hz) }))
	mux.HandleFunc("POST /api/tx/rate", s.handleHzSetter(func(d Device, hz float64) (float64, error) { return d.SetTxRate(hz) }))
	mux.HandleFunc("POST /api/rx/bandwidth", s.handleHzSetter(func(d Device, hz float64) (float64, error) { return d.SetRxBandwidth(hz) }))
	mux.HandleFunc("POST /api/tx/bandwidth", s.handleHzSetter(func(d Device, hz float64) (float64, error) { return d.SetTxBandwidth(hz) }))
	mux.HandleFunc("POST /api/lo_offset", s.handleHzSetter(func(d Device, hz float64) (float64, error) { return d.SetLoOffset(hz) }))

	mux.HandleFunc("POST /api/rx/gain", s.handleDbSetter(func(d Device, db float64) (float64, error) { return d.SetRxGain(db) }))
	mux.HandleFunc("POST /api/tx/gain", s.handleDbSetter(func(d Device, db float64) (float64, error) { return d.SetTxGain(db) }))

	mux.HandleFunc("POST /api/rx/antenna", s.handleNameSetter(func(d Device, name string) (string, error) { return d.SetRxAntenna(name) }))
	mux.HandleFunc("POST /api/tx/antenna", s.handleNameSetter(func(d Device, name string) (string, error) { return d.SetTxAntenna(name) }))

	mux.HandleFunc("POST /api/rx/agc", s.handleBoolSetter(func(d Device, on bool) (bool, error) { return d.SetRxAgc(on) }))
	mux.HandleFunc("POST /api/rx/dc_offset_auto", s.handleBoolSetter(func(d Device, on bool) (bool, error) { return d.SetRxDcOffsetAuto(on) }))
	mux.HandleFunc("POST /api/rx/iq_balance_auto", s.handleBoolSetter(func(d Device, on bool) (bool, error) { return d.SetRxIqBalanceAuto(on) }))

	mux.HandleFunc("POST /api/rx/start", s.handleStartRx)
	mux.HandleFunc("POST /api/rx/stop", s.handleStopRx)

	mux.HandleFunc("GET /ws/spectrum", s.handleSpectrumWS)

	// Catch-all: embedded SPA assets, including "/" -> index.html.
	mux.HandleFunc("GET /", s.serveAsset)

	s.mux = mux
}

// staticContentTypes pins Content-Type by extension instead of relying on
// mime.TypeByExtension, whose answer can depend on OS mime registrations
// (observed to vary on Windows) — the frontend must get the same,
// predictable type every time regardless of host.
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
// index.html so client-side routing (none today, but harmless to allow)
// doesn't 404 on a refresh.
func (s *Server) serveAsset(w http.ResponseWriter, r *http.Request) {
	p := strings.TrimPrefix(path.Clean(r.URL.Path), "/")
	if p == "" || p == "." {
		p = "index.html"
	}

	data, err := fs.ReadFile(s.assets, p)
	if err != nil {
		// SPA fallback for anything under the root that isn't a real asset.
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
