// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package sdrclient

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

// ProtoVersion is the protocol version this client speaks (PROTOCOL.md
// section 5). hello sends it and the server rejects anything it doesn't
// implement.
const ProtoVersion = 1

// ClientName is the default "client" field sent in hello, identifying this
// implementation to the server (shown in its session list).
const ClientName = "mayhem-b200-webgui"

const (
	// DefaultDialTimeout bounds the initial TCP connect.
	DefaultDialTimeout = 5 * time.Second

	// DefaultReplyTimeout bounds every command except `open`. Every other
	// command is answered locally by the server -- a short deadline is the
	// right way to notice a dead server/connection promptly.
	DefaultReplyTimeout = 5 * time.Second

	// DefaultOpenTimeout is the reply deadline for `open` alone. Opening a
	// USRP means loading an FPGA image and enumerating it over USB, which
	// PROTOCOL.md's reference server can take 6-12s to complete; a 5s
	// deadline aborts a perfectly healthy open partway through. This
	// mirrors mayhem-b200's existing C++ NetworkRadio client
	// (kOpenOverallTimeoutMs in network_radio.cpp), which was written after
	// that exact bug was hit once with a short, uniform timeout.
	DefaultOpenTimeout = 30 * time.Second
)

// Sentinel errors a caller may match with errors.Is/errors.As.
var (
	// ErrClosed is returned by any call made after the client (or its
	// underlying connection) has been closed.
	ErrClosed = errors.New("sdrclient: connection closed")
	// ErrTimeout is returned when a reply does not arrive within the
	// command's reply timeout.
	ErrTimeout = errors.New("sdrclient: reply timeout")
)

// CommandError is returned when the server answers a request with
// {"ok":false} (PROTOCOL.md section 2's error reply). Message is exactly
// the server's "error" string -- PROTOCOL.md defines specific wordings
// ("no device open", "device in use") that callers may match on.
type CommandError struct {
	Cmd     string
	Message string
}

func (e *CommandError) Error() string {
	return fmt.Sprintf("sdrclient: %s: %s", e.Cmd, e.Message)
}

// Option configures optional Client parameters.
type Option func(*Client)

// WithReplyTimeout overrides DefaultReplyTimeout.
func WithReplyTimeout(d time.Duration) Option { return func(c *Client) { c.replyTimeout = d } }

// WithOpenTimeout overrides DefaultOpenTimeout.
func WithOpenTimeout(d time.Duration) Option { return func(c *Client) { c.openTimeout = d } }

// wireRequest is one client->server control line (PROTOCOL.md section 2).
type wireRequest struct {
	ID   int64  `json:"id"`
	Cmd  string `json:"cmd"`
	Args any    `json:"args,omitempty"`
}

// wireLine is the union of everything that can arrive on the control
// connection: a reply (has "id") or an unsolicited event (has "event", no
// "id"). Decoding into one shape and branching on which fields are present
// avoids needing to peek the JSON twice. Any field this client doesn't know
// about is simply absent from this struct and therefore ignored by
// encoding/json -- PROTOCOL.md section 5's forward-compatibility rule.
type wireLine struct {
	ID     *int64          `json:"id"`
	OK     bool            `json:"ok"`
	Result json.RawMessage `json:"result"`
	Error  string          `json:"error"`
	Event  string          `json:"event"`
	Count  int             `json:"count"`
}

// pendingReply is what a call() goroutine is waiting to receive.
type pendingReply struct {
	ok     bool
	result json.RawMessage
	errMsg string
}

// Client is one control connection to an sdrlink-protocol server
// (PROTOCOL.md section 1). It supports any number of concurrent in-flight
// requests: every request carries a unique id and the matching reply is
// dispatched back to whichever goroutine sent it, however out of order
// replies arrive (PROTOCOL.md section 2 makes this the server's contract;
// this client relies on it).
//
// A Client is safe for concurrent use by multiple goroutines.
type Client struct {
	conn net.Conn
	r    *bufio.Reader

	writeMu sync.Mutex

	mu        sync.Mutex
	pending   map[int64]chan pendingReply
	closed    bool
	closeErr  error
	sessionID string
	events    chan Event

	closedCh chan struct{} // closed exactly once, when the connection dies

	nextID int64 // atomic

	replyTimeout time.Duration
	openTimeout  time.Duration
}

