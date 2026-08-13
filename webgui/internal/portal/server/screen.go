// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package server

import (
	"bytes"
	"compress/flate"
	"context"
	"encoding/json"
	"errors"
	"io"
	"log"
	"net/http"
	"sort"
	"strconv"
	"sync"
	"time"

	"mayhemb200/webgui/internal/portal/client"
)

// The live screen bridge: mayhem-b200's 240x320 framebuffer in a browser
// tab, with the browser's keyboard/mouse/touch going back the other way.
//
// Why this exists at all: every one of the ~103 apps draws into that one
// framebuffer and every key, encoder detent and touch already funnels
// through app::EventDispatcher::dispatch. Mirroring those two things is
// therefore the universal path — it works for an app nobody has written a
// structured panel for, because the thing on screen IS the app.
//
// Shape of the plumbing:
//
//	   C++ /api/screen  --long poll-->  screenHub.pollLoop  --fan out-->  N browsers
//	   C++ /api/input   <--batch POST--  screenHub.inputLoop  <--events--  controller
//
// One poll goroutine serves every connected browser: the backend is a single
// 240x320 display, so N sockets asking it for the same frame N times would
// be N times the work for identical bytes. The hub only runs while at least
// one browser is attached (see join/leave) — a portal nobody is looking at
// must not keep a long poll open against the radio forever.
//
// CONTRACT 3 (frozen) — browser WebSocket, Go <-> browser, at /api/screen/ws:
//
//	server->client binary: exactly the contract-1 MBSF frame, except format
//	  MAY be 2 = the same 16-byte header followed by a raw-DEFLATE payload.
//	server->client text:   {"type":"status","controlling":bool,"viewers":N}
//	  on join and whenever it changes.
//	client->server text:   {"events":[...]} — contract 2's schema. Ignored,
//	  not an error, from a client that is not the controller.
//
// Control: the first client is the controller, later clients are read-only,
// and when the controller disconnects the longest-connected remaining client
// takes over and is told so with a fresh status message.
//
// TRUST BOUNDARY: this socket carries INPUT, and nothing authenticates it.
// wsUpgrade does not check Origin, and CORS does not apply to WebSockets, so
// any page in a user's browser can open it and drive the radio. That matches
// the posture the C++ side already declares for its own API ("no
// authentication, LAN trust boundary", remote_server.hpp) — but it is a
// materially bigger deal for a socket that presses buttons than for one that
// streams spectrum. Flagged deliberately rather than silently inherited.

const (
	// screenPollWaitMS is how long each long poll asks the backend to hold
	// the request open. Contract 1 caps this at 10000; 5s is comfortably
	// inside that while keeping a wedged connection from lingering.
	screenPollWaitMS = 5000

	// screenMinPollGap is a floor on how fast the poll loop may go round
	// when the backend answers instantly. A backend that returns 204
	// immediately despite wait_ms (misconfigured, or older than contract 1)
	// would otherwise spin this goroutine at full tilt.
	screenMinPollGap = 25 * time.Millisecond

	// screenResyncTimeouts is how many consecutive 204s it takes before the
	// poll loop re-asks from seq 0. If mayhem-b200 restarts, its seq counter
	// restarts too — a hub still asking for "> 4000" would then wait
	// forever for a backend that is only up to 12. Re-syncing costs one
	// duplicate frame at worst (publish() drops it by seq).
	screenResyncTimeouts = 3

	// screenInputFlush is the input batching period. One POST per ~16ms
	// (roughly the UI thread's own frame budget) instead of one per pointer
	// move, without adding latency a human can feel.
	screenInputFlush = 16 * time.Millisecond

	// screenInputQueueMax bounds the pending input batch, so a backend that
	// has stopped accepting POSTs cannot make the hub grow without limit.
	// Oldest events are dropped first — the same choice the C++ window layer
	// makes (window.cpp), for the same reason: the most recent intent is the
	// one worth keeping.
	screenInputQueueMax = 512

	// Reconnect backoff for a backend that is down. Never a busy loop, never
	// so slow that a restarted mayhem-b200 takes ages to reappear.
	screenRetryMin = 250 * time.Millisecond
	screenRetryMax = 3 * time.Second

	// screenCtrlQueueMax bounds queued text (status) messages per client.
	screenCtrlQueueMax = 16

	// screenReleaseTimeout bounds the one synchronous POST this package
	// makes: the key releases sent when the LAST client leaves, after the
	// input loop has already been stopped. Short, because nothing waits on
	// it but the departing connection's own goroutine, and a wedged backend
	// must not hold that open.
	screenReleaseTimeout = 2 * time.Second
)

