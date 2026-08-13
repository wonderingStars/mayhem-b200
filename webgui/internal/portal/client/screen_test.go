// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package client

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// Wire-level tests for contracts 1 and 2. These assert against literal
// bytes and literal query strings on purpose: this file and the C++
// src/remote/ side are two independent implementations of one frozen
// contract, and a test that only checks "it round-trips through my own
// encoder" would stay green through any amount of shared drift.

func sampleFrame(seq uint32, w, h uint16) []byte {
	payload := make([]byte, int(w)*int(h)*2)
	for i := range payload {
		payload[i] = byte(i * 7)
	}
	return BuildScreenFrame(ScreenFormatRGB565, w, h, seq, payload)
}

// TestBuildScreenFrame_ByteLayout pins contract 1's header field by field.
func TestBuildScreenFrame_ByteLayout(t *testing.T) {
	raw := BuildScreenFrame(ScreenFormatRGB565, 240, 320, 0x01020304, make([]byte, 240*320*2))
	if len(raw) != 16+240*320*2 {
		t.Fatalf("len = %d, want %d", len(raw), 16+240*320*2)
	}
	want := []byte{
		'M', 'B', 'S', 'F', // 0..3  magic
		0x01,       // 4     version
		0x01,       // 5     format
		0xF0, 0x00, // 6..7  width 240 LE
		0x40, 0x01, // 8..9  height 320 LE
		0x04, 0x03, 0x02, 0x01, // 10..13 seq LE
		0x00, 0x00, // 14..15 reserved
	}
	if !bytes.Equal(raw[:16], want) {
		t.Fatalf("header = % x\nwant     % x", raw[:16], want)
	}
}

func TestParseScreenFrame_RoundTrip(t *testing.T) {
	raw := sampleFrame(9, 240, 320)
	fr, err := ParseScreenFrame(raw)
	if err != nil {
		t.Fatalf("ParseScreenFrame: %v", err)
	}
	if fr.Version != ScreenVersion || fr.Format != ScreenFormatRGB565 {
		t.Fatalf("version/format = %d/%d", fr.Version, fr.Format)
	}
	if fr.Width != 240 || fr.Height != 320 || fr.Seq != 9 {
		t.Fatalf("frame = %dx%d seq %d", fr.Width, fr.Height, fr.Seq)
	}
	if len(fr.Payload) != 240*320*2 {
		t.Fatalf("payload = %d bytes, want %d", len(fr.Payload), 240*320*2)
	}
	if !bytes.Equal(fr.Raw, raw) {
		t.Fatal("Raw is not the bytes that came in — the WebSocket fan-out forwards it verbatim")
	}
}

// TestParseScreenFrame_RejectsMalformed: a frame this build does not fully
// understand must be an error, never a best-effort render. Putting garbage
// on screen and calling it a picture is the exact failure this project keeps
// getting bitten by.
func TestParseScreenFrame_RejectsMalformed(t *testing.T) {
	good := sampleFrame(1, 4, 4)
	cases := map[string][]byte{
		"empty":           {},
		"header only":     good[:15],
		"wrong magic":     append([]byte("XBSF"), good[4:]...),
		"future version":  append([]byte("MBSF\x02\x01"), good[6:]...),
		"unknown format":  append([]byte("MBSF\x01\x09"), good[6:]...),
		"payload short":   good[:len(good)-2],
		"payload long":    append(append([]byte{}, good...), 0, 0),
		"header no bytes": []byte("MBSF\x01\x01\x04\x00\x04\x00\x01\x00\x00\x00\x00\x00"),
	}
	for name, in := range cases {
		if _, err := ParseScreenFrame(in); err == nil {
			t.Errorf("%s: ParseScreenFrame accepted a malformed frame", name)
		}
	}
	// Format 2 payloads cannot be length-checked (that is the inflater's
	// job) and must be accepted.
	def := BuildScreenFrame(ScreenFormatDeflate, 240, 320, 3, []byte{0x01, 0x02, 0x03})
	if _, err := ParseScreenFrame(def); err != nil {
		t.Errorf("ParseScreenFrame rejected a valid format-2 frame: %v", err)
	}
}

