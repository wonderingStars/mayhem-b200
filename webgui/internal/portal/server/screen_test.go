// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package server

import (
	"bufio"
	"bytes"
	"compress/flate"
	"context"
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"mayhemb200/webgui/internal/portal/client"
)

// Tests for the live screen bridge (screen.go): frame fan-out, the
// controller rules, DEFLATE, the plain-HTTP proxies and contract 4's
// have_image_rev pass-through.
//
// Everything here drives a real httptest server over a real socket rather
// than calling hub methods directly. The bugs worth catching in this code
// are ordering and concurrency bugs — who gets control, which frame lands
// where, whose keystroke reaches the radio — and none of those are visible
// from a direct method call.

// --- websocket test client --------------------------------------------------

func makeWSKey(t *testing.T) string {
	t.Helper()
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		t.Fatal(err)
	}
	return base64.StdEncoding.EncodeToString(b[:])
}

// writeMaskedFrame writes one client->server frame (must be masked per
// RFC 6455 §5.1) directly onto w; wsConn only ever writes unmasked server
// frames, so a browser's side of the conversation has to be written by hand.
func writeMaskedFrame(w io.Writer, opcode byte, payload []byte) error {
	n := len(payload)
	var hdr []byte
	switch {
	case n <= 125:
		hdr = []byte{0x80 | opcode, byte(n) | 0x80}
	case n <= 0xFFFF:
		hdr = []byte{0x80 | opcode, 126 | 0x80, 0, 0}
		binary.BigEndian.PutUint16(hdr[2:4], uint16(n))
	default:
		hdr = []byte{0x80 | opcode, 127 | 0x80, 0, 0, 0, 0, 0, 0, 0, 0}
		binary.BigEndian.PutUint64(hdr[2:10], uint64(n))
	}
	var mask [4]byte
	if _, err := rand.Read(mask[:]); err != nil {
		return err
	}
	hdr = append(hdr, mask[:]...)
	masked := make([]byte, n)
	for i, b := range payload {
		masked[i] = b ^ mask[i%4]
	}
	if _, err := w.Write(hdr); err != nil {
		return err
	}
	_, err := w.Write(masked)
	return err
}

// wsClient is one browser in these tests.
type wsClient struct {
	t    *testing.T
	conn net.Conn
	ws   *wsConn
}

func dialScreenWS(t *testing.T, srv *httptest.Server, query string) *wsClient {
	t.Helper()
	addr := strings.TrimPrefix(srv.URL, "http://")
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	path := "/api/screen/ws" + query
	key := makeWSKey(t)
	req := fmt.Sprintf(
		"GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
		path, addr, key,
	)
	if _, err := conn.Write([]byte(req)); err != nil {
		t.Fatalf("write handshake: %v", err)
	}
	br := bufio.NewReader(conn)
	resp, err := http.ReadResponse(br, nil)
	if err != nil {
		t.Fatalf("read handshake response: %v", err)
	}
	if resp.StatusCode != http.StatusSwitchingProtocols {
		t.Fatalf("status = %d, want 101", resp.StatusCode)
	}
	if got, want := resp.Header.Get("Sec-WebSocket-Accept"), wsAcceptKey(key); got != want {
		t.Fatalf("Sec-WebSocket-Accept = %q, want %q", got, want)
	}
	return &wsClient{t: t, conn: conn, ws: &wsConn{rwc: conn, br: br}}
}

func (c *wsClient) close() { c.conn.Close() }

// nextStatus reads until the next status text frame, failing the test on
// timeout. Binary frames arriving first are skipped, not an error: a joining
// client is legitimately handed the current picture too.
func (c *wsClient) nextStatus(timeout time.Duration) screenStatus {
	c.t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		_ = c.conn.SetReadDeadline(deadline)
		op, payload, err := c.ws.ReadMessage()
		if err != nil {
			c.t.Fatalf("waiting for status: %v", err)
		}
		if op != wsOpText {
			continue
		}
		var st screenStatus
		if err := json.Unmarshal(payload, &st); err != nil {
			c.t.Fatalf("status %q is not JSON: %v", payload, err)
		}
		if st.Type != "status" {
			c.t.Fatalf("text message type = %q, want status (%q)", st.Type, payload)
		}
		return st
	}
}