// screenStatus is contract 3's server->client status message.
//
// BackendError is an addition, not a contract change: it is omitted entirely
// when mayhem-b200 is reachable, so a healthy status message is byte-for-byte
// the shape contract 3 specifies. It exists because a browser staring at a
// frozen last frame deserves to be told the radio went away rather than left
// to guess — "absent data stays absent" cuts both ways.
type screenStatus struct {
	Type         string `json:"type"`
	Controlling  bool   `json:"controlling"`
	Viewers      int    `json:"viewers"`
	BackendError string `json:"backend_error,omitempty"`
}

// screenClient is one attached browser.
//
// Frames and control messages are handed to a per-client writer goroutine
// rather than written inline by the hub: a browser on a slow link must not
// be able to stall the fan-out to every other browser, and the hub's own
// mutex must never be held across a socket write.
type screenClient struct {
	id      uint64
	conn    *wsConn
	deflate bool

	// held is the set of key names this client has pressed and not released,
	// so a socket that dies mid-press does not leave the key down on the
	// device. Read and written only by the hub under h.mu.
	held map[string]bool

	mu   sync.Mutex
	next []byte   // latest frame not yet written; superseded frames are dropped
	ctrl [][]byte // queued text messages, in order

	wake     chan struct{}
	done     chan struct{}
	doneOnce sync.Once
}

func newScreenClient(id uint64, conn *wsConn, deflate bool) *screenClient {
	return &screenClient{
		id:      id,
		conn:    conn,
		deflate: deflate,
		held:    map[string]bool{},
		wake:    make(chan struct{}, 1),
		done:    make(chan struct{}),
	}
}

func (c *screenClient) close() {
	c.doneOnce.Do(func() { close(c.done) })
}

func (c *screenClient) signal() {
	select {
	case c.wake <- struct{}{}:
	default: // already pending; the writer will see whatever is queued
	}
}

// sendFrame queues a frame, replacing any frame not yet written. A screen
// mirror gains nothing from delivering a stale frame behind a newer one —
// the newer one overwrites every pixel the older one would have drawn.
func (c *screenClient) sendFrame(b []byte) {
	if len(b) == 0 {
		return
	}
	c.mu.Lock()
	c.next = b
	c.mu.Unlock()
	c.signal()
}

// sendCtrl queues a text message. Unlike frames these are not coalesced —
// each one reports a distinct state change — but the queue is still bounded
// against a client that never reads.
func (c *screenClient) sendCtrl(b []byte) {
	c.mu.Lock()
	if len(c.ctrl) >= screenCtrlQueueMax {
		c.ctrl = c.ctrl[1:]
	}
	c.ctrl = append(c.ctrl, b)
	c.mu.Unlock()
	c.signal()
}

// writeLoop is the only goroutine that writes data frames to this client.
func (c *screenClient) writeLoop() {
	for {
		select {
		case <-c.done:
			return
		case <-c.wake:
		}
		for {
			c.mu.Lock()
			frame := c.next
			ctrl := c.ctrl
			c.next, c.ctrl = nil, nil
			c.mu.Unlock()
			if frame == nil && ctrl == nil {
				break
			}
			// Control first: a status change describes the world the next
			// frame arrives in, and it is tiny.
			for _, m := range ctrl {
				if err := c.conn.writeFrame(wsOpText, m); err != nil {
					c.close()
					return
				}
			}
			if frame != nil {
				if err := c.conn.writeFrame(wsOpBinary, frame); err != nil {
					c.close()
					return
				}
			}
		}
	}
}

// screenHub owns the single backend poll, the client set, and the input
// batch. Everything guarded by mu is small and touched briefly; no backend
// call, socket write or compression ever happens with mu held.
type screenHub struct {
	backend Backend
	logf    func(format string, args ...any)

	// Tunables, as fields rather than constants so tests can run the loops
	// at test speed instead of screen speed.
	pollWaitMS int
	inputFlush time.Duration
	retryMin   time.Duration
	retryMax   time.Duration
	minPollGap time.Duration

	mu      sync.Mutex
	clients []*screenClient // join order; clients[0] is the controller
	nextID  uint64
	cancel  context.CancelFunc

	haveFrame  bool
	lastSeq    uint32
	lastRaw    []byte // format 1, exactly as the backend sent it
	lastDef    []byte // format 2 of the same frame, or nil if not worth sending
	lastDefOK  bool   // whether lastDef has been computed for lastRaw
	backendErr string

	inputQ []json.RawMessage
}