func TestScreen_QueryParametersAndBody(t *testing.T) {
	raw := sampleFrame(12, 240, 320)
	var gotQuery, gotAccept string
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotQuery = r.URL.RawQuery
		gotAccept = r.Header.Get("Accept")
		w.Header().Set("Content-Type", "application/octet-stream")
		_, _ = w.Write(raw)
	}))
	defer ts.Close()

	c := New(ts.URL, time.Second)
	fr, ok, err := c.Screen(context.Background(), 5, 250)
	if err != nil || !ok {
		t.Fatalf("Screen: ok=%v err=%v", ok, err)
	}
	if gotQuery != "after=5&wait_ms=250" {
		t.Fatalf("query = %q, want after=5&wait_ms=250", gotQuery)
	}
	if gotAccept != "application/octet-stream" {
		t.Fatalf("Accept = %q", gotAccept)
	}
	if fr.Seq != 12 || !bytes.Equal(fr.Raw, raw) {
		t.Fatalf("frame = seq %d, %d bytes", fr.Seq, len(fr.Raw))
	}

	// waitMS <= 0 with after == 0 is "the current frame, now": no query at
	// all, which is the bare form contract 1 documents.
	if _, _, err := c.Screen(context.Background(), 0, 0); err != nil {
		t.Fatalf("Screen: %v", err)
	}
	if gotQuery != "" {
		t.Fatalf("query = %q, want none for a bare current-frame request", gotQuery)
	}
}

// TestScreen_WaitIsCappedAtTheContractMaximum: contract 1 caps wait_ms at
// 10000. Sending more is not the caller's mistake to make on the wire.
func TestScreen_WaitIsCappedAtTheContractMaximum(t *testing.T) {
	var gotWait string
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		gotWait = r.URL.Query().Get("wait_ms")
		w.WriteHeader(http.StatusNoContent)
	}))
	defer ts.Close()

	c := New(ts.URL, time.Second)
	if _, ok, err := c.Screen(context.Background(), 1, 999999); err != nil || ok {
		t.Fatalf("Screen: ok=%v err=%v", ok, err)
	}
	if gotWait != "10000" {
		t.Fatalf("wait_ms = %q, want 10000", gotWait)
	}
}

// TestScreen_NoContentIsNotAnError: 204 means "no frame yet" or "nothing
// newer within the wait". Both are ordinary outcomes of a long poll.
func TestScreen_NoContentIsNotAnError(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	}))
	defer ts.Close()

	fr, ok, err := New(ts.URL, time.Second).Screen(context.Background(), 0, 100)
	if err != nil {
		t.Fatalf("Screen: %v", err)
	}
	if ok {
		t.Fatal("ok = true for a 204")
	}
	if fr.Raw != nil {
		t.Fatal("a 204 produced frame bytes")
	}
}

// TestScreen_LongPollOutlivesTheOrdinaryRequestTimeout is why Client carries
// a second http.Client. DefaultTimeout exists to stop an ordinary JSON call
// hanging a browser poll — but a long poll is *supposed* to block, and
// bounding it by that same 3s (or, here, 80ms) would turn every wait longer
// than the timeout into a spurious "mayhem-b200 is not running".
func TestScreen_LongPollOutlivesTheOrdinaryRequestTimeout(t *testing.T) {
	raw := sampleFrame(4, 8, 8)
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		time.Sleep(300 * time.Millisecond) // backend holding the poll open
		w.Header().Set("Content-Type", "application/octet-stream")
		_, _ = w.Write(raw)
	}))
	defer ts.Close()

	c := New(ts.URL, 80*time.Millisecond) // far shorter than the backend's wait
	fr, ok, err := c.Screen(context.Background(), 0, 1000)
	if err != nil {
		t.Fatalf("Screen: %v — the long poll was bounded by the ordinary request timeout", err)
	}
	if !ok || fr.Seq != 4 {
		t.Fatalf("ok=%v seq=%d", ok, fr.Seq)
	}
}