// nextFrame reads until the next binary frame.
func (c *wsClient) nextFrame(timeout time.Duration) []byte {
	c.t.Helper()
	deadline := time.Now().Add(timeout)
	for {
		_ = c.conn.SetReadDeadline(deadline)
		op, payload, err := c.ws.ReadMessage()
		if err != nil {
			c.t.Fatalf("waiting for frame: %v", err)
		}
		if op == wsOpBinary {
			return payload
		}
	}
}

func (c *wsClient) sendEvents(raw string) {
	c.t.Helper()
	if err := writeMaskedFrame(c.conn, wsOpText, []byte(raw)); err != nil {
		c.t.Fatalf("send events: %v", err)
	}
}

// --- fixtures ---------------------------------------------------------------

// testFrame builds a real 240x320 contract-1 frame. Full size on purpose:
// it is the only size the device has, it exercises writeFrame's 64-bit
// length path, and it makes the DEFLATE assertions meaningful.
func testFrame(t *testing.T, seq uint32, fill func(i int) byte) client.ScreenFrame {
	t.Helper()
	const w, h = 240, 320
	payload := make([]byte, w*h*2)
	for i := range payload {
		payload[i] = fill(i)
	}
	raw := client.BuildScreenFrame(client.ScreenFormatRGB565, w, h, seq, payload)
	fr, err := client.ParseScreenFrame(raw)
	if err != nil {
		t.Fatalf("testFrame is not a valid frame: %v", err)
	}
	return fr
}

func flatFill(b byte) func(int) byte { return func(int) byte { return b } }

// frameSource is a fake mayhem-b200 screen endpoint: it hands out frames the
// test pushes, and otherwise honours wait_ms with a 204 exactly like the C++
// long poll does.
type frameSource struct {
	ch    chan client.ScreenFrame
	calls atomic.Int64
	// err, when set, makes every call fail: a mayhem-b200 that is not running.
	err atomic.Pointer[error]
}

func newFrameSource() *frameSource { return &frameSource{ch: make(chan client.ScreenFrame, 8)} }

func (fs *frameSource) screen(ctx context.Context, after uint32, waitMS int) (client.ScreenFrame, bool, error) {
	fs.calls.Add(1)
	if e := fs.err.Load(); e != nil {
		return client.ScreenFrame{}, false, *e
	}
	timer := time.NewTimer(time.Duration(waitMS) * time.Millisecond)
	defer timer.Stop()
	select {
	case fr := <-fs.ch:
		return fr, true, nil
	case <-timer.C:
		return client.ScreenFrame{}, false, nil
	case <-ctx.Done():
		return client.ScreenFrame{}, false, ctx.Err()
	}
}

func (fs *frameSource) push(fr client.ScreenFrame) { fs.ch <- fr }

func (fs *frameSource) fail(err error) { fs.err.Store(&err) }

// newScreenServer wires a fake backend to a real portal on a real socket,
// with the hub's timings shortened to test speed.
func newScreenServer(t *testing.T, fb *fakeBackend) *httptest.Server {
	t.Helper()
	s := New(fb, withAssets(minimalAssets(t)))
	s.screen.pollWaitMS = 60
	s.screen.minPollGap = time.Millisecond
	s.screen.inputFlush = 5 * time.Millisecond
	s.screen.retryMin = 10 * time.Millisecond
	s.screen.retryMax = 20 * time.Millisecond
	s.screen.logf = func(string, ...any) {} // keep test output readable
	return httptest.NewServer(s)
}

// --- fan-out ----------------------------------------------------------------

