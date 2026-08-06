// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.
//
// These tests drive Client entirely over an in-memory net.Pipe(), with the
// "server" side scripted by each test's own goroutine reading/writing raw
// JSON lines. That is the seam: no real sdrlink-server process is needed
// (or used) here at all -- see cmd/mayhem-webgui's own end-to-end check for
// that.

package sdrclient

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"sync"
	"testing"
	"time"
)

// pipeClientServer returns a Client wired to one end of an in-memory pipe,
// plus a buffered reader and the raw net.Conn for the other end, which the
// caller's own goroutine uses to play the server role.
func pipeClientServer(t *testing.T, opts ...Option) (*Client, *bufio.Reader, net.Conn) {
	t.Helper()
	clientConn, serverConn := net.Pipe()
	c := newClient(clientConn, opts...)
	go c.readLoop()
	t.Cleanup(func() {
		_ = c.Close()
		_ = serverConn.Close()
	})
	return c, bufio.NewReader(serverConn), serverConn
}

type wireReq struct {
	ID   int64           `json:"id"`
	Cmd  string          `json:"cmd"`
	Args json.RawMessage `json:"args"`
}

func readReq(t *testing.T, r *bufio.Reader) wireReq {
	t.Helper()
	line, err := r.ReadString('\n')
	if err != nil {
		t.Fatalf("server: read request: %v", err)
	}
	var req wireReq
	if err := json.Unmarshal([]byte(line), &req); err != nil {
		t.Fatalf("server: unmarshal request %q: %v", line, err)
	}
	return req
}

// --- hello -------------------------------------------------------------------

func TestHelloSendsProtoVersionAndCapturesSessionID(t *testing.T) {
	c, sr, sc := pipeClientServer(t)

	reqCh := make(chan wireReq, 1)
	go func() {
		req := readReq(t, sr)
		reqCh <- req
		reply := fmt.Sprintf(`{"id":%d,"ok":true,"result":{"session_id":"sess-42","server":"fake 1.0","proto":1}}`+"\n", req.ID)
		_, _ = sc.Write([]byte(reply))
	}()

	res, err := c.Hello(context.Background(), "my-client")
	if err != nil {
		t.Fatalf("Hello: %v", err)
	}
	if res.SessionID != "sess-42" {
		t.Errorf("SessionID = %q, want sess-42", res.SessionID)
	}
	if c.SessionID() != "sess-42" {
		t.Errorf("c.SessionID() = %q, want sess-42 (not cached on the Client)", c.SessionID())
	}

	req := <-reqCh
	if req.Cmd != "hello" {
		t.Errorf("cmd = %q, want hello", req.Cmd)
	}
	var args struct {
		Client string `json:"client"`
		Proto  int    `json:"proto"`
	}
	if err := json.Unmarshal(req.Args, &args); err != nil {
		t.Fatalf("unmarshal hello args: %v", err)
	}
	if args.Proto != ProtoVersion {
		t.Errorf("proto = %d, want %d", args.Proto, ProtoVersion)
	}
	if args.Client != "my-client" {
		t.Errorf("client = %q, want my-client", args.Client)
	}
}

// --- concurrent in-flight requests, matched by id ----------------------------

