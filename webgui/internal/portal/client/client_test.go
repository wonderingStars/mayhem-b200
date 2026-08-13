// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package client

import (
	"context"
	"errors"
	"net"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestApps_Success(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet || r.URL.Path != "/api/apps" {
			t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"apps":[{"id":"adsbrx","display_name":"ADS-B RX","category":"Receive","hardware_limited":false}]}`))
	}))
	defer ts.Close()

	c := New(ts.URL, time.Second)
	apps, err := c.Apps(context.Background())
	if err != nil {
		t.Fatalf("Apps: %v", err)
	}
	if len(apps) != 1 || apps[0].ID != "adsbrx" || apps[0].DisplayName != "ADS-B RX" {
		t.Fatalf("apps = %+v, want one adsbrx entry", apps)
	}
}

func TestLaunch_Success(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/api/apps/adsbrx/launch" {
			t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"id":"adsbrx","title":"ADS-B RX","can_go_back":true}`))
	}))
	defer ts.Close()

	c := New(ts.URL, time.Second)
	cur, err := c.Launch(context.Background(), "adsbrx")
	if err != nil {
		t.Fatalf("Launch: %v", err)
	}
	if cur.ID != "adsbrx" || cur.Title != "ADS-B RX" || !cur.CanGoBack {
		t.Fatalf("current = %+v", cur)
	}
	if cur.AtHome() {
		t.Fatalf("AtHome() = true for a launched app")
	}
}

// Launch's id must be escaped in the outbound path so an id with characters
// like '/' or '?' can't smuggle a different route into the backend request.
func TestLaunch_EscapesID(t *testing.T) {
	var gotPath string
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// r.URL.Path is always the *decoded* form; the wire-level encoding
		// (what actually matters here — that a literal '/' or space inside
		// id can't be mistaken for a path separator) only survives on
		// EscapedPath()/RawPath.
		gotPath = r.URL.EscapedPath()
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"title":"x"}`))
	}))
	defer ts.Close()

	c := New(ts.URL, time.Second)
	if _, err := c.Launch(context.Background(), "weird id/x"); err != nil {
		t.Fatalf("Launch: %v", err)
	}
	if gotPath != "/api/apps/weird%20id%2Fx/launch" {
		t.Fatalf("path = %q, want escaped id", gotPath)
	}
}

func TestHome_Success(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/api/apps/home" {
			t.Errorf("unexpected request: %s %s", r.Method, r.URL.Path)
		}
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"title":"Home","can_go_back":false}`))
	}))
	defer ts.Close()

	c := New(ts.URL, time.Second)
	cur, err := c.Home(context.Background())
	if err != nil {
		t.Fatalf("Home: %v", err)
	}
	if !cur.AtHome() {
		t.Fatalf("AtHome() = false for the home response")
	}
}

func TestPanel_NoStructuredView(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{}`))
	}))
	defer ts.Close()

	c := New(ts.URL, time.Second)
	p, err := c.Panel(context.Background(), "")
	if err != nil {
		t.Fatalf("Panel: %v", err)
	}
	if p.HasData() {
		t.Fatalf("HasData() = true for an empty panel response")
	}
}

func TestStatus_Success(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"device":"B200 EDR04ZDB2","receiving":true,"transmitting":false,"version":"0.9.0"}`))
	}))
	defer ts.Close()

	c := New(ts.URL, time.Second)
	st, err := c.Status(context.Background())
	if err != nil {
		t.Fatalf("Status: %v", err)
	}
	if st.Device != "B200 EDR04ZDB2" || !st.Receiving || st.Transmitting {
		t.Fatalf("status = %+v", st)
	}
}

// TestNotRunning_ConnectionRefused is the "mayhem-b200 is not running" case:
// nothing is listening on the port at all. The client must fail cleanly and
// promptly, with an *Error whose Unavailable() is true, not hang or panic.
func TestNotRunning_ConnectionRefused(t *testing.T) {
	// Bind a listener then close it immediately, so we have a real
	// loopback address guaranteed to have nothing listening on it (more
	// reliable across CI/OS network configs than a hard-coded port number).
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("net.Listen: %v", err)
	}
	addr := l.Addr().String()
	l.Close()

	c := New("http://"+addr, time.Second)

	start := time.Now()
	_, err = c.Apps(context.Background())
	elapsed := time.Since(start)

	if err == nil {
		t.Fatalf("Apps against a closed port returned no error")
	}
	var cerr *Error
	if !errors.As(err, &cerr) {
		t.Fatalf("error = %v (%T), want *client.Error", err, err)
	}
	if !cerr.Unavailable() {
		t.Fatalf("Unavailable() = false, want true (status = %d)", cerr.Status)
	}
	if elapsed > 2*time.Second {
		t.Fatalf("Apps against a closed port took %v, want a prompt failure", elapsed)
	}
}

