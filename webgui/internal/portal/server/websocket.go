// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200. Originally written for sdrlink (MIT) and relicensed
// here by its author/copyright holder into this GPL-2.0-or-later project.

package server

import (
	"bufio"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"sync"
)

// A minimal, dependency-free RFC 6455 WebSocket server implementation.
//
// This is a copy-adaptation of internal/web/websocket.go, not an import of
// it. The portal and the sdrlink radio GUI are deliberately separate
// packages that share no state (see this package's doc comment), and
// internal/web's copy is entirely unexported — exporting it would couple the
// two halves together across a boundary the project has kept clean on
// purpose. The duplication is ~200 lines of frozen protocol that has not
// changed since it was written; the coupling would be permanent.
//
// It implements exactly what the screen socket needs: unmasked
// server->client binary and text frames, masked client->server frame
// reading, ping/pong/close. No fragmentation reassembly of client frames and
// no permessage-deflate — screen frames carry their own optional DEFLATE
// payload instead (contract 3, format 2), which keeps compression a property
// of the frame rather than of the transport.

const wsGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

// Opcodes, RFC 6455 §11.8.
const (
	wsOpContinuation byte = 0x0
	wsOpText         byte = 0x1
	wsOpBinary       byte = 0x2
	wsOpClose        byte = 0x8
	wsOpPing         byte = 0x9
	wsOpPong         byte = 0xA
)

var (
	errNotUpgrade  = errors.New("portal: not a websocket upgrade request")
	errNoHijack    = errors.New("portal: response writer does not support hijacking")
	errFrameTooBig = errors.New("portal: websocket frame exceeds maximum size")
)

// maxClientFrame bounds how much of a client frame's declared length we will
// allocate for, so a malicious/buggy client can't make us allocate arbitrary
// amounts of memory from a forged length field. The only thing a browser
// sends on this socket is a small JSON input batch.
const maxClientFrame = 1 << 20 // 1 MiB

// wsAcceptKey computes the Sec-WebSocket-Accept value for a given
// Sec-WebSocket-Key per RFC 6455 §1.3.
func wsAcceptKey(key string) string {
	h := sha1.New()
	h.Write([]byte(key))
	h.Write([]byte(wsGUID))
	return base64.StdEncoding.EncodeToString(h.Sum(nil))
}

// wsConn is one upgraded WebSocket connection.
type wsConn struct {
	rwc net.Conn
	br  *bufio.Reader

	writeMu sync.Mutex
	bw      *bufio.Writer
}

// wsUpgrade validates and performs the WebSocket opening handshake
// (RFC 6455 §4) on r/w, hijacking the underlying connection. On success the
// HTTP response has already been written (101 Switching Protocols); the
// caller owns the returned connection and must Close it.
func wsUpgrade(w http.ResponseWriter, r *http.Request) (*wsConn, error) {
	if !headerContainsToken(r.Header.Get("Connection"), "upgrade") ||
		!strings.EqualFold(r.Header.Get("Upgrade"), "websocket") {
		return nil, errNotUpgrade
	}
	key := strings.TrimSpace(r.Header.Get("Sec-WebSocket-Key"))
	if key == "" {
		return nil, errNotUpgrade
	}

	hj, ok := w.(http.Hijacker)
	if !ok {
		return nil, errNoHijack
	}
	conn, brw, err := hj.Hijack()
	if err != nil {
		return nil, err
	}

	accept := wsAcceptKey(key)
	resp := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + accept + "\r\n\r\n"
	if _, err := brw.WriteString(resp); err != nil {
		conn.Close()
		return nil, err
	}
	if err := brw.Flush(); err != nil {
		conn.Close()
		return nil, err
	}

	return &wsConn{rwc: conn, br: brw.Reader, bw: bufio.NewWriter(conn)}, nil
}

// headerContainsToken reports whether comma-separated header value v
// contains token, case-insensitively (handles "Connection: keep-alive, Upgrade").
func headerContainsToken(v, token string) bool {
	for _, part := range strings.Split(v, ",") {
		if strings.EqualFold(strings.TrimSpace(part), token) {
			return true
		}
	}
	return false
}

func (c *wsConn) Close() error { return c.rwc.Close() }

// writeFrame sends one unmasked, unfragmented frame, per RFC 6455 §5.2.
// Server-to-client frames must not be masked.
func (c *wsConn) writeFrame(opcode byte, payload []byte) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()

	var hdr [10]byte
	hdr[0] = 0x80 | (opcode & 0x0f) // FIN=1, no fragmentation
	n := len(payload)
	var hn int
	switch {
	case n <= 125:
		hdr[1] = byte(n)
		hn = 2
	case n <= 0xFFFF:
		hdr[1] = 126
		binary.BigEndian.PutUint16(hdr[2:4], uint16(n))
		hn = 4
	default:
		hdr[1] = 127
		binary.BigEndian.PutUint64(hdr[2:10], uint64(n))
		hn = 10
	}
	if _, err := c.bw.Write(hdr[:hn]); err != nil {
		return err
	}
	if len(payload) > 0 {
		if _, err := c.bw.Write(payload); err != nil {
			return err
		}
	}
	return c.bw.Flush()
}

// WriteJSON marshals v and sends it as a single text frame.
func (c *wsConn) WriteJSON(v any) error {
	b, err := json.Marshal(v)
	if err != nil {
		return err
	}
	return c.writeFrame(wsOpText, b)
}

// WriteClose sends a close frame with the given status code and reason.
func (c *wsConn) WriteClose(code uint16, reason string) error {
	payload := make([]byte, 2+len(reason))
	binary.BigEndian.PutUint16(payload[:2], code)
	copy(payload[2:], reason)
	return c.writeFrame(wsOpClose, payload)
}

// ReadMessage reads one (unfragmented) frame from the client. Client frames
// must be masked (RFC 6455 §5.1); this unmasks the payload before returning
// it. This server does not need to accept fragmented data frames from the
// browser, so FIN=0 is treated as a protocol error rather than reassembled.
func (c *wsConn) ReadMessage() (opcode byte, payload []byte, err error) {
	var hdr [2]byte
	if _, err = io.ReadFull(c.br, hdr[:]); err != nil {
		return 0, nil, err
	}
	fin := hdr[0]&0x80 != 0
	opcode = hdr[0] & 0x0f
	masked := hdr[1]&0x80 != 0
	length := uint64(hdr[1] & 0x7f)

	switch length {
	case 126:
		var ext [2]byte
		if _, err = io.ReadFull(c.br, ext[:]); err != nil {
			return 0, nil, err
		}
		length = uint64(binary.BigEndian.Uint16(ext[:]))
	case 127:
		var ext [8]byte
		if _, err = io.ReadFull(c.br, ext[:]); err != nil {
			return 0, nil, err
		}
		length = binary.BigEndian.Uint64(ext[:])
	}
	if length > maxClientFrame {
		return 0, nil, errFrameTooBig
	}

	var maskKey [4]byte
	if masked {
		if _, err = io.ReadFull(c.br, maskKey[:]); err != nil {
			return 0, nil, err
		}
	}

	payload = make([]byte, length)
	if _, err = io.ReadFull(c.br, payload); err != nil {
		return 0, nil, err
	}
	if masked {
		for i := range payload {
			payload[i] ^= maskKey[i%4]
		}
	}
	if !fin {
		return 0, nil, fmt.Errorf("portal: fragmented client frames are not supported")
	}
	return opcode, payload, nil
}
