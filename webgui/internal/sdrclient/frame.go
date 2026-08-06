// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package sdrclient

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
)

// HeaderSize is the fixed size, in bytes, of the binary IQ frame header
// defined in PROTOCOL.md section 3.
const HeaderSize = 24

// magic is the 4-byte frame magic, ASCII "SDRK" (PROTOCOL.md section 3).
var magic = [4]byte{'S', 'D', 'R', 'K'}

// FlagOverflow is bit 0 of the header's flags field: an overflow occurred
// at the radio before this frame was produced (PROTOCOL.md section 3).
const FlagOverflow uint32 = 1 << 0

// Sample formats understood by start_rx and the frame payload decoder below
// (PROTOCOL.md section 3).
const (
	FormatCF32 = "cf32" // interleaved float32 I,Q -- 8 bytes/sample
	FormatCI16 = "ci16" // interleaved int16 I,Q, full scale 32767 -- 4 bytes/sample
	FormatCI8  = "ci8"  // interleaved int8 I,Q, full scale 127 -- 2 bytes/sample
)

// BytesPerSample returns the wire size of one complex sample in the given
// format, or an error if format is not one of the three PROTOCOL.md section
// 3 defines.
func BytesPerSample(format string) (int, error) {
	switch format {
	case FormatCF32:
		return 8, nil
	case FormatCI16:
		return 4, nil
	case FormatCI8:
		return 2, nil
	default:
		return 0, fmt.Errorf("sdrclient: unknown sample format %q", format)
	}
}

// Header is the decoded form of the 24-byte frame header (PROTOCOL.md
// section 3).
type Header struct {
	Seq       uint32
	Timestamp uint64 // nanoseconds, server monotonic
	Samples   uint32 // complex samples in this frame
	Flags     uint32
}

// Overflow reports whether FlagOverflow is set.
func (h Header) Overflow() bool { return h.Flags&FlagOverflow != 0 }

// ErrBadMagic is returned by ParseHeader when the first four bytes are not
// the "SDRK" frame magic -- a strong signal the stream connection has
// desynchronised (wrong format assumed, or the peer isn't an sdrlink stream
// port at all).
var ErrBadMagic = errors.New("sdrclient: bad frame magic")

// ParseHeader decodes the first HeaderSize bytes of buf. It returns an
// error if buf is too short or the magic does not match.
func ParseHeader(buf []byte) (Header, error) {
	var h Header
	if len(buf) < HeaderSize {
		return h, fmt.Errorf("sdrclient: short frame header (%d bytes, want %d)", len(buf), HeaderSize)
	}
	if buf[0] != magic[0] || buf[1] != magic[1] || buf[2] != magic[2] || buf[3] != magic[3] {
		return h, ErrBadMagic
	}
	h.Seq = binary.LittleEndian.Uint32(buf[4:8])
	h.Timestamp = binary.LittleEndian.Uint64(buf[8:16])
	h.Samples = binary.LittleEndian.Uint32(buf[16:20])
	h.Flags = binary.LittleEndian.Uint32(buf[20:24])
	return h, nil
}

// DecodePayload converts a frame payload of the given wire format into
// interleaved-I/Q complex64 samples. samples is the sample count from the
// frame's Header (Header.Samples); payload must contain at least
// samples*BytesPerSample(format) bytes.
//
// Conversion for the fixed-point formats mirrors the reference server's own
// decoder exactly (divide by the format's full-scale value, no clamping on
// the way back to float): full scale for ci16 is int16 32767, so the most
// negative representable sample (-32768) decodes to a magnitude fractionally
// past -1.0 rather than being clamped to -1.0. That asymmetry is inherent to
// two's-complement fixed point and callers must tolerate it, not silently
// clamp it away and disagree with every other implementation of this
// protocol.
func DecodePayload(format string, samples uint32, payload []byte) ([]complex64, error) {
	bps, err := BytesPerSample(format)
	if err != nil {
		return nil, err
	}
	want := int(samples) * bps
	if len(payload) < want {
		return nil, fmt.Errorf("sdrclient: short frame payload: have %d bytes, want %d", len(payload), want)
	}

	out := make([]complex64, samples)
	switch format {
	case FormatCF32:
		for i := range out {
			re := math.Float32frombits(binary.LittleEndian.Uint32(payload[i*8:]))
			im := math.Float32frombits(binary.LittleEndian.Uint32(payload[i*8+4:]))
			out[i] = complex(re, im)
		}
	case FormatCI16:
		for i := range out {
			re := float32(int16(binary.LittleEndian.Uint16(payload[i*4:]))) / 32767
			im := float32(int16(binary.LittleEndian.Uint16(payload[i*4+2:]))) / 32767
			out[i] = complex(re, im)
		}
	case FormatCI8:
		for i := range out {
			re := float32(int8(payload[i*2])) / 127
			im := float32(int8(payload[i*2+1])) / 127
			out[i] = complex(re, im)
		}
	}
	return out, nil
}
