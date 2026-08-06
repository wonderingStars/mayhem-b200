// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.
//
// Test-only fake sdrlink-protocol server, driving Backend/deviceAdapter
// tests over an in-memory net.Pipe() control connection (plus, for the one
// test that exercises StartRx, a real loopback TCP listener for the IQ
// stream connection -- sdrclient.DialStream always dials a real address, so
// that one piece can't be a pipe). No live sdrlink-server process is
// involved anywhere in this file.

package webadapter

import (
	"bufio"
	"context"
	"encoding/json"
	"net"
	"sync"
	"testing"

	"mayhemb200/webgui/internal/sdrclient"
)

// fakeServer plays the server side of one control connection: it decodes
// {id,cmd,args} request lines and dispatches to a registered handler,
// replying {id,ok,result} or {id,ok:false,error}.
type fakeServer struct {
	r *bufio.Reader
	w net.Conn

	mu       sync.Mutex
	handlers map[string]func(args json.RawMessage) (result any, errMsg string)
}

func newFakeServer(conn net.Conn) *fakeServer {
	return &fakeServer{
		r:        bufio.NewReader(conn),
		w:        conn,
		handlers: make(map[string]func(json.RawMessage) (any, string)),
	}
}

func (f *fakeServer) handle(cmd string, fn func(args json.RawMessage) (any, string)) {
	f.mu.Lock()
	f.handlers[cmd] = fn
	f.mu.Unlock()
}

type fakeWireReq struct {
	ID   int64           `json:"id"`
	Cmd  string          `json:"cmd"`
	Args json.RawMessage `json:"args"`
}

type fakeWireReply struct {
	ID     int64  `json:"id"`
	OK     bool   `json:"ok"`
	Result any    `json:"result,omitempty"`
	Error  string `json:"error,omitempty"`
}

// serve reads and answers requests until the connection closes. Run it in
// its own goroutine; it returns on any read/write error, including a clean
// close.
func (f *fakeServer) serve() {
	for {
		line, err := f.r.ReadString('\n')
		if err != nil {
			return
		}
		var req fakeWireReq
		if err := json.Unmarshal([]byte(line), &req); err != nil {
			continue
		}

		f.mu.Lock()
		h, ok := f.handlers[req.Cmd]
		f.mu.Unlock()

		reply := fakeWireReply{ID: req.ID}
		if !ok {
			reply.Error = "unknown command " + req.Cmd
		} else {
			result, errMsg := h(req.Args)
			if errMsg != "" {
				reply.Error = errMsg
			} else {
				reply.OK = true
				reply.Result = result
			}
		}

		b, err := json.Marshal(reply)
		if err != nil {
			return
		}
		b = append(b, '\n')
		if _, err := f.w.Write(b); err != nil {
			return
		}
	}
}

// fakeDeviceState is a small, hand-rolled stand-in for a real device's
// state, mutated by the handlers registered in registerDefaultHandlers.
// Every setter's accepted-value handler adds a small, distinctive offset to
// the requested value before storing/returning it (mirroring the pattern
// sdrlink's own fake test doubles use) so tests can tell "the value the
// fake device says it accepted" apart from "the value that was requested".
type fakeDeviceState struct {
	mu sync.Mutex

	open       bool
	caps       sdrclient.Caps
	state      sdrclient.State
	stats      sdrclient.Stats
	closeCalls int
}

func newFakeDeviceState() *fakeDeviceState {
	return &fakeDeviceState{
		caps: sdrclient.Caps{
			Driver: "fake", Label: "Fake Radio", Serial: "FAKE0001",
			RXFreq:      sdrclient.Range{Min: 1, Max: 6e9, Step: 1},
			RXGain:      sdrclient.Range{Min: 0, Max: 76, Step: 1},
			RXRate:      sdrclient.Range{Min: 1, Max: 16e6, Step: 1},
			RXBandwidth: sdrclient.Range{Min: 1, Max: 56e6, Step: 1},
			TXFreq:      sdrclient.Range{Min: 1, Max: 6e9, Step: 1},
			TXGain:      sdrclient.Range{Min: 0, Max: 89.8, Step: 0.2},
			RXAntennas:  []string{"TX/RX", "RX2"},
			TXAntennas:  []string{"TX/RX"},
			HasRX:       true, HasTX: true, FullDuplex: true,
			MasterClockRate: 16e6,
		},
		state: sdrclient.State{RXAntenna: "RX2", TXAntenna: "TX/RX"},
	}
}