// TestTimeout_NoHang exercises a backend that accepts the connection but
// never answers: the client's own timeout must fire well before the test
// timeout, proving a wedged mayhem-b200 process can never hang a caller.
func TestTimeout_NoHang(t *testing.T) {
	block := make(chan struct{})
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		<-block
	}))
	// Deferred LIFO: close(block) must run BEFORE ts.Close(), or Close()
	// deadlocks forever waiting for the still-blocked handler goroutine to
	// finish (it tracks in-flight connections with its own WaitGroup).
	// ts.Close() is deferred FIRST so it runs LAST.
	defer ts.Close()
	defer close(block)

	c := New(ts.URL, 100*time.Millisecond)

	start := time.Now()
	_, err := c.Apps(context.Background())
	elapsed := time.Since(start)

	if err == nil {
		t.Fatalf("Apps against a stalled backend returned no error")
	}
	var cerr *Error
	if !errors.As(err, &cerr) {
		t.Fatalf("error = %v (%T), want *client.Error", err, err)
	}
	if !cerr.Unavailable() {
		t.Fatalf("Unavailable() = false, want true for a client-side timeout")
	}
	if elapsed > 1*time.Second {
		t.Fatalf("Apps against a stalled backend took %v, want ~100ms", elapsed)
	}
}

// TestMalformed_DecodeError exercises a 200 OK response whose body isn't
// valid JSON at all — must be a clean, typed error, not a panic, and must
// NOT be reported as Unavailable (mayhem-b200 clearly did answer).
func TestMalformed_DecodeError(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"apps": [this is not valid json`))
	}))
	defer ts.Close()

	c := New(ts.URL, time.Second)
	_, err := c.Apps(context.Background())
	if err == nil {
		t.Fatalf("Apps with a malformed body returned no error")
	}
	var cerr *Error
	if !errors.As(err, &cerr) {
		t.Fatalf("error = %v (%T), want *client.Error", err, err)
	}
	if cerr.Unavailable() {
		t.Fatalf("Unavailable() = true for a malformed-but-received response")
	}
	if !cerr.DecodeFailed {
		t.Fatalf("DecodeFailed = false, want true")
	}
	if cerr.Status != http.StatusOK {
		t.Fatalf("Status = %d, want 200", cerr.Status)
	}
}

// TestBackendError_PassesThroughStatusAndMessage covers the app-level error
// case (e.g. launching an unknown app id): the backend answered, cleanly,
// with an error. The client must surface both the status and the message
// so the portal server can pass them on to the browser unchanged.
func TestBackendError_PassesThroughStatusAndMessage(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		w.Write([]byte(`{"error":"unknown app id \"bogus\""}`))
	}))
	defer ts.Close()

	c := New(ts.URL, time.Second)
	_, err := c.Launch(context.Background(), "bogus")
	if err == nil {
		t.Fatalf("Launch of an unknown id returned no error")
	}
	var cerr *Error
	if !errors.As(err, &cerr) {
		t.Fatalf("error = %v (%T), want *client.Error", err, err)
	}
	if cerr.Unavailable() {
		t.Fatalf("Unavailable() = true for a real 404 from mayhem-b200")
	}
	if cerr.Status != http.StatusNotFound {
		t.Fatalf("Status = %d, want 404", cerr.Status)
	}
	if cerr.Message != `unknown app id "bogus"` {
		t.Fatalf("Message = %q, want the backend's error text", cerr.Message)
	}
}

// TestBackendError_NonJSONBody covers a backend error response that isn't
// even JSON (e.g. a stock 500 from some layer in front of it) — must still
// produce a usable, non-empty message instead of an empty string or panic.
func TestBackendError_NonJSONBody(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusInternalServerError)
		w.Write([]byte("internal server error"))
	}))
	defer ts.Close()

	c := New(ts.URL, time.Second)
	_, err := c.Status(context.Background())
	var cerr *Error
	if !errors.As(err, &cerr) {
		t.Fatalf("error = %v (%T), want *client.Error", err, err)
	}
	if cerr.Status != http.StatusInternalServerError {
		t.Fatalf("Status = %d, want 500", cerr.Status)
	}
	if cerr.Message == "" {
		t.Fatalf("Message is empty for a non-JSON error body")
	}
}

// TestContextCancellation ensures a caller-supplied context is honoured
// independent of the client's own timeout.
func TestContextCancellation(t *testing.T) {
	block := make(chan struct{})
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		<-block
	}))
	// See the matching comment in TestTimeout_NoHang: order matters here.
	defer ts.Close()
	defer close(block)

	c := New(ts.URL, 10*time.Second) // long client timeout; ctx should win
	ctx, cancel := context.WithTimeout(context.Background(), 50*time.Millisecond)
	defer cancel()

	start := time.Now()
	_, err := c.Apps(ctx)
	elapsed := time.Since(start)

	if err == nil {
		t.Fatalf("Apps with a cancelled context returned no error")
	}
	if elapsed > 1*time.Second {
		t.Fatalf("Apps took %v after context cancellation, want prompt failure", elapsed)
	}
}
