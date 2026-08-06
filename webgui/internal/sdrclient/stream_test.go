// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.
//
// Like client_test.go, these drive Stream over an in-memory net.Pipe()
// scripted by each test's own "fake server" goroutine -- no live sdrlink
// server involved.

package sdrclient

import (
	"bufio"
	"encoding/binary"
	"encoding/json"
	"math"
	"net"
	"testing"
	"time"
)

// pipeStream returns a Stream wired to one end of an in-memory pipe and a
// buffered reader over the other end for the test's fake-server goroutine.
func pipeStream(t *testing.T, format string) (*Stream, *bufio.Reader, net.Conn) {
	t.Helper()
	clientConn, serverConn := net.Pipe()
	s := newStream(clientConn, format)
	t.Cleanup(func() {
		_ = s.Close()
		_ = serverConn.Close()
	})
	return s, bufio.NewReader(serverConn), serverConn
}

func encodeCF32(samples []complex64) []byte {
	buf := make([]byte, len(samples)*8)
	for i, s := range samples {
		binary.LittleEndian.PutUint32(buf[i*8:], math.Float32bits(real(s)))
		binary.LittleEndian.PutUint32(buf[i*8+4:], math.Float32bits(imag(s)))
	}
	return buf
}

func buildFrame(seq uint32, ts uint64, samples []complex64, flags uint32) []byte {
	hdr := buildHeader(seq, ts, uint32(len(samples)), flags)
	return append(hdr, encodeCF32(samples)...)
}

// --- session handshake --------------------------------------------------