// TestConcurrentInFlightRequestsMatchByID fires several requests
// concurrently and has the fake server reply to them in the OPPOSITE order
// it received them, each with a value derived from that specific request.
// If replies were matched by arrival order instead of by id, every caller
// would get someone else's answer.
func TestConcurrentInFlightRequestsMatchByID(t *testing.T) {
	c, sr, sc := pipeClientServer(t)

	const n = 6
	go func() {
		reqs := make([]wireReq, 0, n)
		for i := 0; i < n; i++ {
			reqs = append(reqs, readReq(t, sr))
		}
		for i := len(reqs) - 1; i >= 0; i-- {
			req := reqs[i]
			var args struct {
				Hz float64 `json:"hz"`
			}
			_ = json.Unmarshal(req.Args, &args)
			accepted := args.Hz + 1000 // distinctive per-request transform
			reply := fmt.Sprintf(`{"id":%d,"ok":true,"result":{"hz":%v}}`+"\n", req.ID, accepted)
			if _, err := sc.Write([]byte(reply)); err != nil {
				return
			}
		}
	}()

	var wg sync.WaitGroup
	results := make([]float64, n)
	errs := make([]error, n)
	for i := 0; i < n; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			hz := float64(i) * 1e6
			got, err := c.SetRxFreq(context.Background(), hz)
			results[i] = got
			errs[i] = err
		}(i)
	}
	wg.Wait()

	for i := 0; i < n; i++ {
		if errs[i] != nil {
			t.Fatalf("call %d: %v", i, errs[i])
		}
		want := float64(i)*1e6 + 1000
		if results[i] != want {
			t.Errorf("call %d: got %v, want %v (replies arrived out of order; must be matched by id, not by arrival order)", i, results[i], want)
		}
	}
}

// --- reply timeout differentiation -------------------------------------------

func TestReplyTimeoutFiresAfterConfiguredDuration(t *testing.T) {
	c, sr, _ := pipeClientServer(t, WithReplyTimeout(80*time.Millisecond))
	go func() {
		_ = readReq(t, sr) // read the request but never reply
	}()

	start := time.Now()
	_, err := c.SetRxGain(context.Background(), 10)
	elapsed := time.Since(start)

	if err == nil {
		t.Fatal("SetRxGain succeeded against a server that never replied, want a timeout error")
	}
	if !errors.Is(err, ErrTimeout) {
		t.Errorf("err = %v, want ErrTimeout", err)
	}
	if elapsed < 80*time.Millisecond {
		t.Errorf("elapsed = %v, fired before the configured 80ms reply timeout", elapsed)
	}
	if elapsed > time.Second {
		t.Errorf("elapsed = %v, want close to the configured 80ms reply timeout", elapsed)
	}
}

// TestOpenUsesLongerTimeoutThanNormalCommands reproduces, as a regression
// test, the exact bug this client was written to avoid: a uniform 5s-style
// deadline aborting a healthy `open` that legitimately takes longer than an
// ordinary command. The fake server delays its open reply for LONGER than
// the configured (short) normal reply timeout but SHORTER than the
// configured open timeout; Open must still succeed.
func TestOpenUsesLongerTimeoutThanNormalCommands(t *testing.T) {
	c, sr, sc := pipeClientServer(t, WithReplyTimeout(50*time.Millisecond), WithOpenTimeout(400*time.Millisecond))

	go func() {
		req := readReq(t, sr)
		time.Sleep(150 * time.Millisecond) // > replyTimeout, < openTimeout
		reply := fmt.Sprintf(`{"id":%d,"ok":true,"result":{"caps":{"driver":"mock","label":"sdrlink synthetic device"}}}`+"\n", req.ID)
		_, _ = sc.Write([]byte(reply))
	}()

	caps, err := c.Open(context.Background(), "driver=mock")
	if err != nil {
		t.Fatalf("Open: %v (a 150ms server delay must not exceed the 400ms open timeout, even though it exceeds the 50ms normal reply timeout)", err)
	}
	if caps.Driver != "mock" {
		t.Errorf("caps.Driver = %q, want mock", caps.Driver)
	}
}

// --- error replies -------------------------------------------------------

func TestErrorReplyReturnsCommandError(t *testing.T) {
	c, sr, sc := pipeClientServer(t)
	go func() {
		req := readReq(t, sr)
		reply := fmt.Sprintf(`{"id":%d,"ok":false,"error":"no device open"}`+"\n", req.ID)
		_, _ = sc.Write([]byte(reply))
	}()

	_, err := c.SetRxFreq(context.Background(), 1e6)
	if err == nil {
		t.Fatal("SetRxFreq succeeded against an ok:false reply, want an error")
	}
	var cmdErr *CommandError
	if !errors.As(err, &cmdErr) {
		t.Fatalf("err = %v (%T), want *CommandError", err, err)
	}
	if cmdErr.Message != "no device open" {
		t.Errorf("Message = %q, want %q", cmdErr.Message, "no device open")
	}
	if cmdErr.Cmd != "set_rx_freq" {
		t.Errorf("Cmd = %q, want set_rx_freq", cmdErr.Cmd)
	}
}