func newScreenHub(backend Backend) *screenHub {
	return &screenHub{
		backend:    backend,
		logf:       log.Printf,
		pollWaitMS: screenPollWaitMS,
		inputFlush: screenInputFlush,
		retryMin:   screenRetryMin,
		retryMax:   screenRetryMax,
		minPollGap: screenMinPollGap,
	}
}

// --- membership -------------------------------------------------------------

// join registers a client, starting the backend loops if it is the first,
// and hands it the current frame (if any) plus its status immediately —
// a tab that opens between two damage events should not stare at nothing
// until the app next repaints.
func (h *screenHub) join(conn *wsConn, deflate bool) *screenClient {
	h.mu.Lock()
	h.nextID++
	c := newScreenClient(h.nextID, conn, deflate)
	h.clients = append(h.clients, c)
	if len(h.clients) == 1 {
		ctx, cancel := context.WithCancel(context.Background())
		h.cancel = cancel
		go h.pollLoop(ctx)
		go h.inputLoop(ctx)
	}
	if h.haveFrame {
		c.sendFrame(h.frameBytesLocked(deflate))
	}
	h.broadcastStatusLocked()
	h.mu.Unlock()
	return c
}

// leave removes a client and, if it was the controller, promotes the
// longest-connected client still attached. The last one out stops the
// backend loops.
func (h *screenHub) leave(c *screenClient) {
	var orphanedReleases []json.RawMessage

	h.mu.Lock()
	for i, other := range h.clients {
		if other == c {
			h.clients = append(h.clients[:i:i], h.clients[i+1:]...)
			break
		}
	}

	releases := h.heldKeyReleasesLocked(c)
	if len(h.clients) == 0 && h.cancel != nil {
		h.cancel()
		h.cancel = nil
		// Drop input nobody can deliver any more rather than replaying it
		// at whoever connects next.
		h.inputQ = nil
		// The key releases are the exception, and are sent below instead:
		// they do not carry an intent forward, they undo state this client
		// left behind on the device — and the loop that would have flushed
		// them is the one being stopped here. This is also the case that
		// matters most, since one tab closing mid-press IS the last client
		// leaving.
		orphanedReleases = releases
	} else {
		h.inputQ = append(h.inputQ, releases...)
	}

	// Whoever is now clients[0] may have just been promoted, and the viewer
	// count changed for everyone regardless.
	h.broadcastStatusLocked()
	h.mu.Unlock()
	c.close()

	if len(orphanedReleases) > 0 {
		// Bounded, and off the hub lock: leave() runs on the departing
		// client's own goroutine, so waiting here delays nothing else.
		ctx, cancel := context.WithTimeout(context.Background(), screenReleaseTimeout)
		defer cancel()
		if _, err := h.backend.Input(ctx, orphanedReleases); err != nil {
			h.logf("portal: could not release %d held key(s) after the last client left: %v",
				len(orphanedReleases), err)
		}
	}
}

// isController reports whether c currently holds control.
func (h *screenHub) isController(c *screenClient) bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.clients) > 0 && h.clients[0] == c
}

// broadcastStatusLocked sends every client its own view of the status.
// Caller holds h.mu; sendCtrl only takes the client's own lock, so there is
// no lock-order edge back into the hub.
func (h *screenHub) broadcastStatusLocked() {
	viewers := len(h.clients)
	for i, c := range h.clients {
		msg, err := json.Marshal(screenStatus{
			Type:         "status",
			Controlling:  i == 0,
			Viewers:      viewers,
			BackendError: h.backendErr,
		})
		if err != nil {
			continue // screenStatus is plain data; marshalling it cannot fail
		}
		c.sendCtrl(msg)
	}
}

// setBackendError records a change in backend reachability and tells every
// client. It returns true when the state actually changed, which is also the
// only time it logs — a backend that is down for an hour must not fill the
// terminal with one line per retry.
func (h *screenHub) setBackendError(msg string) bool {
	h.mu.Lock()
	if h.backendErr == msg {
		h.mu.Unlock()
		return false
	}
	h.backendErr = msg
	h.broadcastStatusLocked()
	h.mu.Unlock()
	return true
}