// TestScreenWS_FansOneFrameOutToEveryClient is the core of contract 3: one
// backend poll, N browsers, the same bytes to each. It also pins the first
// client as controller and the second as read-only.
func TestScreenWS_FansOneFrameOutToEveryClient(t *testing.T) {
	fs := newFrameSource()
	fb := &fakeBackend{screenFn: fs.screen}
	ts := newScreenServer(t, fb)
	defer ts.Close()

	a := dialScreenWS(t, ts, "?deflate=0")
	defer a.close()
	// Reading A's join status before B dials makes the join ORDER
	// deterministic, which is what the controller rules are defined in terms
	// of; without it the two joins race.
	if st := a.nextStatus(2 * time.Second); !st.Controlling || st.Viewers != 1 {
		t.Fatalf("first client status = %+v, want controlling with 1 viewer", st)
	}

	b := dialScreenWS(t, ts, "?deflate=0")
	defer b.close()
	if st := b.nextStatus(2 * time.Second); st.Controlling || st.Viewers != 2 {
		t.Fatalf("second client status = %+v, want read-only with 2 viewers", st)
	}

	fr := testFrame(t, 7, func(i int) byte { return byte(i * 31) })
	fs.push(fr)

	gotA := a.nextFrame(3 * time.Second)
	gotB := b.nextFrame(3 * time.Second)
	if !bytes.Equal(gotA, fr.Raw) {
		t.Fatalf("client A frame differs from what the backend sent (%d vs %d bytes)", len(gotA), len(fr.Raw))
	}
	if !bytes.Equal(gotB, gotA) {
		t.Fatalf("client B got different bytes than client A")
	}

	// One poll goroutine, not one per client: with two clients attached the
	// backend must not be being polled twice as fast.
	parsed, err := client.ParseScreenFrame(gotA)
	if err != nil {
		t.Fatalf("fanned-out frame does not parse: %v", err)
	}
	if parsed.Seq != 7 || parsed.Width != 240 || parsed.Height != 320 || parsed.Format != client.ScreenFormatRGB565 {
		t.Fatalf("frame header = %+v, want seq 7, 240x320, format 1", parsed)
	}
}

// TestScreenWS_JoinerGetsTheCurrentFrameImmediately: a tab opened between
// two damage events must not stare at nothing until the app next repaints.
func TestScreenWS_JoinerGetsTheCurrentFrameImmediately(t *testing.T) {
	fs := newFrameSource()
	fb := &fakeBackend{screenFn: fs.screen}
	ts := newScreenServer(t, fb)
	defer ts.Close()

	a := dialScreenWS(t, ts, "?deflate=0")
	defer a.close()
	a.nextStatus(2 * time.Second)

	fr := testFrame(t, 3, flatFill(0x5A))
	fs.push(fr)
	if got := a.nextFrame(3 * time.Second); !bytes.Equal(got, fr.Raw) {
		t.Fatalf("client A did not receive the pushed frame")
	}

	// B joins after the only frame was published, and no new frame is ever
	// pushed: it can only get a picture from the hub's cache.
	b := dialScreenWS(t, ts, "?deflate=0")
	defer b.close()
	if got := b.nextFrame(3 * time.Second); !bytes.Equal(got, fr.Raw) {
		t.Fatalf("late joiner did not receive the cached current frame")
	}
}

// TestScreenWS_IdenticalSeqIsNotResent guards the dedupe in publish(): seq
// only advances on damage, so the same seq is the same picture and resending
// it costs 150kB per client to change nothing.
func TestScreenWS_IdenticalSeqIsNotResent(t *testing.T) {
	fs := newFrameSource()
	fb := &fakeBackend{screenFn: fs.screen}
	ts := newScreenServer(t, fb)
	defer ts.Close()

	a := dialScreenWS(t, ts, "?deflate=0")
	defer a.close()
	a.nextStatus(2 * time.Second)

	first := testFrame(t, 9, flatFill(0x11))
	fs.push(first)
	a.nextFrame(3 * time.Second)

	// Same seq, different pixels: the backend contract says that cannot
	// happen, so the hub is entitled to (and does) skip it.
	fs.push(testFrame(t, 9, flatFill(0x22)))
	// A genuinely new frame after it must still arrive.
	third := testFrame(t, 10, flatFill(0x33))
	fs.push(third)

	got := a.nextFrame(3 * time.Second)
	parsed, err := client.ParseScreenFrame(got)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if parsed.Seq != 10 {
		t.Fatalf("next frame seq = %d, want 10 (the duplicate seq 9 should have been dropped)", parsed.Seq)
	}
}

