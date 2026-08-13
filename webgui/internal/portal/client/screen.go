// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package client

import (
	"bytes"
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strconv"
	"time"
)

// Screen mirroring and remote input: the two halves of "the browser IS the
// device screen".
//
// This file is the ONLY place in the Go tree that knows the byte layout of a
// screen frame or the JSON shape of an input event. Both are frozen wire
// contracts shared with the C++ side (src/remote/), and this project has
// already been bitten four times by C++ and Go drifting apart with both
// sides' tests green — so the layout lives in exactly one function per
// direction (ParseScreenFrame / BuildScreenFrame), and everything else in
// the portal, including the WebSocket fan-out, goes through them.
//
// CONTRACT 1 — screen frames, C++ -> Go
//
//	GET /api/screen                      -> the current frame now (204 if none yet)
//	GET /api/screen?after=SEQ&wait_ms=MS -> next frame with seq > SEQ, waiting up
//	                                        to MS (cap 10000); 204 on timeout.
//
// 200 bodies are binary (application/octet-stream):
//
//	offset size field
//	0      4    magic "MBSF"
//	4      1    version = 1
//	5      1    format  = 1 (raw RGB565, little-endian)
//	6      2    width  u16 LE (240)
//	8      2    height u16 LE (320)
//	10     4    seq    u32 LE (increments only when the display reported damage)
//	14     2    reserved = 0
//	16     w*h*2 payload, rows top to bottom
//
// CONTRACT 2 — input, Go -> C++
//
//	POST /api/input  {"events":[ ... ]}  -> {"queued":N,"dropped":M}
//
// Events are passed through as raw JSON objects, never re-marshalled from a
// typed Go struct: the C++ side is the authority on which event types and
// fields are valid (it drops unknown types and counts them, and that is not
// an error), so parsing them here would only add a second, silently
// diverging opinion.

const (
	// ScreenMagic is the 4-byte frame magic, offset 0.
	ScreenMagic = "MBSF"
	// ScreenHeaderSize is the fixed header length in bytes.
	ScreenHeaderSize = 16
	// ScreenVersion is the only header version this build understands.
	ScreenVersion = 1

	// ScreenFormatRGB565 is a raw little-endian RGB565 payload: the only
	// format the C++ side ever sends.
	ScreenFormatRGB565 = 1
	// ScreenFormatDeflate is the same header with a raw-DEFLATE-compressed
	// RGB565 payload. It exists only on the browser WebSocket (contract 3);
	// the C++ API never produces it, and ParseScreenFrame therefore accepts
	// it without trying to validate the payload length.
	ScreenFormatDeflate = 2

	// ScreenMaxWaitMS is contract 1's cap on the long-poll wait.
	ScreenMaxWaitMS = 10000
)

// screenMaxBody bounds how large a frame body this client will read. A
// 240x320 RGB565 frame is 153616 bytes with its header; the slack is for a
// future larger display, not for trusting the peer.
const screenMaxBody = 8 << 20

// ScreenFrame is one decoded contract-1 frame.
type ScreenFrame struct {
	Version uint8
	Format  uint8
	Width   uint16
	Height  uint16
	Seq     uint32
	// Payload is the pixel data: Width*Height*2 bytes of little-endian
	// RGB565 for ScreenFormatRGB565. It aliases Raw rather than copying.
	Payload []byte
	// Raw is the complete frame exactly as it arrived, header included. The
	// WebSocket fan-out forwards this verbatim, so a byte the C++ side sent
	// is a byte the browser receives.
	Raw []byte
}

// ErrNotScreenFrame is returned by ParseScreenFrame for anything that is not
// a well-formed contract-1 frame.
var ErrNotScreenFrame = errors.New("not a screen frame")

// ParseScreenFrame validates and decodes a contract-1 frame. It is strict on
// purpose: a header this code does not fully understand means the C++ side
// and this build disagree about the wire format, and rendering it anyway
// would put garbage on screen and call it a picture.
func ParseScreenFrame(b []byte) (ScreenFrame, error) {
	if len(b) < ScreenHeaderSize {
		return ScreenFrame{}, fmt.Errorf("%w: %d bytes is shorter than the %d-byte header", ErrNotScreenFrame, len(b), ScreenHeaderSize)
	}
	if string(b[0:4]) != ScreenMagic {
		return ScreenFrame{}, fmt.Errorf("%w: magic %q, want %q", ErrNotScreenFrame, b[0:4], ScreenMagic)
	}
	f := ScreenFrame{
		Version: b[4],
		Format:  b[5],
		Width:   binary.LittleEndian.Uint16(b[6:8]),
		Height:  binary.LittleEndian.Uint16(b[8:10]),
		Seq:     binary.LittleEndian.Uint32(b[10:14]),
		Payload: b[ScreenHeaderSize:],
		Raw:     b,
	}
	if f.Version != ScreenVersion {
		return ScreenFrame{}, fmt.Errorf("%w: version %d, want %d", ErrNotScreenFrame, f.Version, ScreenVersion)
	}
	switch f.Format {
	case ScreenFormatRGB565:
		want := int(f.Width) * int(f.Height) * 2
		if len(f.Payload) != want {
			return ScreenFrame{}, fmt.Errorf("%w: %dx%d RGB565 needs a %d-byte payload, got %d",
				ErrNotScreenFrame, f.Width, f.Height, want, len(f.Payload))
		}
	case ScreenFormatDeflate:
		// Compressed length is by definition not derivable from w*h; the
		// inflater is the only thing that can validate it.
	default:
		return ScreenFrame{}, fmt.Errorf("%w: unknown format %d", ErrNotScreenFrame, f.Format)
	}
	return f, nil
}