// --- frame plumbing ---------------------------------------------------------

// frameBytesLocked returns the current frame in the encoding this client
// asked for, computing the compressed form on demand. Caller holds h.mu.
func (h *screenHub) frameBytesLocked(deflate bool) []byte {
	if !deflate {
		return h.lastRaw
	}
	if !h.lastDefOK {
		h.lastDef = deflateScreenFrame(h.lastRaw)
		h.lastDefOK = true
	}
	if h.lastDef == nil {
		return h.lastRaw
	}
	return h.lastDef
}

// publish stores a newly polled frame and fans it out.
func (h *screenHub) publish(fr client.ScreenFrame) {
	h.mu.Lock()
	dup := h.haveFrame && h.lastSeq == fr.Seq
	wantDeflate := false
	for _, c := range h.clients {
		if c.deflate {
			wantDeflate = true
			break
		}
	}
	h.mu.Unlock()

	if dup {
		// seq only advances when the display reported damage, so an
		// identical seq is the identical picture. Sending it again would
		// cost 150kB per client to change nothing.
		return
	}

	// Compress outside the lock: ~150kB of DEFLATE is not something to do
	// while every other goroutine waits to join or leave.
	var def []byte
	if wantDeflate {
		def = deflateScreenFrame(fr.Raw)
	}

	h.mu.Lock()
	h.haveFrame = true
	h.lastSeq = fr.Seq
	h.lastRaw = fr.Raw
	h.lastDef = def
	h.lastDefOK = wantDeflate
	for _, c := range h.clients {
		c.sendFrame(h.frameBytesLocked(c.deflate))
	}
	h.mu.Unlock()
}

// flateWriters recycles DEFLATE writers; one frame's worth of window state
// is ~64kB of allocation that would otherwise be made 30 times a second.
var flateWriters = sync.Pool{
	New: func() any {
		w, err := flate.NewWriter(io.Discard, flate.BestSpeed)
		if err != nil {
			// flate.NewWriter only errors on an out-of-range level, and
			// BestSpeed is in range by construction.
			panic("portal: flate.NewWriter(BestSpeed): " + err.Error())
		}
		return w
	},
}

// deflateScreenFrame re-headers a format-1 frame as format 2 with a
// raw-DEFLATE payload (what DecompressionStream("deflate-raw") expects on
// the browser side). It returns nil — meaning "send format 1 as-is" — when
// the frame is not a well-formed format-1 frame or when compression did not
// actually make it smaller. BestSpeed rather than BestCompression: this runs
// per frame on a live mirror, and a screen of flat UI panels is already
// mostly runs of identical pixels.
func deflateScreenFrame(raw []byte) []byte {
	fr, err := client.ParseScreenFrame(raw)
	if err != nil || fr.Format != client.ScreenFormatRGB565 {
		return nil
	}
	var buf bytes.Buffer
	buf.Grow(len(fr.Payload) / 4)
	w, _ := flateWriters.Get().(*flate.Writer)
	w.Reset(&buf)
	if _, err := w.Write(fr.Payload); err != nil {
		flateWriters.Put(w)
		return nil
	}
	if err := w.Close(); err != nil {
		flateWriters.Put(w)
		return nil
	}
	flateWriters.Put(w)
	if buf.Len() >= len(fr.Payload) {
		return nil // compression made it bigger; not worth the client's CPU
	}
	return client.BuildScreenFrame(client.ScreenFormatDeflate, fr.Width, fr.Height, fr.Seq, buf.Bytes())
}

// --- backend loops ----------------------------------------------------------

// pollLoop is the single long poll against contract 1, running only while at
// least one browser is attached.
func (h *screenHub) pollLoop(ctx context.Context) {
	h.mu.Lock()
	after := h.lastSeq
	h.mu.Unlock()

	backoff := h.retryMin
	timeouts := 0

	for ctx.Err() == nil {
		start := time.Now()
		fr, ok, err := h.backend.Screen(ctx, after, h.pollWaitMS)
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			if h.setBackendError(backendErrorMessage(err)) {
				h.logf("portal: screen stream: %v", err)
			}
			if !sleepCtx(ctx, backoff) {
				return
			}
			backoff *= 2
			if backoff > h.retryMax {
				backoff = h.retryMax
			}
			continue
		}
		backoff = h.retryMin
		if h.setBackendError("") {
			h.logf("portal: screen stream: mayhem-b200 is answering again")
		}

		if !ok {
			// 204: no frame newer than `after` within the wait window.
			timeouts++
			if timeouts >= screenResyncTimeouts {
				timeouts = 0
				after = 0
			}
			if !h.pace(ctx, start) {
				return
			}
			continue
		}
		timeouts = 0
		after = fr.Seq
		h.publish(fr)
		if !h.pace(ctx, start) {
			return
		}
	}
}