// TestScreen_ContextStillBoundsTheRequest: the poll client has no timeout of
// its own, so the caller's context must be the thing that ends it.
func TestScreen_ContextStillBoundsTheRequest(t *testing.T) {
	block := make(chan struct{})
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		select {
		case <-block:
		case <-r.Context().Done():
		}
	}))
	defer func() { close(block); ts.Close() }()

	ctx, cancel := context.WithTimeout(context.Background(), 100*time.Millisecond)
	defer cancel()
	start := time.Now()
	if _, _, err := New(ts.URL, time.Second).Screen(ctx, 0, 5000); err == nil {
		t.Fatal("Screen returned nil error for a cancelled context")
	}
	if elapsed := time.Since(start); elapsed > 3*time.Second {
		t.Fatalf("Screen took %s to notice a 100ms context deadline", elapsed)
	}
}

func TestScreen_MalformedFrameIsADecodeFailure(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/octet-stream")
		_, _ = w.Write([]byte("this is not a frame"))
	}))
	defer ts.Close()

	_, _, err := New(ts.URL, time.Second).Screen(context.Background(), 0, 0)
	if err == nil {
		t.Fatal("Screen accepted a body that is not a frame")
	}
	var cerr *Error
	if !errors.As(err, &cerr) {
		t.Fatalf("error is %T, want *client.Error", err)
	}
	if !cerr.DecodeFailed {
		t.Fatalf("DecodeFailed = false for a malformed frame (%v)", cerr)
	}
	if cerr.Unavailable() {
		t.Fatal("Unavailable() = true for a backend that answered — the UI would show the wrong message")
	}
}

func TestScreen_BackendNotRunning(t *testing.T) {
	c := New("http://127.0.0.1:1", time.Second) // nothing listens on port 1
	_, _, err := c.Screen(context.Background(), 0, 0)
	if err == nil {
		t.Fatal("Screen succeeded against a dead address")
	}
	var cerr *Error
	if !errors.As(err, &cerr) || !cerr.Unavailable() {
		t.Fatalf("error = %v, want an *Error with Unavailable()", err)
	}
}

// --- contract 2 --------------------------------------------------------------

func TestInput_PostsEventsVerbatim(t *testing.T) {
	var gotBody, gotType, gotMethod string
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		b, _ := io.ReadAll(r.Body)
		gotBody, gotType, gotMethod = string(b), r.Header.Get("Content-Type"), r.Method
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"queued":2,"dropped":1}`))
	}))
	defer ts.Close()

	events := []json.RawMessage{
		json.RawMessage(`{"type":"key","key":"up","down":true}`),
		json.RawMessage(`{"type":"wobble","amount":3}`), // unknown type: the C++ side drops and counts it
	}
	res, err := New(ts.URL, time.Second).Input(context.Background(), events)
	if err != nil {
		t.Fatalf("Input: %v", err)
	}
	if gotMethod != http.MethodPost || !strings.HasPrefix(gotType, "application/json") {
		t.Fatalf("method/content-type = %s / %q", gotMethod, gotType)
	}
	want := `{"events":[{"type":"key","key":"up","down":true},{"type":"wobble","amount":3}]}`
	if gotBody != want {
		t.Fatalf("body = %s\nwant   %s", gotBody, want)
	}
	if res.Queued != 2 || res.Dropped != 1 {
		t.Fatalf("result = %+v, want queued 2 dropped 1", res)
	}
}

// TestInput_EmptyBatchIsNotSent: the input loop ticks constantly; an empty
// tick must not become an HTTP request.
func TestInput_EmptyBatchIsNotSent(t *testing.T) {
	calls := 0
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		_, _ = w.Write([]byte(`{}`))
	}))
	defer ts.Close()

	if _, err := New(ts.URL, time.Second).Input(context.Background(), nil); err != nil {
		t.Fatalf("Input: %v", err)
	}
	if calls != 0 {
		t.Fatalf("an empty batch made %d request(s)", calls)
	}
}

func TestInput_BackendErrorIsTranslated(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusBadRequest)
		_, _ = w.Write([]byte(`{"error":"events must be an array"}`))
	}))
	defer ts.Close()

	_, err := New(ts.URL, time.Second).Input(context.Background(), []json.RawMessage{json.RawMessage(`1`)})
	if err == nil {
		t.Fatal("Input succeeded against a 400")
	}
	var cerr *Error
	if !errors.As(err, &cerr) {
		t.Fatalf("error is %T, want *client.Error", err)
	}
	if cerr.Status != http.StatusBadRequest || cerr.Message != "events must be an array" {
		t.Fatalf("error = %+v, want the backend's own status and message", cerr)
	}
}