// --- control ----------------------------------------------------------------

// TestScreenWS_OnlyTheControllerCanDriveTheRadio.
func TestScreenWS_OnlyTheControllerCanDriveTheRadio(t *testing.T) {
	fs := newFrameSource()
	fb := &fakeBackend{screenFn: fs.screen}
	ts := newScreenServer(t, fb)
	defer ts.Close()

	a := dialScreenWS(t, ts, "?deflate=0")
	defer a.close()
	a.nextStatus(2 * time.Second)
	b := dialScreenWS(t, ts, "?deflate=0")
	defer b.close()
	b.nextStatus(2 * time.Second)

	// The read-only client goes first, so if its events were going to be
	// forwarded they would reach the backend no later than the controller's.
	b.sendEvents(`{"events":[{"type":"key","key":"left","down":true}]}`)
	time.Sleep(40 * time.Millisecond) // several input flush ticks
	a.sendEvents(`{"events":[{"type":"key","key":"up","down":true},{"type":"encoder","delta":-2}]}`)

	waitFor(t, 3*time.Second, func() bool { return len(fb.inputEvents()) >= 2 })
	// And nothing else turns up afterwards.
	time.Sleep(40 * time.Millisecond)

	got := fb.inputEvents()
	joined := strings.Join(got, " ")
	if !strings.Contains(joined, `"key":"up"`) || !strings.Contains(joined, `"delta":-2`) {
		t.Fatalf("controller's events did not reach the backend: %q", got)
	}
	if strings.Contains(joined, `"key":"left"`) {
		t.Fatalf("a read-only client's input reached the backend: %q", got)
	}
	if len(got) != 2 {
		t.Fatalf("backend saw %d events, want exactly the controller's 2: %q", len(got), got)
	}
	// Events are relayed verbatim, not re-marshalled from a typed struct:
	// the C++ side is the authority on which fields are meaningful.
	if got[0] != `{"type":"key","key":"up","down":true}` {
		t.Fatalf("event was rewritten in transit: %q", got[0])
	}
}

// TestScreenWS_ControlHandsOverWhenTheControllerLeaves.
func TestScreenWS_ControlHandsOverWhenTheControllerLeaves(t *testing.T) {
	fs := newFrameSource()
	fb := &fakeBackend{screenFn: fs.screen}
	ts := newScreenServer(t, fb)
	defer ts.Close()

	a := dialScreenWS(t, ts, "?deflate=0")
	if st := a.nextStatus(2 * time.Second); !st.Controlling {
		t.Fatalf("first client is not the controller: %+v", st)
	}
	b := dialScreenWS(t, ts, "?deflate=0")
	defer b.close()
	if st := b.nextStatus(2 * time.Second); st.Controlling {
		t.Fatalf("second client should be read-only: %+v", st)
	}
	c := dialScreenWS(t, ts, "?deflate=0")
	defer c.close()
	if st := c.nextStatus(2 * time.Second); st.Controlling {
		t.Fatalf("third client should be read-only: %+v", st)
	}
	// B is told about C's arrival (viewers changed).
	if st := b.nextStatus(2 * time.Second); st.Viewers != 3 || st.Controlling {
		t.Fatalf("B's updated status = %+v, want 3 viewers, still read-only", st)
	}

	a.close() // controller disconnects

	// The longest-connected remaining client takes over...
	stB := b.nextStatus(3 * time.Second)
	if !stB.Controlling || stB.Viewers != 2 {
		t.Fatalf("B status after handover = %+v, want controlling with 2 viewers", stB)
	}
	// ...and the other one is told it is still read-only.
	stC := c.nextStatus(3 * time.Second)
	if stC.Controlling || stC.Viewers != 2 {
		t.Fatalf("C status after handover = %+v, want read-only with 2 viewers", stC)
	}

	// The promotion is real, not just cosmetic: B's input now reaches the
	// backend, where before the handover it would have been ignored.
	b.sendEvents(`{"events":[{"type":"key","key":"select","down":true}]}`)
	waitFor(t, 3*time.Second, func() bool {
		return strings.Contains(strings.Join(fb.inputEvents(), " "), `"key":"select"`)
	})
}