// Dial opens a control connection to addr (host:port, PROTOCOL.md section
// 1's control port, default 5960) and starts reading replies/events in the
// background. It does not send hello -- call Hello explicitly, which is
// PROTOCOL.md's required first command.
//
// Dial fails cleanly (returns an error) if the server is unreachable; it
// never hangs past ctx's deadline or DefaultDialTimeout, whichever applies.
func Dial(ctx context.Context, addr string, opts ...Option) (*Client, error) {
	dialCtx := ctx
	if _, ok := ctx.Deadline(); !ok {
		var cancel context.CancelFunc
		dialCtx, cancel = context.WithTimeout(ctx, DefaultDialTimeout)
		defer cancel()
	}
	var d net.Dialer
	conn, err := d.DialContext(dialCtx, "tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("sdrclient: dial %s: %w", addr, err)
	}
	return NewClientFromConn(conn, opts...), nil
}

// NewClientFromConn wraps an already-established connection as a Client,
// starting its background read loop, without performing a network dial
// itself. This is the seam package-external tests (e.g.
// internal/webadapter's) use to drive the control protocol over an
// in-memory net.Pipe() without a live server; it is equally usable by a
// real caller that obtained a net.Conn some other way (e.g. a TLS-wrapped
// connection).
func NewClientFromConn(conn net.Conn, opts ...Option) *Client {
	c := newClient(conn, opts...)
	go c.readLoop()
	return c
}

// newClient wraps an already-established connection. Exported via Dial for
// real callers; tests in this package construct one directly over a
// net.Pipe() end to drive the protocol without a live server.
func newClient(conn net.Conn, opts ...Option) *Client {
	c := &Client{
		conn:         conn,
		r:            bufio.NewReader(conn),
		pending:      make(map[int64]chan pendingReply),
		closedCh:     make(chan struct{}),
		replyTimeout: DefaultReplyTimeout,
		openTimeout:  DefaultOpenTimeout,
	}
	for _, opt := range opts {
		opt(c)
	}
	return c
}

// SessionID returns the session id captured from Hello's reply, or "" if
// Hello has not completed yet. The stream connection needs this
// (PROTOCOL.md section 3).
func (c *Client) SessionID() string {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.sessionID
}

// Events returns a channel of unsolicited server events (PROTOCOL.md
// section 2's event messages, e.g. {"event":"overflow"}). The channel is
// created on first call, is never closed while the client is open (closing
// happens when the Client itself closes, see Close), and drops events
// rather than blocking the read loop if the caller isn't keeping up.
func (c *Client) Events() <-chan Event {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.events == nil {
		c.events = make(chan Event, 16)
	}
	return c.events
}

// Close closes the underlying connection and fails every in-flight and
// future call with ErrClosed (or the error that actually broke the
// connection, if it died on its own first).
func (c *Client) Close() error {
	c.fail(ErrClosed)
	return c.conn.Close()
}

// fail marks the client closed (idempotent: only the first call has any
// effect) and unblocks every call() currently waiting in its select, so a
// dead connection is reported immediately rather than only after each
// individual reply timeout expires.
func (c *Client) fail(err error) {
	c.mu.Lock()
	if c.closed {
		c.mu.Unlock()
		return
	}
	c.closed = true
	c.closeErr = err
	c.pending = nil
	c.mu.Unlock()
	close(c.closedCh)
}

func (c *Client) readLoop() {
	for {
		line, err := c.r.ReadBytes('\n')
		if len(line) > 0 {
			c.handleLine(line)
		}
		if err != nil {
			c.fail(fmt.Errorf("sdrclient: control connection read: %w", err))
			return
		}
	}
}

