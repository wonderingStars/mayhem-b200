// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200. Originally written for sdrlink (MIT) and relicensed
// here by its author/copyright holder into this GPL-2.0-or-later project.

package web

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

// doJSON drives Server.ServeHTTP directly (no real network needed for plain
// HTTP endpoints) and returns the recorder for the caller to assert on.
func doJSON(t *testing.T, s *Server, method, path string, body any) *httptest.ResponseRecorder {
	t.Helper()
	var reader *bytes.Reader
	if body != nil {
		b, err := json.Marshal(body)
		if err != nil {
			t.Fatalf("marshal request body: %v", err)
		}
		reader = bytes.NewReader(b)
	} else {
		reader = bytes.NewReader(nil)
	}
	req := httptest.NewRequest(method, path, reader)
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	rr := httptest.NewRecorder()
	s.ServeHTTP(rr, req)
	return rr
}

func decodeBody[T any](t *testing.T, rr *httptest.ResponseRecorder) T {
	t.Helper()
	var v T
	if err := json.Unmarshal(rr.Body.Bytes(), &v); err != nil {
		t.Fatalf("decode response body %q: %v", rr.Body.String(), err)
	}
	return v
}

func newTestServer() *Server {
	return NewServer(newFakeBackend())
}

// --- device-not-open error path (explicitly required by the task) ---------

func TestDeviceNotOpen_ErrorPath(t *testing.T) {
	s := newTestServer()

	cases := []struct {
		method, path string
		body         any
	}{
		{http.MethodGet, "/api/caps", nil},
		{http.MethodGet, "/api/state", nil},
		{http.MethodGet, "/api/stats", nil},
		{http.MethodPost, "/api/rx/freq", map[string]float64{"hz": 100e6}},
		{http.MethodPost, "/api/tx/freq", map[string]float64{"hz": 100e6}},
		{http.MethodPost, "/api/rx/rate", map[string]float64{"hz": 2e6}},
		{http.MethodPost, "/api/rx/gain", map[string]float64{"db": 20}},
		{http.MethodPost, "/api/rx/bandwidth", map[string]float64{"hz": 2e6}},
		{http.MethodPost, "/api/rx/antenna", map[string]string{"name": "RX2"}},
		{http.MethodPost, "/api/lo_offset", map[string]float64{"hz": 0}},
		{http.MethodPost, "/api/rx/agc", map[string]bool{"on": true}},
		{http.MethodPost, "/api/rx/start", map[string]string{"format": "cf32"}},
		{http.MethodPost, "/api/rx/stop", nil},
	}

	for _, c := range cases {
		t.Run(c.method+" "+c.path, func(t *testing.T) {
			rr := doJSON(t, s, c.method, c.path, c.body)
			if rr.Code != http.StatusConflict {
				t.Fatalf("status = %d, want 409 (body: %s)", rr.Code, rr.Body.String())
			}
			er := decodeBody[errorResponse](t, rr)
			if er.Error != ErrNoDevice.Error() {
				t.Fatalf("error = %q, want %q", er.Error, ErrNoDevice.Error())
			}
		})
	}
}

// closeDevice never requires a device to be open — verify it's a no-op then.
func TestCloseDevice_NoopWhenNotOpen(t *testing.T) {
	s := newTestServer()
	rr := doJSON(t, s, http.MethodPost, "/api/devices/close", nil)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (body: %s)", rr.Code, rr.Body.String())
	}
}

// --- discovery & lifecycle --------------------------------------------------

func TestListDevices(t *testing.T) {
	s := newTestServer()
	rr := doJSON(t, s, http.MethodGet, "/api/devices", nil)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	got := decodeBody[struct {
		Devices []DeviceInfo `json:"devices"`
	}](t, rr)
	if len(got.Devices) != 1 || got.Devices[0].Driver != "fake" {
		t.Fatalf("devices = %+v, want one sim device", got.Devices)
	}
}