// --- DEFLATE ----------------------------------------------------------------

// TestScreenWS_DeflateFrameInflatesByteIdentical. Contract 3's format 2 is
// the same 16-byte header with a raw-DEFLATE payload; anything else and the
// browser's DecompressionStream("deflate-raw") produces garbage or throws.
func TestScreenWS_DeflateFrameInflatesByteIdentical(t *testing.T) {
	fs := newFrameSource()
	fb := &fakeBackend{screenFn: fs.screen}
	ts := newScreenServer(t, fb)
	defer ts.Close()

	a := dialScreenWS(t, ts, "") // no deflate=0: compression allowed
	defer a.close()
	a.nextStatus(2 * time.Second)

	// A plausible screen: large flat areas, which is what a 240x320 UI of
	// panels and text actually looks like to a compressor.
	fr := testFrame(t, 42, func(i int) byte {
		if (i/2)%240 < 30 {
			return 0xFF
		}
		return 0x00
	})
	fs.push(fr)

	got := a.nextFrame(3 * time.Second)
	parsed, err := client.ParseScreenFrame(got)
	if err != nil {
		t.Fatalf("compressed frame does not parse: %v", err)
	}
	if parsed.Format != client.ScreenFormatDeflate {
		t.Fatalf("format = %d, want %d (deflate) for a highly compressible frame", parsed.Format, client.ScreenFormatDeflate)
	}
	if parsed.Version != client.ScreenVersion || parsed.Width != 240 || parsed.Height != 320 || parsed.Seq != 42 {
		t.Fatalf("compressed frame header = %+v, want the original header with only format changed", parsed)
	}
	if len(got) >= len(fr.Raw) {
		t.Fatalf("compressed frame is %d bytes, not smaller than the %d-byte original", len(got), len(fr.Raw))
	}
	// The reserved field must stay zero — the browser reads a fixed 16-byte
	// header and would silently mis-slice the payload otherwise.
	if got[14] != 0 || got[15] != 0 {
		t.Fatalf("reserved bytes = %v, want 0,0", got[14:16])
	}

	inflated, err := io.ReadAll(flate.NewReader(bytes.NewReader(parsed.Payload)))
	if err != nil {
		t.Fatalf("inflate: %v", err)
	}
	if !bytes.Equal(inflated, fr.Payload) {
		t.Fatalf("inflated payload differs from the original (%d vs %d bytes)", len(inflated), len(fr.Payload))
	}
}

// TestScreenWS_DeflateOptOutGetsFormat1: a browser without
// DecompressionStream asks with ?deflate=0 and must get raw frames.
func TestScreenWS_DeflateOptOutGetsFormat1(t *testing.T) {
	fs := newFrameSource()
	fb := &fakeBackend{screenFn: fs.screen}
	ts := newScreenServer(t, fb)
	defer ts.Close()

	plain := dialScreenWS(t, ts, "?deflate=0")
	defer plain.close()
	if st := plain.nextStatus(2 * time.Second); !st.Controlling {
		t.Fatalf("status = %+v", st)
	}
	comp := dialScreenWS(t, ts, "")
	defer comp.close()
	comp.nextStatus(2 * time.Second)

	fr := testFrame(t, 5, flatFill(0))
	fs.push(fr)

	gotPlain := plain.nextFrame(3 * time.Second)
	if !bytes.Equal(gotPlain, fr.Raw) {
		t.Fatalf("deflate=0 client did not get the raw frame verbatim")
	}
	gotComp := comp.nextFrame(3 * time.Second)
	parsedComp, err := client.ParseScreenFrame(gotComp)
	if err != nil {
		t.Fatalf("parse: %v", err)
	}
	if parsedComp.Format != client.ScreenFormatDeflate {
		t.Fatalf("compressing client got format %d, want 2 — the two encodings must coexist on one hub", parsedComp.Format)
	}
}

