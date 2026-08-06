// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package sdrclient

import (
	"encoding/binary"
	"math"
	"testing"
)

// i16bits and i8byte reinterpret a signed value's bit pattern as its
// unsigned wire representation at RUNTIME (the parameter is a variable, not
// a constant expression) -- writing e.g. uint16(int16(-32768)) directly as
// a constant expression fails to compile ("constant -32768 overflows
// uint16"), because Go's constant-conversion rules check representability
// of the ORIGINAL value, not bit-pattern reinterpretation; that check only
// applies to constants, not variables.
func i16bits(v int16) uint16 { return uint16(v) }
func i8byte(v int8) byte     { return byte(v) }

// buildHeader hand-encodes a 24-byte frame header directly with
// encoding/binary, independent of ParseHeader, so these tests exercise the
// real wire byte order rather than round-tripping through our own encoder.
func buildHeader(seq uint32, ts uint64, samples uint32, flags uint32) []byte {
	buf := make([]byte, HeaderSize)
	copy(buf[0:4], []byte{'S', 'D', 'R', 'K'})
	binary.LittleEndian.PutUint32(buf[4:8], seq)
	binary.LittleEndian.PutUint64(buf[8:16], ts)
	binary.LittleEndian.PutUint32(buf[16:20], samples)
	binary.LittleEndian.PutUint32(buf[20:24], flags)
	return buf
}

func TestParseHeaderByteOrderAndFields(t *testing.T) {
	buf := buildHeader(0x11223344, 0x0102030405060708, 96, FlagOverflow)
	h, err := ParseHeader(buf)
	if err != nil {
		t.Fatalf("ParseHeader: %v", err)
	}
	if h.Seq != 0x11223344 {
		t.Errorf("Seq = %#x, want %#x", h.Seq, 0x11223344)
	}
	if h.Timestamp != 0x0102030405060708 {
		t.Errorf("Timestamp = %#x, want %#x", h.Timestamp, uint64(0x0102030405060708))
	}
	if h.Samples != 96 {
		t.Errorf("Samples = %d, want 96", h.Samples)
	}
	if !h.Overflow() {
		t.Error("Overflow() = false, want true (FlagOverflow was set)")
	}
}

func TestParseHeaderOverflowFlagUnset(t *testing.T) {
	buf := buildHeader(1, 0, 10, 0)
	h, err := ParseHeader(buf)
	if err != nil {
		t.Fatalf("ParseHeader: %v", err)
	}
	if h.Overflow() {
		t.Error("Overflow() = true, want false (flags=0)")
	}
}

func TestParseHeaderShortBuffer(t *testing.T) {
	buf := buildHeader(1, 2, 3, 0)
	if _, err := ParseHeader(buf[:HeaderSize-1]); err == nil {
		t.Fatal("ParseHeader accepted a 23-byte buffer, want an error")
	}
}

func TestParseHeaderBadMagic(t *testing.T) {
	buf := buildHeader(1, 2, 3, 0)
	buf[0] = 'X'
	_, err := ParseHeader(buf)
	if err == nil {
		t.Fatal("ParseHeader accepted a bad magic, want an error")
	}
	if err != ErrBadMagic {
		t.Errorf("ParseHeader error = %v, want ErrBadMagic", err)
	}
}

func TestBytesPerSample(t *testing.T) {
	cases := []struct {
		format string
		want   int
	}{
		{FormatCF32, 8},
		{FormatCI16, 4},
		{FormatCI8, 2},
	}
	for _, c := range cases {
		got, err := BytesPerSample(c.format)
		if err != nil {
			t.Fatalf("BytesPerSample(%q): %v", c.format, err)
		}
		if got != c.want {
			t.Errorf("BytesPerSample(%q) = %d, want %d", c.format, got, c.want)
		}
	}
	if _, err := BytesPerSample("bogus"); err == nil {
		t.Fatal("BytesPerSample(\"bogus\") succeeded, want an error")
	}
}

// --- sample-count maths ------------------------------------------------------

func TestDecodePayloadSampleCountMaths(t *testing.T) {
	// 12 complex samples of cf32 (8 bytes each) = 96 bytes.
	const n = 12
	payload := make([]byte, n*8)
	for i := 0; i < n; i++ {
		binary.LittleEndian.PutUint32(payload[i*8:], math.Float32bits(float32(i)))
		binary.LittleEndian.PutUint32(payload[i*8+4:], math.Float32bits(float32(-i)))
	}
	iq, err := DecodePayload(FormatCF32, n, payload)
	if err != nil {
		t.Fatalf("DecodePayload: %v", err)
	}
	if len(iq) != n {
		t.Fatalf("len(iq) = %d, want %d", len(iq), n)
	}
	for i, s := range iq {
		if real(s) != float32(i) || imag(s) != float32(-i) {
			t.Errorf("iq[%d] = %v, want (%d,%d)", i, s, i, -i)
		}
	}
}

