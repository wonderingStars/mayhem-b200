// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.
//
// Package webadapter bridges internal/sdrclient (a network client of any
// sdrlink-protocol server) to internal/web (the browser-facing GUI, which
// was built against its own, deliberately minimal Backend/Device
// interfaces -- see that package's types.go). It plays the same structural
// role sdrlink's own internal/webadapter plays for its in-process device
// backends, but the thing being adapted is different in kind: sdrlink's
// version wraps an in-process device.Device; this one wraps a TCP client
// of a possibly-remote server, so every method here may block on a network
// round trip (bounded by internal/sdrclient's own reply timeouts) rather
// than returning instantly from a local read.
package webadapter

import (
	"context"
	"sync"
	"time"

	"mayhemb200/webgui/internal/sdrclient"
	"mayhemb200/webgui/internal/web"
)

// Backend implements web.Backend over one sdrclient.Client control
// connection. PROTOCOL.md gives a client no visibility into a server's
// other sessions (there is no "list sessions" command), so unlike
// sdrlink's in-process Backend, which reports every session the server
// knows about, Sessions() here reports exactly one entry: this client's own
// control session. Likewise Uptime() reports time since THIS client
// connected, not the server's own process uptime, which the wire protocol
// has no command to read.
type Backend struct {
	client      *sdrclient.Client
	controlAddr string
	streamAddr  string // host:port of the server's IQ stream connection, PROTOCOL.md section 1
	clientName  string

	connectedAt time.Time
	sessionID   string

	mu      sync.Mutex
	current *deviceAdapter
}

// NewBackend wraps client, which must already have completed Hello
// (PROTOCOL.md's mandatory first command) -- Backend reads its SessionID()
// once, at construction. controlAddr/streamAddr are the host:port pairs of
// the server's two connections (PROTOCOL.md section 1, default ports
// 5960/5961) and clientName is whatever name was given to Hello, for the
// status panel.
func NewBackend(client *sdrclient.Client, controlAddr, streamAddr, clientName string) *Backend {
	return &Backend{
		client:      client,
		controlAddr: controlAddr,
		streamAddr:  streamAddr,
		clientName:  clientName,
		connectedAt: time.Now(),
		sessionID:   client.SessionID(),
	}
}

// Compile-time interface satisfaction checks.
var (
	_ web.Backend = (*Backend)(nil)
	_ web.Device  = (*deviceAdapter)(nil)
)

func (b *Backend) ListDevices(ctx context.Context) ([]web.DeviceInfo, error) {
	infos, err := b.client.ListDevices(ctx)
	if err != nil {
		return nil, translateErr(err)
	}
	out := make([]web.DeviceInfo, len(infos))
	for i, in := range infos {
		out[i] = convertInfo(in)
	}
	return out, nil
}

func (b *Backend) OpenDevice(ctx context.Context, args string) (web.Device, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.current != nil {
		return nil, web.ErrDeviceInUse
	}

	caps, err := b.client.Open(ctx, args)
	if err != nil {
		return nil, translateErr(err)
	}

	da, err := newDeviceAdapter(b.client, b.streamAddr, args, caps)
	if err != nil {
		// Open succeeded control-side but we couldn't even read the
		// device's initial state back -- release it rather than leave the
		// server holding it open with no adapter anyone can reach.
		closeCtx, cancel := context.WithTimeout(context.Background(), sdrclient.DefaultReplyTimeout)
		_ = b.client.CloseDevice(closeCtx)
		cancel()
		return nil, err
	}
	b.current = da
	return da, nil
}

func (b *Backend) CurrentDevice() (web.Device, bool) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.current == nil {
		return nil, false
	}
	return b.current, true
}

func (b *Backend) CloseDevice() error {
	b.mu.Lock()
	cur := b.current
	b.current = nil
	b.mu.Unlock()
	if cur == nil {
		return nil
	}
	return cur.close()
}

func (b *Backend) Sessions() []web.SessionInfo {
	b.mu.Lock()
	open := b.current != nil
	var streaming bool
	if open {
		streaming = b.current.State().Streaming
	}
	b.mu.Unlock()

	return []web.SessionInfo{{
		ID:     b.sessionID,
		Client: b.clientName,
		// RemoteAddr is the address of the sdrlink server THIS client is
		// connected to, not (as the field means for sdrlink's own in-process
		// Backend) the address the server sees a peer connecting from --
		// this Backend has no way to learn how the server sees us.
		RemoteAddr:  b.controlAddr,
		ConnectedAt: b.connectedAt,
		DeviceOpen:  open,
		Streaming:   streaming,
	}}
}

func (b *Backend) Uptime() time.Duration { return time.Since(b.connectedAt) }