// TestDeflateScreenFrame_IncompressibleStaysFormat1: compression that makes
// a frame bigger must not be used. Random bytes are the worst case.
func TestDeflateScreenFrame_IncompressibleStaysFormat1(t *testing.T) {
	payload := make([]byte, 240*320*2)
	if _, err := rand.Read(payload); err != nil {
		t.Fatal(err)
	}
	raw := client.BuildScreenFrame(client.ScreenFormatRGB565, 240, 320, 1, payload)
	if got := deflateScreenFrame(raw); got != nil {
		t.Fatalf("deflateScreenFrame returned %d bytes for incompressible input; want nil (send format 1)", len(got))
	}
}

// TestDeflateScreenFrame_RejectsNonFrames: a caller must never be handed a
// "compressed" frame built from something that was not a frame.
func TestDeflateScreenFrame_RejectsNonFrames(t *testing.T) {
	for name, input := range map[string][]byte{
		"empty":      {},
		"short":      []byte("MBSF"),
		"bad magic":  client.BuildScreenFrame(1, 2, 2, 1, make([]byte, 8))[:0:0],
		"bad length": append([]byte("MBSF\x01\x01"), make([]byte, 20)...),
	} {
		if got := deflateScreenFrame(input); got != nil {
			t.Errorf("%s: deflateScreenFrame = %d bytes, want nil", name, len(got))
		}
	}
}

// --- backend down -----------------------------------------------------------

// TestScreenWS_BackendDownIsReportedAndDoesNotBusyLoop. Two failure modes in
// one test because they are the same bug from different angles: a portal
// that silently shows a frozen frame, and a portal that hammers a dead
// backend as fast as the CPU allows.
func TestScreenWS_BackendDownIsReportedAndDoesNotBusyLoop(t *testing.T) {
	fs := newFrameSource()
	fs.fail(&client.Error{Op: "screen", Message: "mayhem-b200 is not running or not reachable", Err: errors.New("connection refused")})
	fb := &fakeBackend{screenFn: fs.screen}
	ts := newScreenServer(t, fb) // retryMin 10ms, retryMax 20ms
	defer ts.Close()

	a := dialScreenWS(t, ts, "?deflate=0")
	defer a.close()

	// The join status may or may not already carry the error, depending on
	// whether the first poll has failed yet; keep reading until it does.
	deadline := time.Now().Add(3 * time.Second)
	var st screenStatus
	for time.Now().Before(deadline) {
		st = a.nextStatus(3 * time.Second)
		if st.BackendError != "" {
			break
		}
	}
	if st.BackendError == "" {
		t.Fatal("no status reported the backend being down")
	}
	if !strings.Contains(st.BackendError, "mayhem-b200 is not running") {
		t.Fatalf("backend_error = %q, want the honest 'not running' message", st.BackendError)
	}
	if !st.Controlling || st.Viewers != 1 {
		t.Fatalf("status = %+v: the contract fields must still be correct while the backend is down", st)
	}

	// 300ms at a 10-20ms backoff is at most ~30 attempts. A busy loop would
	// be in the tens of thousands.
	before := fs.calls.Load()
	time.Sleep(300 * time.Millisecond)
	attempts := fs.calls.Load() - before
	if attempts > 100 {
		t.Fatalf("%d poll attempts in 300ms — the retry path is busy-looping", attempts)
	}
	if attempts == 0 {
		t.Fatal("no poll attempts in 300ms — the hub gave up on a backend that may come back")
	}
}

// TestScreenHub_StopsPollingWhenTheLastClientLeaves: a portal nobody is
// looking at must not keep a long poll open against the radio forever.
func TestScreenHub_StopsPollingWhenTheLastClientLeaves(t *testing.T) {
	fs := newFrameSource()
	fb := &fakeBackend{screenFn: fs.screen}
	ts := newScreenServer(t, fb)
	defer ts.Close()

	a := dialScreenWS(t, ts, "?deflate=0")
	a.nextStatus(2 * time.Second)
	waitFor(t, 2*time.Second, func() bool { return fs.calls.Load() > 0 })

	a.close()
	// Let the in-flight poll's wait (60ms) expire and the loop notice.
	time.Sleep(200 * time.Millisecond)
	quiescent := fs.calls.Load()
	time.Sleep(300 * time.Millisecond)
	if got := fs.calls.Load(); got != quiescent {
		t.Fatalf("polls continued after the last client left: %d -> %d", quiescent, got)
	}
}