// pace enforces screenMinPollGap over one loop iteration, so a backend that
// answers instantly cannot turn the poll loop into a spin. Returns false if
// the context ended while waiting.
func (h *screenHub) pace(ctx context.Context, start time.Time) bool {
	if h.minPollGap <= 0 {
		return ctx.Err() == nil
	}
	if elapsed := time.Since(start); elapsed < h.minPollGap {
		return sleepCtx(ctx, h.minPollGap-elapsed)
	}
	return ctx.Err() == nil
}

// sleepCtx waits for d, returning false if ctx ended first.
func sleepCtx(ctx context.Context, d time.Duration) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-t.C:
		return true
	}
}

// queueInput appends events from the controlling client to the pending
// batch. Returns how many were dropped for want of room.
func (h *screenHub) queueInput(events []json.RawMessage) int {
	if len(events) == 0 {
		return 0
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	h.inputQ = append(h.inputQ, events...)
	dropped := 0
	if over := len(h.inputQ) - screenInputQueueMax; over > 0 {
		h.inputQ = append(h.inputQ[:0:0], h.inputQ[over:]...)
		dropped = over
	}
	return dropped
}

// inputLoop drains the pending batch into POST /api/input on a fixed tick.
func (h *screenHub) inputLoop(ctx context.Context) {
	ticker := time.NewTicker(h.inputFlush)
	defer ticker.Stop()
	loggedFailure := false
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}

		h.mu.Lock()
		batch := h.inputQ
		h.inputQ = nil
		h.mu.Unlock()
		if len(batch) == 0 {
			continue
		}

		if _, err := h.backend.Input(ctx, batch); err != nil {
			if ctx.Err() != nil {
				return
			}
			// Input is not replayed: by the time a wedged backend recovers,
			// a stale keypress is a wrong keypress. Log the first failure of
			// a run only — the poll loop is already reporting reachability.
			if !loggedFailure {
				loggedFailure = true
				h.logf("portal: input to mayhem-b200 failed, dropping %d event(s): %v", len(batch), err)
			}
			continue
		}
		loggedFailure = false
	}
}

// backendErrorMessage renders a backend failure for a browser: the same
// distinction writeBackendError makes, in one line of text.
func backendErrorMessage(err error) string {
	var cerr *client.Error
	if errors.As(err, &cerr) {
		if cerr.Unavailable() {
			return "mayhem-b200 is not running"
		}
		return cerr.Message
	}
	return err.Error()
}

// --- HTTP handlers ----------------------------------------------------------

