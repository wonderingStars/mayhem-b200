// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package sdrclient

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"time"
)

// Frame is one decoded IQ frame delivered by Stream.ReadFrame.
type Frame struct {
	Header Header
	IQ     []complex64

	// SeqGap is how many frames appear to have been lost in transit before
	// this one: the difference between this frame's Seq and the previous
	// one, minus one. It is 0 for consecutive frames and for the first
	// frame read on a Stream (nothing to compare against yet). PROTOCOL.md
	// section 3: "A client MUST tolerate a seq gap -- it means frames were
	// dropped in transit and is the honest signal that the link cannot keep
	// up." Overflow (a drop at the radio, a different failure) is reported
	// separately via Header.Overflow().
	SeqGap uint32
}

// Stream is a connected binary IQ stream connection (PROTOCOL.md section
// 3).
type Stream struct {
	conn   net.Conn
	r      *bufio.Reader
	format string

	haveSeq bool
	lastSeq uint32
}

// DialStream opens the stream connection to addr (PROTOCOL.md section 1's
// stream port, default 5961), immediately sends the session handshake line
// carrying sessionID (obtained from the control connection's Hello), and
// prepares to decode frames in the given format (whatever start_rx returned
// as actually in effect).
//
// DialStream fails cleanly if the server is unreachable; it never hangs
// past ctx's deadline or DefaultDialTimeout, whichever applies.
func DialStream(ctx context.Context, addr, sessionID, format string) (*Stream, error) {
	dialCtx := ctx
	if _, ok := ctx.Deadline(); !ok {
		var cancel context.CancelFunc
		dialCtx, cancel = context.WithTimeout(ctx, DefaultDialTimeout)
		defer cancel()
	}
	var d net.Dialer
	conn, err := d.DialContext(dialCtx, "tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("sdrclient: dial stream %s: %w", addr, err)
	}
	s := newStream(conn, format)
	if err := s.sendHandshake(sessionID); err != nil {
		_ = conn.Close()
		return nil, err
	}
	return s, nil
}

// newStream wraps an already-established connection, before the handshake
// line is sent. Used by DialStream for real connections and directly by
// tests in this package over a net.Pipe() end.
func newStream(conn net.Conn, format string) *Stream {
	return &Stream{conn: conn, r: bufio.NewReader(conn), format: format}
}

func (s *Stream) sendHandshake(sessionID string) error {
	line, err := json.Marshal(struct {
		SessionID string `json:"session_id"`
	}{SessionID: sessionID})
	if err != nil {
		return fmt.Errorf("sdrclient: marshal stream handshake: %w", err)
	}
	line = append(line, '\n')
	if _, err := s.conn.Write(line); err != nil {
		return fmt.Errorf("sdrclient: send stream handshake: %w", err)
	}
	return nil
}

// SetFormat updates the format used to decode subsequent frames, for a
// caller that reissues start_rx with a different format on an already-open
// stream connection.
func (s *Stream) SetFormat(format string) { s.format = format }

// SetReadDeadline forwards to the underlying connection, letting a caller
// bound how long ReadFrame can block.
func (s *Stream) SetReadDeadline(t time.Time) error { return s.conn.SetReadDeadline(t) }

// Close closes the stream connection.
func (s *Stream) Close() error { return s.conn.Close() }

// ReadFrame blocks for the next frame, decodes it against the stream's
// current format, and updates seq-gap tracking. It returns an error if the
// connection is lost, the header magic is wrong (desync), or the payload is
// short for the declared sample count.
func (s *Stream) ReadFrame() (Frame, error) {
	var hdr [HeaderSize]byte
	if _, err := io.ReadFull(s.r, hdr[:]); err != nil {
		return Frame{}, fmt.Errorf("sdrclient: read frame header: %w", err)
	}
	h, err := ParseHeader(hdr[:])
	if err != nil {
		return Frame{}, err
	}

	bps, err := BytesPerSample(s.format)
	if err != nil {
		return Frame{}, err
	}
	payload := make([]byte, int(h.Samples)*bps)
	if len(payload) > 0 {
		if _, err := io.ReadFull(s.r, payload); err != nil {
			return Frame{}, fmt.Errorf("sdrclient: read frame payload: %w", err)
		}
	}
	iq, err := DecodePayload(s.format, h.Samples, payload)
	if err != nil {
		return Frame{}, err
	}

	var gap uint32
	if s.haveSeq && h.Seq != s.lastSeq {
		// uint32 arithmetic wraps correctly across the Seq field's own
		// wraparound (PROTOCOL.md section 3: "increments per frame,
		// wraps"): e.g. lastSeq=0xFFFFFFFF, Seq=0 gives gap=0, not a false
		// 4-billion-frame gap.
		gap = h.Seq - s.lastSeq - 1
	}
	s.haveSeq = true
	s.lastSeq = h.Seq

	return Frame{Header: h, IQ: iq, SeqGap: gap}, nil
}