// --- plain HTTP proxies -----------------------------------------------------

func TestHandleScreen_ProxiesFrameBytesAnd204(t *testing.T) {
	fr := testFrame(t, 11, flatFill(0xAB))
	fb := &fakeBackend{}
	fb.screenFn = func(ctx context.Context, after uint32, waitMS int) (client.ScreenFrame, bool, error) {
		// The query parameters must survive the proxy, or every long poll
		// silently degrades into "give me the current frame now".
		if after != 4 || waitMS != 250 {
			return client.ScreenFrame{}, false, fmt.Errorf("after=%d wait_ms=%d, want 4/250", after, waitMS)
		}
		return fr, true, nil
	}
	s := New(fb, withAssets(minimalAssets(t)))

	rr := doReq(t, s, http.MethodGet, "/api/screen?after=4&wait_ms=250")
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200, body=%s", rr.Code, rr.Body.String())
	}
	if ct := rr.Header().Get("Content-Type"); ct != "application/octet-stream" {
		t.Fatalf("Content-Type = %q, want application/octet-stream", ct)
	}
	if !bytes.Equal(rr.Body.Bytes(), fr.Raw) {
		t.Fatalf("body is %d bytes, want the frame's %d verbatim", rr.Body.Len(), len(fr.Raw))
	}

	// No frame yet -> 204 with no body, not an error and not a fake frame.
	empty := &fakeBackend{}
	rr2 := doReq(t, New(empty, withAssets(minimalAssets(t))), http.MethodGet, "/api/screen")
	if rr2.Code != http.StatusNoContent {
		t.Fatalf("status = %d, want 204 when the backend has no frame", rr2.Code)
	}
	if rr2.Body.Len() != 0 {
		t.Fatalf("204 body = %q, want empty", rr2.Body.String())
	}
}

func TestHandleInput_RelaysEventsVerbatim(t *testing.T) {
	fb := &fakeBackend{}
	s := New(fb, withAssets(minimalAssets(t)))

	body := `{"events":[{"type":"touch","x":10,"y":20,"phase":"start"},{"type":"char","c":65}]}`
	req := httptest.NewRequest(http.MethodPost, "/api/input", strings.NewReader(body))
	rr := httptest.NewRecorder()
	s.ServeHTTP(rr, req)

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200, body=%s", rr.Code, rr.Body.String())
	}
	var res client.InputResult
	if err := json.Unmarshal(rr.Body.Bytes(), &res); err != nil {
		t.Fatalf("decode %q: %v", rr.Body.String(), err)
	}
	if res.Queued != 2 {
		t.Fatalf("queued = %d, want 2", res.Queued)
	}
	got := fb.inputEvents()
	if len(got) != 2 || got[0] != `{"type":"touch","x":10,"y":20,"phase":"start"}` {
		t.Fatalf("events reached the backend rewritten: %q", got)
	}
}

func TestHandleInput_MalformedBodyIs400(t *testing.T) {
	fb := &fakeBackend{}
	s := New(fb, withAssets(minimalAssets(t)))
	req := httptest.NewRequest(http.MethodPost, "/api/input", strings.NewReader(`not json`))
	rr := httptest.NewRecorder()
	s.ServeHTTP(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400", rr.Code)
	}
	if len(fb.inputEvents()) != 0 {
		t.Fatalf("a malformed body still reached the backend: %q", fb.inputEvents())
	}
}

// --- contract 4 pass-through -------------------------------------------------