func (c *Client) handleLine(line []byte) {
	var wl wireLine
	if err := json.Unmarshal(line, &wl); err != nil {
		// A malformed line from the server is not fatal to the whole
		// connection -- there is nothing to correlate it to, so drop it
		// and keep serving whatever request eventually does reply
		// correctly (or times out on its own).
		return
	}
	if wl.Event != "" {
		c.dispatchEvent(Event{Event: wl.Event, Count: wl.Count})
		return
	}
	if wl.ID == nil {
		return
	}

	c.mu.Lock()
	var ch chan pendingReply
	if c.pending != nil {
		ch = c.pending[*wl.ID]
		delete(c.pending, *wl.ID)
	}
	c.mu.Unlock()
	if ch == nil {
		return // reply to a request we're no longer waiting on (already timed out)
	}
	ch <- pendingReply{ok: wl.OK, result: wl.Result, errMsg: wl.Error}
}

func (c *Client) dispatchEvent(e Event) {
	c.mu.Lock()
	ch := c.events
	c.mu.Unlock()
	if ch == nil {
		return
	}
	select {
	case ch <- e:
	default:
		// Slow/absent consumer: drop rather than stall the read loop, same
		// tolerance PROTOCOL.md asks of clients for stream seq gaps.
	}
}

// call sends {id, cmd, args} and waits for the matching reply, decoding its
// "result" into result (if non-nil) on success. If timeout is zero,
// c.replyTimeout is used.
func (c *Client) call(ctx context.Context, cmd string, args any, timeout time.Duration, result any) error {
	c.mu.Lock()
	if c.closed {
		err := c.closeErr
		c.mu.Unlock()
		if err == nil {
			err = ErrClosed
		}
		return fmt.Errorf("sdrclient: %s: %w", cmd, err)
	}
	id := atomic.AddInt64(&c.nextID, 1)
	ch := make(chan pendingReply, 1)
	c.pending[id] = ch
	c.mu.Unlock()

	req := wireRequest{ID: id, Cmd: cmd, Args: args}
	b, err := json.Marshal(req)
	if err != nil {
		c.removePending(id)
		return fmt.Errorf("sdrclient: marshal %s request: %w", cmd, err)
	}
	b = append(b, '\n')

	c.writeMu.Lock()
	_, werr := c.conn.Write(b)
	c.writeMu.Unlock()
	if werr != nil {
		c.removePending(id)
		return fmt.Errorf("sdrclient: send %s request: %w", cmd, werr)
	}

	if timeout <= 0 {
		timeout = c.replyTimeout
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()

	select {
	case r := <-ch:
		if !r.ok {
			return &CommandError{Cmd: cmd, Message: r.errMsg}
		}
		if result != nil && len(r.result) > 0 {
			if err := json.Unmarshal(r.result, result); err != nil {
				return fmt.Errorf("sdrclient: decode %s result: %w", cmd, err)
			}
		}
		return nil
	case <-timer.C:
		c.removePending(id)
		return fmt.Errorf("sdrclient: %s: %w", cmd, ErrTimeout)
	case <-ctx.Done():
		c.removePending(id)
		return fmt.Errorf("sdrclient: %s: %w", cmd, ctx.Err())
	case <-c.closedCh:
		c.removePending(id)
		c.mu.Lock()
		err := c.closeErr
		c.mu.Unlock()
		if err == nil {
			err = ErrClosed
		}
		return fmt.Errorf("sdrclient: %s: %w", cmd, err)
	}
}

func (c *Client) removePending(id int64) {
	c.mu.Lock()
	if c.pending != nil {
		delete(c.pending, id)
	}
	c.mu.Unlock()
}

// Hello performs the mandatory first command (PROTOCOL.md section 2.1) and
// caches the returned session_id for later use by the stream connection
// (Stream, section 3).
func (c *Client) Hello(ctx context.Context, clientName string) (HelloResult, error) {
	if clientName == "" {
		clientName = ClientName
	}
	args := struct {
		Client string `json:"client"`
		Proto  int    `json:"proto"`
	}{Client: clientName, Proto: ProtoVersion}

	var res HelloResult
	if err := c.call(ctx, "hello", args, 0, &res); err != nil {
		return HelloResult{}, err
	}
	c.mu.Lock()
	c.sessionID = res.SessionID
	c.mu.Unlock()
	return res, nil
}