// --- forward compatibility: unknown fields ignored ---------------------------

func TestReplyWithUnknownFieldsIsIgnored(t *testing.T) {
	c, sr, sc := pipeClientServer(t)
	go func() {
		req := readReq(t, sr)
		reply := fmt.Sprintf(`{"id":%d,"ok":true,"result":{"hz":100000000,"future_field":"surprise"},"unexpected_top_level":42}`+"\n", req.ID)
		_, _ = sc.Write([]byte(reply))
	}()

	got, err := c.SetRxFreq(context.Background(), 1e6)
	if err != nil {
		t.Fatalf("SetRxFreq: %v (an unrecognised field must be ignored, not break parsing -- PROTOCOL.md section 5)", err)
	}
	if got != 100000000 {
		t.Errorf("got = %v, want 100000000", got)
	}
}

// --- unreachable / dead server: fails cleanly, never hangs -------------------

func TestDialUnreachableHostFailsCleanly(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("Listen: %v", err)
	}
	addr := ln.Addr().String()
	if err := ln.Close(); err != nil {
		t.Fatalf("Close listener: %v", err)
	}
	// Nothing is listening at addr any more.

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()

	start := time.Now()
	_, err = Dial(ctx, addr)
	elapsed := time.Since(start)

	if err == nil {
		t.Fatal("Dial to a closed port succeeded, want an error")
	}
	if elapsed > 3*time.Second {
		t.Errorf("Dial took %v, want it to fail well within the 3s context deadline (never hang)", elapsed)
	}
}

// TestOpenFailsCleanlyWhenConnectionDrops proves Open (which uses the
// long, 30s-by-default open timeout) does not wait out that whole timeout
// when the connection is simply dead -- it must fail as soon as that's
// detected, not hang until the deadline.
func TestOpenFailsCleanlyWhenConnectionDrops(t *testing.T) {
	c, _, sc := pipeClientServer(t, WithOpenTimeout(5*time.Second))
	if err := sc.Close(); err != nil {
		t.Fatalf("close server side: %v", err)
	}

	start := time.Now()
	_, err := c.Open(context.Background(), "driver=uhd")
	elapsed := time.Since(start)

	if err == nil {
		t.Fatal("Open succeeded against a dead connection, want an error")
	}
	if elapsed > 2*time.Second {
		t.Errorf("Open took %v to fail after the connection dropped, want well under its 5s open timeout", elapsed)
	}
}

// --- Close unblocks pending calls immediately --------------------------------

func TestCloseUnblocksPendingCallsImmediately(t *testing.T) {
	c, sr, _ := pipeClientServer(t, WithReplyTimeout(10*time.Second))
	go func() {
		_ = readReq(t, sr) // read but never reply
	}()

	done := make(chan error, 1)
	go func() {
		_, err := c.SetRxGain(context.Background(), 5)
		done <- err
	}()

	time.Sleep(50 * time.Millisecond) // let the call register itself as pending
	_ = c.Close()

	select {
	case err := <-done:
		if err == nil {
			t.Fatal("SetRxGain succeeded after Close, want an error")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("call did not unblock after Close (appears to be waiting out the full 10s reply timeout)")
	}
}

// --- events --------------------------------------------------------------

func TestEventsChannelDeliversUnsolicitedEvents(t *testing.T) {
	c, _, sc := pipeClientServer(t)
	events := c.Events()

	if _, err := sc.Write([]byte(`{"event":"overflow","count":3}` + "\n")); err != nil {
		t.Fatalf("write event: %v", err)
	}

	select {
	case e := <-events:
		if e.Event != "overflow" || e.Count != 3 {
			t.Errorf("event = %+v, want {overflow 3}", e)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for the overflow event")
	}
}