func TestOpenCloseFlow(t *testing.T) {
	s := newTestServer()

	openRR := doJSON(t, s, http.MethodPost, "/api/devices/open", map[string]string{"args": "driver=fake"})
	if openRR.Code != http.StatusOK {
		t.Fatalf("open status = %d, want 200 (body: %s)", openRR.Code, openRR.Body.String())
	}
	openResp := decodeBody[struct {
		Caps Caps `json:"caps"`
	}](t, openRR)
	if !openResp.Caps.HasTx || !openResp.Caps.HasRx || !openResp.Caps.FullDuplex {
		t.Fatalf("caps = %+v, want has_rx/has_tx/full_duplex all true", openResp.Caps)
	}
	if openResp.Caps.RxFreq.Max <= openResp.Caps.RxFreq.Min {
		t.Fatalf("rx_freq range invalid: %+v", openResp.Caps.RxFreq)
	}

	// A second open while one is active must fail with device-in-use.
	second := doJSON(t, s, http.MethodPost, "/api/devices/open", map[string]string{"args": "driver=fake"})
	if second.Code != http.StatusConflict {
		t.Fatalf("second open status = %d, want 409", second.Code)
	}
	er := decodeBody[errorResponse](t, second)
	if er.Error != ErrDeviceInUse.Error() {
		t.Fatalf("second open error = %q, want %q", er.Error, ErrDeviceInUse.Error())
	}

	capsRR := doJSON(t, s, http.MethodGet, "/api/caps", nil)
	if capsRR.Code != http.StatusOK {
		t.Fatalf("caps status = %d, want 200", capsRR.Code)
	}

	stateRR := doJSON(t, s, http.MethodGet, "/api/state", nil)
	if stateRR.Code != http.StatusOK {
		t.Fatalf("state status = %d, want 200", stateRR.Code)
	}
	state := decodeBody[State](t, stateRR)
	if state.Streaming {
		t.Fatalf("freshly opened device should not be streaming: %+v", state)
	}

	closeRR := doJSON(t, s, http.MethodPost, "/api/devices/close", nil)
	if closeRR.Code != http.StatusOK {
		t.Fatalf("close status = %d, want 200", closeRR.Code)
	}

	// After close, device-scoped endpoints are 409 again.
	afterClose := doJSON(t, s, http.MethodGet, "/api/state", nil)
	if afterClose.Code != http.StatusConflict {
		t.Fatalf("state after close status = %d, want 409", afterClose.Code)
	}
}

// --- setters return the ACTUAL accepted value, not the request ------------

func openDevice(t *testing.T, s *Server) {
	t.Helper()
	rr := doJSON(t, s, http.MethodPost, "/api/devices/open", map[string]string{"args": "driver=fake"})
	if rr.Code != http.StatusOK {
		t.Fatalf("open failed: %d %s", rr.Code, rr.Body.String())
	}
}

func TestSetRxFreq_ReturnsQuantizedActualValue(t *testing.T) {
	s := newTestServer()
	openDevice(t, s)

	requested := 100_000_000.7 // rx_freq step is 1 Hz -> must round
	rr := doJSON(t, s, http.MethodPost, "/api/rx/freq", map[string]float64{"hz": requested})
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (body: %s)", rr.Code, rr.Body.String())
	}
	got := decodeBody[hzResult](t, rr)
	if got.Hz == requested {
		t.Fatalf("actual (%v) must not equal the raw request when it doesn't land on a hardware step", got.Hz)
	}
	if got.Hz != 100_000_001 {
		t.Fatalf("actual = %v, want 100000001 (nearest 1 Hz step)", got.Hz)
	}

	// GET /api/state must reflect the accepted value, not the request.
	stateRR := doJSON(t, s, http.MethodGet, "/api/state", nil)
	state := decodeBody[State](t, stateRR)
	if state.RxFreqHz != got.Hz {
		t.Fatalf("state.rx_freq_hz = %v, want %v (the accepted value)", state.RxFreqHz, got.Hz)
	}
}

func TestSetTxGain_QuantizesToPointTwoDbStep(t *testing.T) {
	s := newTestServer()
	openDevice(t, s)

	rr := doJSON(t, s, http.MethodPost, "/api/tx/gain", map[string]float64{"db": 10.05})
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (body: %s)", rr.Code, rr.Body.String())
	}
	got := decodeBody[dbResult](t, rr)
	if !approxEqual(got.Db, 10.0, 1e-9) {
		t.Fatalf("actual = %v, want 10.0 (nearest 0.2 dB step)", got.Db)
	}
}

func TestSetRxAntenna_InvalidNameRejected(t *testing.T) {
	s := newTestServer()
	openDevice(t, s)

	rr := doJSON(t, s, http.MethodPost, "/api/rx/antenna", map[string]string{"name": "BOGUS"})
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 (body: %s)", rr.Code, rr.Body.String())
	}
}