// BuildScreenFrame assembles a frame from its parts. Used by the WebSocket
// fan-out to re-header a compressed payload (format 2) and by tests to
// synthesize frames; it is the inverse of ParseScreenFrame and the only
// other function that encodes the layout.
func BuildScreenFrame(format uint8, width, height uint16, seq uint32, payload []byte) []byte {
	out := make([]byte, ScreenHeaderSize+len(payload))
	copy(out[0:4], ScreenMagic)
	out[4] = ScreenVersion
	out[5] = format
	binary.LittleEndian.PutUint16(out[6:8], width)
	binary.LittleEndian.PutUint16(out[8:10], height)
	binary.LittleEndian.PutUint32(out[10:14], seq)
	// out[14:16] is the reserved field: explicitly zero.
	copy(out[ScreenHeaderSize:], payload)
	return out
}

// Screen fetches a frame per contract 1.
//
// after is the last sequence number the caller already has (0 to accept any
// frame). waitMS is how long mayhem-b200 may hold the request open waiting
// for a newer frame, capped at ScreenMaxWaitMS; <= 0 asks for whatever is
// current right now.
//
// The second return value is false when mayhem-b200 answered 204 — either it
// has produced no frame at all yet, or the long poll timed out with nothing
// newer than after. That is a normal outcome, not an error.
func (c *Client) Screen(ctx context.Context, after uint32, waitMS int) (ScreenFrame, bool, error) {
	if waitMS > ScreenMaxWaitMS {
		waitMS = ScreenMaxWaitMS
	}

	path := "/api/screen"
	if waitMS > 0 {
		q := url.Values{}
		q.Set("after", strconv.FormatUint(uint64(after), 10))
		q.Set("wait_ms", strconv.Itoa(waitMS))
		path += "?" + q.Encode()
	} else if after > 0 {
		path += "?after=" + strconv.FormatUint(uint64(after), 10)
	}

	// The shared http.Client's timeout (3s by default) is far too short for
	// a poll that is *supposed* to block for up to 10s, so long polls go
	// through a client with no timeout of its own and are bounded by this
	// context instead: the wait the backend was asked for, plus the ordinary
	// per-request budget for connect + transfer.
	reqCtx, cancel := context.WithTimeout(ctx, time.Duration(waitMS)*time.Millisecond+c.timeout)
	defer cancel()

	req, err := http.NewRequestWithContext(reqCtx, http.MethodGet, c.baseURL+path, nil)
	if err != nil {
		return ScreenFrame{}, false, &Error{Op: "screen", Message: "invalid request: " + err.Error(), Err: err}
	}
	req.Header.Set("Accept", "application/octet-stream")

	resp, err := c.pollClient.Do(req)
	if err != nil {
		return ScreenFrame{}, false, &Error{
			Op:      "screen",
			Message: "mayhem-b200 is not running or not reachable at " + c.baseURL,
			Err:     err,
		}
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusNoContent {
		return ScreenFrame{}, false, nil
	}
	body, err := io.ReadAll(io.LimitReader(resp.Body, screenMaxBody))
	if err != nil {
		return ScreenFrame{}, false, &Error{Op: "screen", Status: resp.StatusCode, Message: "reading frame: " + err.Error(), Err: err}
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return ScreenFrame{}, false, &Error{Op: "screen", Status: resp.StatusCode, Message: extractErrorMessage(body, resp.StatusCode)}
	}

	frame, err := ParseScreenFrame(body)
	if err != nil {
		return ScreenFrame{}, false, &Error{
			Op:           "screen",
			Status:       resp.StatusCode,
			DecodeFailed: true,
			Message:      "malformed screen frame from mayhem-b200: " + err.Error(),
			Err:          err,
		}
	}
	return frame, true, nil
}

// InputResult is the response to POST /api/input: how many events the C++
// side accepted onto its UI-thread queue, and how many it threw away
// (unknown type, or a full queue). Dropped is not an error — it is how the
// backend reports back-pressure honestly.
type InputResult struct {
	Queued  int `json:"queued"`
	Dropped int `json:"dropped"`
}

// inputRequest is contract 2's request body.
type inputRequest struct {
	Events []json.RawMessage `json:"events"`
}

// Input posts a batch of input events per contract 2. Events are forwarded
// exactly as the browser sent them (see the file header for why they are not
// re-marshalled here). An empty batch is not sent at all.
func (c *Client) Input(ctx context.Context, events []json.RawMessage) (InputResult, error) {
	if len(events) == 0 {
		return InputResult{}, nil
	}
	body, err := json.Marshal(inputRequest{Events: events})
	if err != nil {
		return InputResult{}, &Error{Op: "input", Message: "encoding events: " + err.Error(), Err: err}
	}
	var resp InputResult
	if err := c.do(ctx, "input", http.MethodPost, "/api/input", bytes.NewReader(body), &resp); err != nil {
		return InputResult{}, err
	}
	return resp, nil
}