func TestDecodePayloadShortPayloadErrors(t *testing.T) {
	// Header claims 4 samples of ci16 (4 bytes each = 16 bytes) but only 8
	// bytes are actually supplied.
	if _, err := DecodePayload(FormatCI16, 4, make([]byte, 8)); err == nil {
		t.Fatal("DecodePayload accepted a short payload, want an error")
	}
}

func TestDecodePayloadUnknownFormat(t *testing.T) {
	if _, err := DecodePayload("bogus", 1, make([]byte, 8)); err == nil {
		t.Fatal("DecodePayload accepted an unknown format, want an error")
	}
}

// --- sample conversion: full scale and edge values --------------------------

func TestDecodePayloadCF32PassesThroughExactly(t *testing.T) {
	payload := make([]byte, 8)
	binary.LittleEndian.PutUint32(payload[0:], math.Float32bits(1.0))
	binary.LittleEndian.PutUint32(payload[4:], math.Float32bits(-1.0))
	iq, err := DecodePayload(FormatCF32, 1, payload)
	if err != nil {
		t.Fatalf("DecodePayload: %v", err)
	}
	if real(iq[0]) != 1.0 || imag(iq[0]) != -1.0 {
		t.Errorf("iq[0] = %v, want (1,-1)", iq[0])
	}
}

func TestDecodePayloadCI16FullScale(t *testing.T) {
	// PROTOCOL.md section 3: ci16 full scale is int16 32767.
	payload := make([]byte, 4)
	binary.LittleEndian.PutUint16(payload[0:], i16bits(32767))  // I = +full scale
	binary.LittleEndian.PutUint16(payload[2:], i16bits(-32767)) // Q = -full scale
	iq, err := DecodePayload(FormatCI16, 1, payload)
	if err != nil {
		t.Fatalf("DecodePayload: %v", err)
	}
	if got := real(iq[0]); got != 1.0 {
		t.Errorf("I = %v, want 1.0 exactly at +32767", got)
	}
	if got := imag(iq[0]); got != -1.0 {
		t.Errorf("Q = %v, want -1.0 exactly at -32767", got)
	}
}

func TestDecodePayloadCI16MostNegativeIsBeyondUnity(t *testing.T) {
	// int16's most negative value, -32768, has no positive counterpart at
	// the same magnitude (two's complement asymmetry). Divided by the
	// format's full scale (32767) this decodes to a value fractionally past
	// -1.0, not clamped to -1.0 -- documented behaviour, must match the
	// reference server's own decoder exactly for interop.
	payload := make([]byte, 4)
	binary.LittleEndian.PutUint16(payload[0:], i16bits(-32768))
	binary.LittleEndian.PutUint16(payload[2:], 0)
	iq, err := DecodePayload(FormatCI16, 1, payload)
	if err != nil {
		t.Fatalf("DecodePayload: %v", err)
	}
	got := real(iq[0])
	want := float32(-32768) / 32767
	if got != want {
		t.Errorf("I = %v, want %v (unclamped)", got, want)
	}
	if got >= -1.0 {
		t.Errorf("I = %v, want a magnitude strictly beyond -1.0", got)
	}
}

func TestDecodePayloadCI8FullScaleAndEdge(t *testing.T) {
	// PROTOCOL.md section 3: ci8 full scale is int8 127.
	payload := []byte{
		i8byte(127),  // I = +full scale
		i8byte(-127), // Q = -full scale
	}
	iq, err := DecodePayload(FormatCI8, 1, payload)
	if err != nil {
		t.Fatalf("DecodePayload: %v", err)
	}
	if real(iq[0]) != 1.0 {
		t.Errorf("I = %v, want 1.0 exactly at +127", real(iq[0]))
	}
	if imag(iq[0]) != -1.0 {
		t.Errorf("Q = %v, want -1.0 exactly at -127", imag(iq[0]))
	}

	// -128 (int8's most negative, no positive counterpart) decodes
	// unclamped past -1.0, same reasoning as the ci16 case above.
	edge := []byte{i8byte(-128), 0}
	iq2, err := DecodePayload(FormatCI8, 1, edge)
	if err != nil {
		t.Fatalf("DecodePayload: %v", err)
	}
	want := float32(-128) / 127
	if real(iq2[0]) != want {
		t.Errorf("I = %v, want %v (unclamped)", real(iq2[0]), want)
	}
}

func TestDecodePayloadZeroSamples(t *testing.T) {
	iq, err := DecodePayload(FormatCF32, 0, nil)
	if err != nil {
		t.Fatalf("DecodePayload with 0 samples: %v", err)
	}
	if len(iq) != 0 {
		t.Errorf("len(iq) = %d, want 0", len(iq))
	}
}
