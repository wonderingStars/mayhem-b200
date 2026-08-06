// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200. Originally written for sdrlink (MIT) and relicensed
// here by its author/copyright holder into this GPL-2.0-or-later project.

package web

import (
	"bufio"
	"crypto/rand"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

// TestWsAcceptKey_RFCExample checks wsAcceptKey against the worked example
// from RFC 6455 §1.3, so a bug in the accept-key computation is caught
// against the spec itself rather than only against our own client/server.
func TestWsAcceptKey_RFCExample(t *testing.T) {
	got := wsAcceptKey("dGhlIHNhbXBsZSBub25jZQ==")
	want := "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
	if got != want {
		t.Fatalf("wsAcceptKey = %q, want %q", got, want)
	}
}

func makeWSKey(t *testing.T) string {
	t.Helper()
	var b [16]byte
	if _, err := rand.Read(b[:]); err != nil {
		t.Fatal(err)
	}
	return base64.StdEncoding.EncodeToString(b[:])
}

// writeMaskedFrame writes one client->server frame (must be masked per
// RFC 6455 §5.1) directly onto w, independent of wsConn (which only ever
// writes unmasked server frames).
func writeMaskedFrame(w io.Writer, opcode byte, payload []byte) error {
	n := len(payload)
	var hdr []byte
	switch {
	case n <= 125:
		hdr = []byte{0x80 | opcode, byte(n) | 0x80}
	case n <= 0xFFFF:
		hdr = []byte{0x80 | opcode, 126 | 0x80, 0, 0}
		binary.BigEndian.PutUint16(hdr[2:4], uint16(n))
	default:
		hdr = []byte{0x80 | opcode, 127 | 0x80, 0, 0, 0, 0, 0, 0, 0, 0}
		binary.BigEndian.PutUint64(hdr[2:10], uint64(n))
	}
	var mask [4]byte
	if _, err := rand.Read(mask[:]); err != nil {
		return err
	}
	hdr = append(hdr, mask[:]...)
	masked := make([]byte, n)
	for i, b := range payload {
		masked[i] = b ^ mask[i%4]
	}
	if _, err := w.Write(hdr); err != nil {
		return err
	}
	_, err := w.Write(masked)
	return err
}

// dialWS performs the RFC 6455 opening handshake as a raw HTTP client
// against srv's path, returning the underlying connection, a *wsConn
// wrapping it for reading server frames, and the response headers (so
// callers can assert on Sec-WebSocket-Accept).
func dialWS(t *testing.T, srv *httptest.Server, path string) (net.Conn, *wsConn, *http.Response) {
	t.Helper()
	addr := strings.TrimPrefix(srv.URL, "http://")
	conn, err := net.Dial("tcp", addr)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	key := makeWSKey(t)
	req := fmt.Sprintf(
		"GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n",
		path, addr, key,
	)
	if _, err := conn.Write([]byte(req)); err != nil {
		t.Fatalf("write handshake: %v", err)
	}
	br := bufio.NewReader(conn)
	resp, err := http.ReadResponse(br, nil)
	if err != nil {
		t.Fatalf("read handshake response: %v", err)
	}
	if resp.StatusCode != http.StatusSwitchingProtocols {
		t.Fatalf("status = %d, want 101", resp.StatusCode)
	}
	wantAccept := wsAcceptKey(key)
	if got := resp.Header.Get("Sec-WebSocket-Accept"); got != wantAccept {
		t.Fatalf("Sec-WebSocket-Accept = %q, want %q", got, wantAccept)
	}
	return conn, &wsConn{rwc: conn, br: br}, resp
}

func TestWsUpgrade_HandshakeAndTextFrame(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		conn, err := wsUpgrade(w, r)
		if err != nil {
			t.Errorf("wsUpgrade: %v", err)
			return
		}
		defer conn.Close()
		if err := conn.WriteJSON(map[string]string{"hello": "world"}); err != nil {
			t.Errorf("WriteJSON: %v", err)
			return
		}
		// Wait for the client's close frame so Close() doesn't race the
		// client's read.
		_, _, _ = conn.ReadMessage()
	})
	srv := httptest.NewServer(mux)
	defer srv.Close()

	conn, wc, _ := dialWS(t, srv, "/ws")
	defer conn.Close()

	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))
	opcode, payload, err := wc.ReadMessage()
	if err != nil {
		t.Fatalf("ReadMessage: %v", err)
	}
	if opcode != wsOpText {
		t.Fatalf("opcode = %d, want text (%d)", opcode, wsOpText)
	}
	var got map[string]string
	if err := json.Unmarshal(payload, &got); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if got["hello"] != "world" {
		t.Fatalf("payload = %v, want hello=world", got)
	}

	if err := writeMaskedFrame(conn, wsOpClose, []byte{0x03, 0xE8}); err != nil {
		t.Fatalf("write close: %v", err)
	}
}

func TestWsUpgrade_ReadsMaskedClientFrame(t *testing.T) {
	received := make(chan string, 1)
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		conn, err := wsUpgrade(w, r)
		if err != nil {
			t.Errorf("wsUpgrade: %v", err)
			return
		}
		defer conn.Close()
		opcode, payload, err := conn.ReadMessage()
		if err != nil {
			t.Errorf("ReadMessage: %v", err)
			return
		}
		if opcode == wsOpText {
			received <- string(payload)
		}
	})
	srv := httptest.NewServer(mux)
	defer srv.Close()

	conn, _, _ := dialWS(t, srv, "/ws")
	defer conn.Close()

	if err := writeMaskedFrame(conn, wsOpText, []byte("ping from client")); err != nil {
		t.Fatalf("write: %v", err)
	}

	select {
	case s := <-received:
		if s != "ping from client" {
			t.Fatalf("server received %q, want %q", s, "ping from client")
		}
	case <-time.After(5 * time.Second):
		t.Fatal("timed out waiting for server to read masked frame")
	}
}

func TestWsUpgrade_RejectsNonUpgradeRequest(t *testing.T) {
	rr := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/ws", nil)
	// Deliberately no Upgrade/Connection headers.
	_, err := wsUpgrade(rr, req)
	if err == nil {
		t.Fatal("expected error for a non-upgrade request")
	}
}

func TestWsFrame_LargePayloadRoundTrip(t *testing.T) {
	// Exercises the 16-bit and 64-bit extended-length header paths.
	mux := http.NewServeMux()
	mux.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		conn, err := wsUpgrade(w, r)
		if err != nil {
			t.Errorf("wsUpgrade: %v", err)
			return
		}
		defer conn.Close()
		payload := make([]byte, 70000) // > 65535 forces the 64-bit length path
		for i := range payload {
			payload[i] = byte(i)
		}
		if err := conn.writeFrame(wsOpBinary, payload); err != nil {
			t.Errorf("writeFrame: %v", err)
		}
	})
	srv := httptest.NewServer(mux)
	defer srv.Close()

	conn, wc, _ := dialWS(t, srv, "/ws")
	defer conn.Close()
	_ = conn.SetReadDeadline(time.Now().Add(5 * time.Second))

	opcode, payload, err := wc.ReadMessage()
	if err != nil {
		t.Fatalf("ReadMessage: %v", err)
	}
	if opcode != wsOpBinary {
		t.Fatalf("opcode = %d, want binary", opcode)
	}
	if len(payload) != 70000 {
		t.Fatalf("len(payload) = %d, want 70000", len(payload))
	}
	for i := range payload {
		if payload[i] != byte(i) {
			t.Fatalf("payload[%d] = %d, want %d", i, payload[i], byte(i))
		}
	}
}