func TestStreamSendsSessionHandshakeLine(t *testing.T) {
	s, sr, _ := pipeStream(t, FormatCF32)

	handshakeCh := make(chan string, 1)
	go func() {
		line, err := sr.ReadString('\n')
		if err != nil {
			return
		}
		handshakeCh <- line
	}()

	if err := s.sendHandshake("sess-abc"); err != nil {
		t.Fatalf("sendHandshake: %v", err)
	}

	select {
	case line := <-handshakeCh:
		var hs struct {
			SessionID string `json:"session_id"`
		}
		if err := json.Unmarshal([]byte(line), &hs); err != nil {
			t.Fatalf("unmarshal handshake line %q: %v", line, err)
		}
		if hs.SessionID != "sess-abc" {
			t.Errorf("session_id = %q, want sess-abc", hs.SessionID)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for the handshake line")
	}
}

// --- frame decoding across all three formats ---------------------------

func TestStreamReadFrameDecodesCF32(t *testing.T) {
	s, _, sc := pipeStream(t, FormatCF32)

	want := []complex64{complex(float32(0.5), float32(-0.25)), complex(float32(1), float32(-1))}
	go func() {
		_, _ = sc.Write(buildFrame(7, 123456789, want, 0))
	}()

	fr, err := s.ReadFrame()
	if err != nil {
		t.Fatalf("ReadFrame: %v", err)
	}
	if fr.Header.Seq != 7 || fr.Header.Timestamp != 123456789 {
		t.Errorf("header = %+v", fr.Header)
	}
	if len(fr.IQ) != len(want) {
		t.Fatalf("len(IQ) = %d, want %d", len(fr.IQ), len(want))
	}
	for i, s := range want {
		if fr.IQ[i] != s {
			t.Errorf("IQ[%d] = %v, want %v", i, fr.IQ[i], s)
		}
	}
}

func TestStreamReadFrameDecodesCI16(t *testing.T) {
	s, _, sc := pipeStream(t, FormatCI16)

	payload := make([]byte, 4)
	binary.LittleEndian.PutUint16(payload[0:], i16bits(16383))
	binary.LittleEndian.PutUint16(payload[2:], i16bits(-16383))
	hdr := buildHeader(1, 0, 1, 0)
	go func() {
		_, _ = sc.Write(append(hdr, payload...))
	}()

	fr, err := s.ReadFrame()
	if err != nil {
		t.Fatalf("ReadFrame: %v", err)
	}
	if len(fr.IQ) != 1 {
		t.Fatalf("len(IQ) = %d, want 1", len(fr.IQ))
	}
	wantI := float32(16383) / 32767
	wantQ := float32(-16383) / 32767
	if real(fr.IQ[0]) != wantI || imag(fr.IQ[0]) != wantQ {
		t.Errorf("IQ[0] = %v, want (%v,%v)", fr.IQ[0], wantI, wantQ)
	}
}

func TestStreamReadFrameDecodesCI8(t *testing.T) {
	s, _, sc := pipeStream(t, FormatCI8)

	payload := []byte{i8byte(63), i8byte(-63)}
	hdr := buildHeader(1, 0, 1, 0)
	go func() {
		_, _ = sc.Write(append(hdr, payload...))
	}()

	fr, err := s.ReadFrame()
	if err != nil {
		t.Fatalf("ReadFrame: %v", err)
	}
	wantI := float32(63) / 127
	wantQ := float32(-63) / 127
	if real(fr.IQ[0]) != wantI || imag(fr.IQ[0]) != wantQ {
		t.Errorf("IQ[0] = %v, want (%v,%v)", fr.IQ[0], wantI, wantQ)
	}
}

// --- seq gap detection ----------------------------------------------------

func TestStreamSeqGapDetection(t *testing.T) {
	s, _, sc := pipeStream(t, FormatCF32)
	one := []complex64{1}

	go func() {
		for _, seq := range []uint32{0, 1, 2, 5, 6} { // gap of 2 missing before seq=5 (3,4 lost)
			if _, err := sc.Write(buildFrame(seq, 0, one, 0)); err != nil {
				return
			}
		}
	}()

	wantGaps := []uint32{0, 0, 0, 2, 0}
	for i, want := range wantGaps {
		fr, err := s.ReadFrame()
		if err != nil {
			t.Fatalf("ReadFrame #%d: %v", i, err)
		}
		if fr.SeqGap != want {
			t.Errorf("frame #%d (seq=%d): SeqGap = %d, want %d", i, fr.Header.Seq, fr.SeqGap, want)
		}
	}
}

func TestStreamSeqGapWrapsCorrectly(t *testing.T) {
	s, _, sc := pipeStream(t, FormatCF32)
	one := []complex64{1}

	go func() {
		for _, seq := range []uint32{0xFFFFFFFE, 0xFFFFFFFF, 0, 1} {
			if _, err := sc.Write(buildFrame(seq, 0, one, 0)); err != nil {
				return
			}
		}
	}()

	for i, want := range []uint32{0, 0, 0, 0} {
		fr, err := s.ReadFrame()
		if err != nil {
			t.Fatalf("ReadFrame #%d: %v", i, err)
		}
		if fr.SeqGap != want {
			t.Errorf("frame #%d (seq=%#x): SeqGap = %d, want %d (wraparound must not read as a huge gap)", i, fr.Header.Seq, fr.SeqGap, want)
		}
	}
}

// --- overflow flag ----------------------------------------------------------

func TestStreamOverflowFlagReported(t *testing.T) {
	s, _, sc := pipeStream(t, FormatCF32)
	one := []complex64{1}

	go func() {
		_, _ = sc.Write(buildFrame(0, 0, one, FlagOverflow))
	}()

	fr, err := s.ReadFrame()
	if err != nil {
		t.Fatalf("ReadFrame: %v", err)
	}
	if !fr.Header.Overflow() {
		t.Error("Header.Overflow() = false, want true (FlagOverflow was set on the wire)")
	}
}

func TestStreamNoOverflowFlagWhenUnset(t *testing.T) {
	s, _, sc := pipeStream(t, FormatCF32)
	one := []complex64{1}

	go func() {
		_, _ = sc.Write(buildFrame(0, 0, one, 0))
	}()

	fr, err := s.ReadFrame()
	if err != nil {
		t.Fatalf("ReadFrame: %v", err)
	}
	if fr.Header.Overflow() {
		t.Error("Header.Overflow() = true, want false")
	}
}

// --- desync / malformed frame -------------------------------------------

func TestStreamReadFrameBadMagicErrors(t *testing.T) {
	s, _, sc := pipeStream(t, FormatCF32)
	go func() {
		bad := buildHeader(0, 0, 1, 0)
		bad[0] = 'X' // corrupt the magic
		_, _ = sc.Write(bad)
	}()

	if _, err := s.ReadFrame(); err == nil {
		t.Fatal("ReadFrame accepted a bad-magic header, want an error")
	}
}