// handleScreenWS is contract 3's endpoint.
//
// Query parameter deflate=0 opts a client out of format 2. That is not a
// contract change — contract 3 governs the messages on the socket, not the
// URL — and it is how a browser without DecompressionStream("deflate-raw")
// asks for frames it can actually decode, rather than silently rendering
// nothing.
func (s *Server) handleScreenWS(w http.ResponseWriter, r *http.Request) {
	conn, err := wsUpgrade(w, r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	defer conn.Close()

	deflate := r.URL.Query().Get("deflate") != "0"
	c := s.screen.join(conn, deflate)
	go c.writeLoop()
	defer s.screen.leave(c)

	// The read side owns this goroutine: it is also how a disconnect is
	// noticed, which is what triggers control handover.
	for {
		opcode, payload, err := conn.ReadMessage()
		if err != nil {
			return
		}
		switch opcode {
		case wsOpClose:
			return
		case wsOpPing:
			if err := conn.writeFrame(wsOpPong, payload); err != nil {
				return
			}
		case wsOpText:
			s.screen.handleClientText(c, payload)
		}
	}
}

// handleClientText applies one client->server text message. Anything that
// isn't a well-formed input batch from the controlling client is ignored,
// deliberately without an error: a read-only viewer whose browser sends a
// keystroke has not done anything wrong, and telling it off on the socket
// would only teach the page to treat control as an error condition.
func (h *screenHub) handleClientText(c *screenClient, payload []byte) {
	if !h.isController(c) {
		return
	}
	var req struct {
		Events []json.RawMessage `json:"events"`
	}
	if err := json.Unmarshal(payload, &req); err != nil {
		return
	}
	h.noteHeldKeys(c, req.Events)
	h.queueInput(req.Events)
}

// screenKeyEvent is the only part of an event this hub ever looks at. Events
// are forwarded as the raw bytes the browser sent (never re-marshalled), so
// decoding one here cannot change what reaches mayhem-b200 — this is
// observation, not translation.
type screenKeyEvent struct {
	Type string `json:"type"`
	Key  string `json:"key"`
	Down bool   `json:"down"`
}

// noteHeldKeys tracks which keys the controller has pressed and not released.
//
// The device's long-press timer is fed by both transitions, so a key-down with
// no matching key-up leaves ui::key_is_long_pressed() true for that key until
// someone presses and releases it again. The browser releases its own keys on
// blur, pagehide and unmount, but it cannot send anything down a socket that
// has already dropped — and an abrupt disconnect is exactly when a key is most
// likely to be down. So the release has to come from this side.
func (h *screenHub) noteHeldKeys(c *screenClient, events []json.RawMessage) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for _, raw := range events {
		var ev screenKeyEvent
		if err := json.Unmarshal(raw, &ev); err != nil || ev.Type != "key" || ev.Key == "" {
			continue
		}
		if ev.Down {
			c.held[ev.Key] = true
		} else {
			delete(c.held, ev.Key)
		}
	}
}

// heldKeyReleasesLocked returns a key-up event for everything c still holds,
// and forgets them. Caller holds h.mu.
func (h *screenHub) heldKeyReleasesLocked(c *screenClient) []json.RawMessage {
	if len(c.held) == 0 {
		return nil
	}
	// Sorted so the batch is deterministic, which is what makes it testable;
	// the device does not care about the order of releases for distinct keys.
	keys := make([]string, 0, len(c.held))
	for k := range c.held {
		keys = append(keys, k)
	}
	sort.Strings(keys)

	out := make([]json.RawMessage, 0, len(keys))
	for _, k := range keys {
		raw, err := json.Marshal(screenKeyEvent{Type: "key", Key: k})
		if err != nil {
			continue // plain data; marshalling it cannot fail
		}
		out = append(out, json.RawMessage(raw))
	}
	c.held = map[string]bool{}
	return out
}

// handleScreen is the plain-HTTP half of contract 1, proxied through
// unchanged: same query parameters, same binary body, same 204. It exists
// for clients that cannot hold a WebSocket open (and for debugging the
// stream with curl), and it does not go through the hub — a one-shot fetch
// has no fan-out to share.
func (s *Server) handleScreen(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	after := parseUint32Param(q.Get("after"))
	waitMS := parseIntParam(q.Get("wait_ms"))

	fr, ok, err := s.backend.Screen(r.Context(), after, waitMS)
	if err != nil {
		s.writeBackendError(w, err)
		return
	}
	if !ok {
		w.WriteHeader(http.StatusNoContent)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write(fr.Raw)
}

// handleInput is the plain-HTTP half of contract 2. The event objects are
// relayed exactly as received (see client/screen.go for why the portal does
// not parse them); only the envelope is validated.
func (s *Server) handleInput(w http.ResponseWriter, r *http.Request) {
	const maxInputBody = 1 << 20
	body, err := io.ReadAll(io.LimitReader(r.Body, maxInputBody))
	if err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: "could not read request body"})
		return
	}
	var req struct {
		Events []json.RawMessage `json:"events"`
	}
	if err := json.Unmarshal(body, &req); err != nil {
		writeJSON(w, http.StatusBadRequest, errorResponse{Error: `body must be {"events":[...]}`})
		return
	}
	res, err := s.backend.Input(r.Context(), req.Events)
	if err != nil {
		s.writeBackendError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, res)
}

func parseUint32Param(s string) uint32 {
	if s == "" {
		return 0
	}
	v, err := strconv.ParseUint(s, 10, 32)
	if err != nil {
		return 0
	}
	return uint32(v)
}

func parseIntParam(s string) int {
	if s == "" {
		return 0
	}
	v, err := strconv.Atoi(s)
	if err != nil {
		return 0
	}
	return v
}
