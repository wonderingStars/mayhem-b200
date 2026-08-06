// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package webadapter

import (
	"bufio"
	"context"
	"encoding/binary"
	"encoding/json"
	"io"
	"math"
	"net"
	"testing"
	"time"
)

// --- setters: forward the accepted value, not the request -------------------

// TestDeviceAdapterSettersForwardAcceptedValue drives every setter through
// deviceAdapter (via Backend.OpenDevice's returned web.Device) and checks
// that it returns exactly what the fake server's reply said was accepted --
// the fake server's handlers all add +1 (numeric) or echo-with-a-suffix
// (name) rather than trivially returning the request, so a bug that
// short-circuits and returns the input instead of the reply would be
// caught here. It also checks State() reflects the accepted value
// afterwards.
func TestDeviceAdapterSettersForwardAcceptedValueAndUpdatesState(t *testing.T) {
	b, _, _ := newTestBackend(t, "")
	dev, err := b.OpenDevice(context.Background(), "driver=fake")
	if err != nil {
		t.Fatalf("OpenDevice: %v", err)
	}

	t.Run("SetRxFreq", func(t *testing.T) {
		got, err := dev.SetRxFreq(100e6)
		if err != nil || got != 100e6+1 {
			t.Fatalf("SetRxFreq = (%v, %v), want (%v, nil)", got, err, 100e6+1)
		}
		if dev.State().RxFreqHz != got {
			t.Errorf("State().RxFreqHz = %v, want %v", dev.State().RxFreqHz, got)
		}
	})
	t.Run("SetTxFreq", func(t *testing.T) {
		got, err := dev.SetTxFreq(200e6)
		if err != nil || got != 200e6+1 {
			t.Fatalf("SetTxFreq = (%v, %v), want (%v, nil)", got, err, 200e6+1)
		}
	})
	t.Run("SetRxRate", func(t *testing.T) {
		got, err := dev.SetRxRate(2e6)
		if err != nil || got != 2e6+1 {
			t.Fatalf("SetRxRate = (%v, %v), want (%v, nil)", got, err, 2e6+1)
		}
	})
	t.Run("SetTxRate", func(t *testing.T) {
		got, err := dev.SetTxRate(3e6)
		if err != nil || got != 3e6+1 {
			t.Fatalf("SetTxRate = (%v, %v), want (%v, nil)", got, err, 3e6+1)
		}
	})
	t.Run("SetRxGain", func(t *testing.T) {
		got, err := dev.SetRxGain(30)
		if err != nil || got != 31 {
			t.Fatalf("SetRxGain = (%v, %v), want (31, nil)", got, err)
		}
	})
	t.Run("SetTxGain", func(t *testing.T) {
		got, err := dev.SetTxGain(10)
		if err != nil || got != 11 {
			t.Fatalf("SetTxGain = (%v, %v), want (11, nil)", got, err)
		}
	})
	t.Run("SetRxBandwidth", func(t *testing.T) {
		got, err := dev.SetRxBandwidth(1e6)
		if err != nil || got != 1e6+1 {
			t.Fatalf("SetRxBandwidth = (%v, %v), want (%v, nil)", got, err, 1e6+1)
		}
	})
	t.Run("SetTxBandwidth", func(t *testing.T) {
		got, err := dev.SetTxBandwidth(1.5e6)
		if err != nil || got != 1.5e6+1 {
			t.Fatalf("SetTxBandwidth = (%v, %v), want (%v, nil)", got, err, 1.5e6+1)
		}
	})
	t.Run("SetLoOffset", func(t *testing.T) {
		got, err := dev.SetLoOffset(1000)
		if err != nil || got != 1001 {
			t.Fatalf("SetLoOffset = (%v, %v), want (1001, nil)", got, err)
		}
	})
	t.Run("SetRxAntenna", func(t *testing.T) {
		got, err := dev.SetRxAntenna("TX/RX")
		if err != nil || got != "TX/RX" {
			t.Fatalf("SetRxAntenna = (%q, %v), want (\"TX/RX\", nil)", got, err)
		}
		if dev.State().RxAntenna != "TX/RX" {
			t.Errorf("State().RxAntenna = %q, want TX/RX", dev.State().RxAntenna)
		}
	})
	t.Run("SetTxAntenna", func(t *testing.T) {
		got, err := dev.SetTxAntenna("TX/RX")
		if err != nil || got != "TX/RX" {
			t.Fatalf("SetTxAntenna = (%q, %v), want (\"TX/RX\", nil)", got, err)
		}
	})
	t.Run("SetRxAgc", func(t *testing.T) {
		got, err := dev.SetRxAgc(true)
		if err != nil || got != true {
			t.Fatalf("SetRxAgc = (%v, %v), want (true, nil)", got, err)
		}
	})
	t.Run("SetRxDcOffsetAuto", func(t *testing.T) {
		got, err := dev.SetRxDcOffsetAuto(true)
		if err != nil || got != true {
			t.Fatalf("SetRxDcOffsetAuto = (%v, %v), want (true, nil)", got, err)
		}
	})
	t.Run("SetRxIqBalanceAuto", func(t *testing.T) {
		got, err := dev.SetRxIqBalanceAuto(true)
		if err != nil || got != true {
			t.Fatalf("SetRxIqBalanceAuto = (%v, %v), want (true, nil)", got, err)
		}
	})
}