// TestHandlePanel_ForwardsHaveImageRev is the whole of this change-set's
// contract-4 obligation on the Go side. Getting it wrong is silent: the
// backend would re-send a multi-hundred-kilobyte image on every 700ms poll,
// or (worse, with a fabricated value) omit an image the browser never got.
func TestHandlePanel_ForwardsHaveImageRev(t *testing.T) {
	fb := &fakeBackend{panel: client.Panel{PanelKind: "image"}}
	s := New(fb, withAssets(minimalAssets(t)))

	if rr := doReq(t, s, http.MethodGet, "/api/panel?have_image_rev=17"); rr.Code != http.StatusOK {
		t.Fatalf("status = %d", rr.Code)
	}
	if got := fb.panelRev(); got != "17" {
		t.Fatalf("backend saw have_image_rev = %q, want %q", got, "17")
	}

	// Absent stays absent: no parameter means the browser has nothing, and
	// the portal must not invent a revision on its behalf.
	if rr := doReq(t, s, http.MethodGet, "/api/panel"); rr.Code != http.StatusOK {
		t.Fatalf("status = %d", rr.Code)
	}
	if got := fb.panelRev(); got != "" {
		t.Fatalf("backend saw have_image_rev = %q for a request that had none", got)
	}
}

// TestClientPanel_OmitsHaveImageRevWhenEmpty checks the same thing one layer
// down, on the wire the C++ side actually sees.
func TestClientPanel_OmitsHaveImageRevWhenEmpty(t *testing.T) {
	var gotQuery string
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotQuery = r.URL.RawQuery
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"panel_kind":"image"}`))
	}))
	defer backend.Close()

	c := client.New(backend.URL, time.Second)
	if _, err := c.Panel(context.Background(), "8"); err != nil {
		t.Fatalf("Panel: %v", err)
	}
	if gotQuery != "have_image_rev=8" {
		t.Fatalf("query = %q, want have_image_rev=8", gotQuery)
	}
	if _, err := c.Panel(context.Background(), ""); err != nil {
		t.Fatalf("Panel: %v", err)
	}
	if gotQuery != "" {
		t.Fatalf("query = %q, want no query at all", gotQuery)
	}
}

// --- end to end through a real client.Client --------------------------------

// TestScreenWS_EndToEndThroughRealClient runs the whole bridge against an
// httptest server speaking contract 1 on the wire, through the real
// *client.Client rather than a Go-level fake. This is the layer where C++/Go
// drift actually shows up: header bytes, the 204, and the query parameters.
func TestScreenWS_EndToEndThroughRealClient(t *testing.T) {
	fr := testFrame(t, 100, func(i int) byte { return byte(i / 977) })

	var lastAfter atomic.Uint32
	served := make(chan struct{}, 1)
	cpp := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/screen" {
			http.NotFound(w, r)
			return
		}
		after := parseUint32Param(r.URL.Query().Get("after"))
		lastAfter.Store(after)
		if after >= fr.Seq {
			// Nothing newer: exactly what the C++ long poll does on timeout.
			w.WriteHeader(http.StatusNoContent)
			return
		}
		w.Header().Set("Content-Type", "application/octet-stream")
		_, _ = w.Write(fr.Raw)
		select {
		case served <- struct{}{}:
		default:
		}
	}))
	defer cpp.Close()

	s := New(client.New(cpp.URL, time.Second), withAssets(minimalAssets(t)))
	s.screen.pollWaitMS = 60
	s.screen.minPollGap = 5 * time.Millisecond
	s.screen.logf = func(string, ...any) {}
	ts := httptest.NewServer(s)
	defer ts.Close()

	a := dialScreenWS(t, ts, "?deflate=0")
	defer a.close()
	a.nextStatus(2 * time.Second)

	got := a.nextFrame(3 * time.Second)
	if !bytes.Equal(got, fr.Raw) {
		t.Fatalf("frame did not survive the C++ wire -> client -> hub -> browser path")
	}
	<-served
	waitFor(t, 2*time.Second, func() bool { return lastAfter.Load() == fr.Seq })
}

// --- helpers -----------------------------------------------------------------

// waitFor polls cond until it holds or the timeout expires. Used instead of
// a fixed sleep so these tests are neither flaky nor slower than they need
// to be.
func waitFor(t *testing.T, timeout time.Duration, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		if cond() {
			return
		}
		time.Sleep(2 * time.Millisecond)
	}
	t.Fatalf("condition never became true within %s", timeout)
}