// registerDefaultHandlers wires every PROTOCOL.md section 2.1 command fd
// needs to answer for these tests onto fs.
func registerDefaultHandlers(fs *fakeServer, fd *fakeDeviceState) {
	fs.handle("hello", func(json.RawMessage) (any, string) {
		return struct {
			SessionID string `json:"session_id"`
			Server    string `json:"server"`
			Proto     int    `json:"proto"`
		}{"sess-fake", "fake 1.0", 1}, ""
	})

	fs.handle("list_devices", func(json.RawMessage) (any, string) {
		return struct {
			Devices []sdrclient.Info `json:"devices"`
		}{[]sdrclient.Info{{Driver: "fake", Label: "Fake Radio", Serial: "FAKE0001", Args: "driver=fake"}}}, ""
	})

	fs.handle("open", func(args json.RawMessage) (any, string) {
		fd.mu.Lock()
		defer fd.mu.Unlock()
		if fd.open {
			return nil, "device in use"
		}
		fd.open = true
		return struct {
			Caps sdrclient.Caps `json:"caps"`
		}{fd.caps}, ""
	})

	fs.handle("close", func(json.RawMessage) (any, string) {
		fd.mu.Lock()
		defer fd.mu.Unlock()
		fd.closeCalls++
		fd.open = false
		fd.state.Streaming = false
		return struct{}{}, ""
	})

	fs.handle("get_caps", func(json.RawMessage) (any, string) {
		fd.mu.Lock()
		defer fd.mu.Unlock()
		if !fd.open {
			return nil, "no device open"
		}
		return struct {
			Caps sdrclient.Caps `json:"caps"`
		}{fd.caps}, ""
	})

	fs.handle("get_state", func(json.RawMessage) (any, string) {
		fd.mu.Lock()
		defer fd.mu.Unlock()
		if !fd.open {
			return nil, "no device open"
		}
		return fd.state, ""
	})

	fs.handle("get_stats", func(json.RawMessage) (any, string) {
		fd.mu.Lock()
		defer fd.mu.Unlock()
		if !fd.open {
			return nil, "no device open"
		}
		return fd.stats, ""
	})

	setHz := func(cmd string, field *float64) {
		fs.handle(cmd, func(args json.RawMessage) (any, string) {
			var a struct {
				Hz float64 `json:"hz"`
			}
			_ = json.Unmarshal(args, &a)
			fd.mu.Lock()
			defer fd.mu.Unlock()
			if !fd.open {
				return nil, "no device open"
			}
			*field = a.Hz + 1 // distinctive "accepted" transform
			return struct {
				Hz float64 `json:"hz"`
			}{*field}, ""
		})
	}
	setHz("set_rx_freq", &fd.state.RXFreqHz)
	setHz("set_tx_freq", &fd.state.TXFreqHz)
	setHz("set_rx_rate", &fd.state.RXRateHz)
	setHz("set_tx_rate", &fd.state.TXRateHz)
	setHz("set_rx_bandwidth", &fd.state.RXBandwidthHz)
	setHz("set_tx_bandwidth", &fd.state.TXBandwidthHz)
	setHz("set_lo_offset", &fd.state.LOOffsetHz)

	setDb := func(cmd string, field *float64) {
		fs.handle(cmd, func(args json.RawMessage) (any, string) {
			var a struct {
				Db float64 `json:"db"`
			}
			_ = json.Unmarshal(args, &a)
			fd.mu.Lock()
			defer fd.mu.Unlock()
			if !fd.open {
				return nil, "no device open"
			}
			*field = a.Db + 1
			return struct {
				Db float64 `json:"db"`
			}{*field}, ""
		})
	}
	setDb("set_rx_gain", &fd.state.RXGainDB)
	setDb("set_tx_gain", &fd.state.TXGainDB)

	setName := func(cmd string, field *string) {
		fs.handle(cmd, func(args json.RawMessage) (any, string) {
			var a struct {
				Name string `json:"name"`
			}
			_ = json.Unmarshal(args, &a)
			fd.mu.Lock()
			defer fd.mu.Unlock()
			if !fd.open {
				return nil, "no device open"
			}
			*field = a.Name
			return struct {
				Name string `json:"name"`
			}{*field}, ""
		})
	}
	setName("set_rx_antenna", &fd.state.RXAntenna)
	setName("set_tx_antenna", &fd.state.TXAntenna)

	setOn := func(cmd string, field *bool) {
		fs.handle(cmd, func(args json.RawMessage) (any, string) {
			var a struct {
				On bool `json:"on"`
			}
			_ = json.Unmarshal(args, &a)
			fd.mu.Lock()
			defer fd.mu.Unlock()
			if !fd.open {
				return nil, "no device open"
			}
			*field = a.On
			return struct {
				On bool `json:"on"`
			}{*field}, ""
		})
	}
	setOn("set_rx_agc", &fd.state.RXAGC)
	setOn("set_rx_dc_offset_auto", &fd.state.RXDCOffsetAuto)
	setOn("set_rx_iq_balance_auto", &fd.state.RXIQBalanceAuto)

	fs.handle("start_rx", func(args json.RawMessage) (any, string) {
		var a struct {
			Format string `json:"format"`
		}
		_ = json.Unmarshal(args, &a)
		fd.mu.Lock()
		defer fd.mu.Unlock()
		if !fd.open {
			return nil, "no device open"
		}
		if a.Format == "" {
			a.Format = "cf32"
		}
		fd.state.Streaming = true
		fd.state.StreamFormat = a.Format
		return struct {
			Format string `json:"format"`
		}{a.Format}, ""
	})

	fs.handle("stop_rx", func(json.RawMessage) (any, string) {
		fd.mu.Lock()
		defer fd.mu.Unlock()
		fd.state.Streaming = false
		return struct{}{}, ""
	})
}

// newTestBackend wires up a Backend over an in-memory control pipe served
// by a fresh fakeServer/fakeDeviceState pair, having already completed
// Hello. streamAddr is whatever the caller wants Backend to dial for IQ
// streaming (only exercised by tests that call StartRx); pass "" if the
// test never starts streaming.
func newTestBackend(t *testing.T, streamAddr string) (*Backend, *fakeServer, *fakeDeviceState) {
	t.Helper()
	clientConn, serverConn := net.Pipe()
	fs := newFakeServer(serverConn)
	fd := newFakeDeviceState()
	registerDefaultHandlers(fs, fd)
	go fs.serve()
	t.Cleanup(func() {
		_ = clientConn.Close()
		_ = serverConn.Close()
	})

	client := sdrclient.NewClientFromConn(clientConn)
	if _, err := client.Hello(context.Background(), "test-client"); err != nil {
		t.Fatalf("Hello: %v", err)
	}

	b := NewBackend(client, "127.0.0.1:5960", streamAddr, "test-client")
	return b, fs, fd
}