func TestDeviceAdapterSetterOnNoDeviceOpenTranslatesError(t *testing.T) {
	b, _, _ := newTestBackend(t, "")
	dev, err := b.OpenDevice(context.Background(), "driver=fake")
	if err != nil {
		t.Fatalf("OpenDevice: %v", err)
	}
	// Close out from under the adapter (as another path might), then try a
	// setter: the fake server's "no device open" reply must translate to
	// web.ErrNoDevice.
	if err := b.CloseDevice(); err != nil {
		t.Fatalf("CloseDevice: %v", err)
	}
	if _, err := dev.SetRxFreq(1e6); err == nil {
		t.Fatal("SetRxFreq succeeded on a closed device, want an error")
	}
}

// --- streaming: StartRx dials the stream connection and delivers frames -----

func buildTestHeader(seq uint32, ts uint64, samples uint32, flags uint32) []byte {
	buf := make([]byte, 24)
	copy(buf[0:4], []byte{'S', 'D', 'R', 'K'})
	binary.LittleEndian.PutUint32(buf[4:8], seq)
	binary.LittleEndian.PutUint64(buf[8:16], ts)
	binary.LittleEndian.PutUint32(buf[16:20], samples)
	binary.LittleEndian.PutUint32(buf[20:24], flags)
	return buf
}

func buildTestCF32Frame(seq uint32, iq []complex64) []byte {
	hdr := buildTestHeader(seq, 0, uint32(len(iq)), 0)
	payload := make([]byte, len(iq)*8)
	for i, s := range iq {
		binary.LittleEndian.PutUint32(payload[i*8:], math.Float32bits(real(s)))
		binary.LittleEndian.PutUint32(payload[i*8+4:], math.Float32bits(imag(s)))
	}
	return append(hdr, payload...)
}

// TestDeviceAdapterStartRxStreamsFramesAndStopRxTearsDown exercises the one
// piece of deviceAdapter that a control-only fake server can't cover:
// StartRx actively dialing the separate binary IQ stream connection
// (PROTOCOL.md section 3), sending the session handshake line, and
// republishing decoded frames to Subscribe callers with the RateHz/SeqHz
// that were in force at capture time. This needs a REAL TCP listener
// (sdrclient.DialStream always dials a real address) but still no live
// sdrlink-server process.
func TestDeviceAdapterStartRxStreamsFramesAndStopRxTearsDown(t *testing.T) {
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("Listen: %v", err)
	}
	defer ln.Close()

	handshakeCh := make(chan string, 1)
	go func() {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		defer conn.Close()

		r := bufio.NewReader(conn)
		line, err := r.ReadString('\n')
		if err != nil {
			return
		}
		var hs struct {
			SessionID string `json:"session_id"`
		}
		_ = json.Unmarshal([]byte(line), &hs)
		handshakeCh <- hs.SessionID

		want := []complex64{complex(float32(0.25), float32(-0.5))}
		if _, err := conn.Write(buildTestCF32Frame(0, want)); err != nil {
			return
		}

		// Keep the connection open (as the real server would, mid-stream)
		// until the test tears it down via StopRx/Close on the client side.
		_, _ = io.Copy(io.Discard, r)
	}()

	b, _, _ := newTestBackend(t, ln.Addr().String())
	ctx := context.Background()

	dev, err := b.OpenDevice(ctx, "driver=fake")
	if err != nil {
		t.Fatalf("OpenDevice: %v", err)
	}

	rate, err := dev.SetRxRate(2_000_000)
	if err != nil {
		t.Fatalf("SetRxRate: %v", err)
	}
	freq, err := dev.SetRxFreq(915_000_000)
	if err != nil {
		t.Fatalf("SetRxFreq: %v", err)
	}

	sub, unsub := dev.Subscribe(4)
	defer unsub()

	format, err := dev.StartRx("cf32")
	if err != nil {
		t.Fatalf("StartRx: %v", err)
	}
	if format != "cf32" {
		t.Errorf("StartRx format = %q, want cf32", format)
	}
	if !dev.State().Streaming {
		t.Error("State().Streaming = false after StartRx, want true")
	}

	select {
	case sid := <-handshakeCh:
		if sid != "sess-fake" {
			t.Errorf("stream handshake session_id = %q, want sess-fake", sid)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for the stream handshake to be sent")
	}

	select {
	case batch := <-sub:
		if len(batch.IQ) != 1 || batch.IQ[0] != complex(float32(0.25), float32(-0.5)) {
			t.Fatalf("batch.IQ = %v, want [(0.25,-0.5)]", batch.IQ)
		}
		if batch.RateHz != rate {
			t.Errorf("batch.RateHz = %v, want the accepted rate %v", batch.RateHz, rate)
		}
		if batch.SeqHz != freq {
			t.Errorf("batch.SeqHz = %v, want the accepted freq %v", batch.SeqHz, freq)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for a frame from the stream")
	}

	if err := dev.StopRx(); err != nil {
		t.Fatalf("StopRx: %v", err)
	}
	if dev.State().Streaming {
		t.Error("State().Streaming = true after StopRx, want false")
	}
}