func TestSetRxAntenna_ValidNameAccepted(t *testing.T) {
	s := newTestServer()
	openDevice(t, s)

	rr := doJSON(t, s, http.MethodPost, "/api/rx/antenna", map[string]string{"name": "TX/RX"})
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (body: %s)", rr.Code, rr.Body.String())
	}
	got := decodeBody[nameResult](t, rr)
	if got.Name != "TX/RX" {
		t.Fatalf("name = %q, want TX/RX", got.Name)
	}
}

func TestBoolSetters(t *testing.T) {
	s := newTestServer()
	openDevice(t, s)

	for _, path := range []string{"/api/rx/agc", "/api/rx/dc_offset_auto", "/api/rx/iq_balance_auto"} {
		rr := doJSON(t, s, http.MethodPost, path, map[string]bool{"on": true})
		if rr.Code != http.StatusOK {
			t.Fatalf("%s status = %d, want 200 (body: %s)", path, rr.Code, rr.Body.String())
		}
		got := decodeBody[boolResult](t, rr)
		if !got.On {
			t.Fatalf("%s: on = false, want true", path)
		}
	}
}

// --- streaming control -------------------------------------------------------

func TestStartStopRx(t *testing.T) {
	s := newTestServer()
	openDevice(t, s)

	startRR := doJSON(t, s, http.MethodPost, "/api/rx/start", map[string]string{"format": "ci16"})
	if startRR.Code != http.StatusOK {
		t.Fatalf("start status = %d, want 200 (body: %s)", startRR.Code, startRR.Body.String())
	}
	started := decodeBody[startRxResult](t, startRR)
	if started.Format != "ci16" {
		t.Fatalf("format = %q, want ci16", started.Format)
	}

	stateRR := doJSON(t, s, http.MethodGet, "/api/state", nil)
	state := decodeBody[State](t, stateRR)
	if !state.Streaming || state.Format != "ci16" {
		t.Fatalf("state = %+v, want streaming=true format=ci16", state)
	}

	stopRR := doJSON(t, s, http.MethodPost, "/api/rx/stop", nil)
	if stopRR.Code != http.StatusOK {
		t.Fatalf("stop status = %d, want 200", stopRR.Code)
	}

	stateRR2 := doJSON(t, s, http.MethodGet, "/api/state", nil)
	state2 := decodeBody[State](t, stateRR2)
	if state2.Streaming {
		t.Fatalf("state after stop = %+v, want streaming=false", state2)
	}
}

func TestStartRx_InvalidFormatRejected(t *testing.T) {
	s := newTestServer()
	openDevice(t, s)

	rr := doJSON(t, s, http.MethodPost, "/api/rx/start", map[string]string{"format": "bogus"})
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 (body: %s)", rr.Code, rr.Body.String())
	}
}

// --- status ------------------------------------------------------------------

func TestStatus_ReflectsDeviceLifecycle(t *testing.T) {
	s := newTestServer()

	rr := doJSON(t, s, http.MethodGet, "/api/status", nil)
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	st := decodeBody[ServerStatus](t, rr)
	if st.DeviceOpen {
		t.Fatalf("device_open = true before any device is opened")
	}
	if st.Stats != nil {
		t.Fatalf("stats = %+v, want nil before a device is opened", st.Stats)
	}
	if st.Sessions == nil {
		t.Fatalf("sessions must be an empty slice, not null, when there are none")
	}

	openDevice(t, s)
	rr2 := doJSON(t, s, http.MethodGet, "/api/status", nil)
	st2 := decodeBody[ServerStatus](t, rr2)
	if !st2.DeviceOpen {
		t.Fatalf("device_open = false after opening a device")
	}
	if st2.Stats == nil {
		t.Fatalf("stats = nil, want non-nil once a device is open")
	}
	if len(st2.Sessions) != 1 {
		t.Fatalf("sessions = %+v, want exactly one", st2.Sessions)
	}
}

// --- malformed request bodies ------------------------------------------------

func TestMalformedJSON_Returns400(t *testing.T) {
	s := newTestServer()
	openDevice(t, s)

	req := httptest.NewRequest(http.MethodPost, "/api/rx/freq", bytes.NewReader([]byte("{not json")))
	req.Header.Set("Content-Type", "application/json")
	rr := httptest.NewRecorder()
	s.ServeHTTP(rr, req)
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("status = %d, want 400 (body: %s)", rr.Code, rr.Body.String())
	}
}
