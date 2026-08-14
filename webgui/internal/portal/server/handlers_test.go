// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package server

import (
	"bytes"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"

	"mayhemb200/webgui/internal/portal/client"
)

func doReq(t *testing.T, s *Server, method, path string) *httptest.ResponseRecorder {
	t.Helper()
	req := httptest.NewRequest(method, path, bytes.NewReader(nil))
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

// --- happy paths, against fakeBackend -------------------------------------

func TestHandleApps_Success(t *testing.T) {
	fb := &fakeBackend{apps: []client.App{
		{ID: "adsbrx", DisplayName: "ADS-B RX", Category: "Receive"},
		{ID: "tuner", DisplayName: "Tuner", Category: "Receive"},
		{ID: "battleship", DisplayName: "Battleship", Category: "Games"},
	}}
	s := New(fb, withAssets(minimalAssets(t)))

	rr := doReq(t, s, http.MethodGet, "/api/apps")
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200, body=%s", rr.Code, rr.Body.String())
	}
	body := decodeBody[appsResponse](t, rr)
	// The Games entry is filtered out of BOTH the flat list and the groups:
	// the portal deliberately does not offer games (appindex.excludedCategories).
	if len(body.Apps) != 2 {
		t.Fatalf("Apps = %+v, want 2 entries (the game excluded)", body.Apps)
	}
	for _, a := range body.Apps {
		if a.ID == "battleship" {
			t.Fatalf("game leaked into the flat Apps list: %+v", body.Apps)
		}
	}
	if len(body.Groups) != 1 {
		t.Fatalf("Groups = %+v, want 1 category (the game excluded)", body.Groups)
	}
	if body.Groups[0].Category != "Receive" {
		t.Fatalf("Groups[0].Category = %q, want Receive first (canonical order)", body.Groups[0].Category)
	}
}

func TestHandleApps_NilSliceBecomesEmptyArray(t *testing.T) {
	fb := &fakeBackend{apps: nil}
	s := New(fb, withAssets(minimalAssets(t)))
	rr := doReq(t, s, http.MethodGet, "/api/apps")
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	// Must literally serialize as [] not null, so app.js's Array.isArray
	// checks and .filter/.forEach calls never see null.
	if !bytes.Contains(rr.Body.Bytes(), []byte(`"apps":[]`)) {
		t.Fatalf("body = %s, want apps:[] not null", rr.Body.String())
	}
	if !bytes.Contains(rr.Body.Bytes(), []byte(`"groups":[]`)) {
		t.Fatalf("body = %s, want groups:[] not null", rr.Body.String())
	}
}

func TestHandleCurrentApp_Success(t *testing.T) {
	fb := &fakeBackend{current: client.CurrentApp{ID: "adsbrx", Title: "ADS-B RX", CanGoBack: true}}
	s := New(fb, withAssets(minimalAssets(t)))
	rr := doReq(t, s, http.MethodGet, "/api/apps/current")
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	body := decodeBody[client.CurrentApp](t, rr)
	if body.ID != "adsbrx" {
		t.Fatalf("current = %+v", body)
	}
}

func TestHandleLaunch_Success(t *testing.T) {
	fb := &fakeBackend{}
	s := New(fb, withAssets(minimalAssets(t)))
	rr := doReq(t, s, http.MethodPost, "/api/apps/adsbrx/launch")
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200, body=%s", rr.Code, rr.Body.String())
	}
	if fb.lastLaunch != "adsbrx" {
		t.Fatalf("lastLaunch = %q, want adsbrx", fb.lastLaunch)
	}
	body := decodeBody[client.CurrentApp](t, rr)
	if body.ID != "adsbrx" {
		t.Fatalf("current = %+v", body)
	}
}

func TestHandleLaunch_IDWithSpecialCharsRoutes(t *testing.T) {
	fb := &fakeBackend{}
	s := New(fb, withAssets(minimalAssets(t)))
	// Percent-encoded id segment must still route to the launch handler and
	// decode back to the original id.
	rr := doReq(t, s, http.MethodPost, "/api/apps/weird%20id/launch")
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200, body=%s", rr.Code, rr.Body.String())
	}
	if fb.lastLaunch != "weird id" {
		t.Fatalf("lastLaunch = %q, want %q", fb.lastLaunch, "weird id")
	}
}

func TestHandleHome_Success(t *testing.T) {
	fb := &fakeBackend{}
	s := New(fb, withAssets(minimalAssets(t)))
	rr := doReq(t, s, http.MethodPost, "/api/apps/home")
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	if !fb.homeCalled {
		t.Fatalf("Home was not called on the backend")
	}
}

func TestHandlePanel_Success(t *testing.T) {
	fb := &fakeBackend{panel: client.Panel{AppID: "adsbrx", PanelKind: "table", Data: json.RawMessage(`{"columns":["a"],"rows":[["1"]]}`)}}
	s := New(fb, withAssets(minimalAssets(t)))
	rr := doReq(t, s, http.MethodGet, "/api/panel")
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	body := decodeBody[client.Panel](t, rr)
	if body.PanelKind != "table" {
		t.Fatalf("panel = %+v", body)
	}
}

func TestHandleStatus_Success(t *testing.T) {
	fb := &fakeBackend{status: client.Status{Device: "B200", Receiving: true}}
	s := New(fb, withAssets(minimalAssets(t)))
	rr := doReq(t, s, http.MethodGet, "/api/status")
	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	body := decodeBody[client.Status](t, rr)
	if body.Device != "B200" || !body.Receiving {
		t.Fatalf("status = %+v", body)
	}
}

func TestHandleLaunch_MissingID(t *testing.T) {
	fb := &fakeBackend{}
	s := New(fb, withAssets(minimalAssets(t)))
	// No id segment at all -> ServeMux itself won't even match this
	// pattern to handleLaunch; it should 404, not panic.
	rr := doReq(t, s, http.MethodPost, "/api/apps//launch")
	if rr.Code == http.StatusInternalServerError {
		t.Fatalf("status = 500 (panic path?) for an empty id segment")
	}
}

// --- generic (non *client.Error) failures from the Backend -----------------

func TestHandleApps_UnexpectedErrorType(t *testing.T) {
	fb := &fakeBackend{appsErr: errBoom}
	s := New(fb, withAssets(minimalAssets(t)))
	rr := doReq(t, s, http.MethodGet, "/api/apps")
	if rr.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500 for a non-*client.Error failure", rr.Code)
	}
	body := decodeBody[errorResponse](t, rr)
	if body.Error == "" {
		t.Fatalf("error message is empty")
	}
}

// --- real *client.Error translation, against an httptest fake mayhem-b200 --

func newClientPointedAt(url string) *client.Client {
	return client.New(url, time.Second)
}

func TestWriteBackendError_Unavailable(t *testing.T) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("net.Listen: %v", err)
	}
	addr := l.Addr().String()
	l.Close() // guaranteed nothing listening there now

	s := New(newClientPointedAt("http://"+addr), withAssets(minimalAssets(t)))
	rr := doReq(t, s, http.MethodGet, "/api/apps")

	if rr.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503, body=%s", rr.Code, rr.Body.String())
	}
	body := decodeBody[errorResponse](t, rr)
	if body.Code != "backend_unavailable" {
		t.Fatalf("code = %q, want backend_unavailable", body.Code)
	}
	if body.Error == "" {
		t.Fatalf("error message is empty")
	}
}

func TestWriteBackendError_PassthroughStatusAndMessage(t *testing.T) {
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusNotFound)
		w.Write([]byte(`{"error":"unknown app id \"bogus\""}`))
	}))
	defer backend.Close()

	s := New(newClientPointedAt(backend.URL), withAssets(minimalAssets(t)))
	rr := doReq(t, s, http.MethodPost, "/api/apps/bogus/launch")

	if rr.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404, body=%s", rr.Code, rr.Body.String())
	}
	body := decodeBody[errorResponse](t, rr)
	if body.Error != `unknown app id "bogus"` {
		t.Fatalf("error = %q, want the backend's exact message", body.Error)
	}
}

func TestWriteBackendError_DecodeFailure(t *testing.T) {
	backend := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{not valid json`))
	}))
	defer backend.Close()

	s := New(newClientPointedAt(backend.URL), withAssets(minimalAssets(t)))
	rr := doReq(t, s, http.MethodGet, "/api/status")

	if rr.Code != http.StatusBadGateway {
		t.Fatalf("status = %d, want 502, body=%s", rr.Code, rr.Body.String())
	}
	body := decodeBody[errorResponse](t, rr)
	if body.Code != "backend_bad_response" {
		t.Fatalf("code = %q, want backend_bad_response", body.Code)
	}
}

// --- end-to-end sanity against a real net/http.Server -----------------------

func TestRealHTTPServer_ServesAPIAndAssets(t *testing.T) {
	fb := &fakeBackend{apps: []client.App{{ID: "a", DisplayName: "A", Category: "Receive"}}}
	s := New(fb, withAssets(minimalAssets(t)))
	ts := httptest.NewServer(s)
	defer ts.Close()

	res, err := http.Get(ts.URL + "/api/apps")
	if err != nil {
		t.Fatalf("GET /api/apps: %v", err)
	}
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200", res.StatusCode)
	}

	res2, err := http.Get(ts.URL + "/")
	if err != nil {
		t.Fatalf("GET /: %v", err)
	}
	defer res2.Body.Close()
	if res2.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200", res2.StatusCode)
	}
}

// The Morse panel's Transmit button is the one browser path that keys the
// radio, so the proxy must forward the operator's exact text/wpm and relay
// the backend's answer verbatim — including a refusal (ok:false), which is
// the radio declining, not a transport error.
func TestMorseTransmitForwardsAndRelays(t *testing.T) {
	fb := &fakeBackend{morseResult: client.MorseTransmitResult{Ok: true, DurationMs: 4200, FrequencyHz: 144200000}}
	srv := New(fb)

	rr := httptest.NewRecorder()
	srv.ServeHTTP(rr, httptest.NewRequest(http.MethodPost, "/api/morse/transmit",
		bytes.NewReader([]byte(`{"text":"CQ CQ DE M0ABC","wpm":22}`))))

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", rr.Code)
	}
	if fb.morseText != "CQ CQ DE M0ABC" || fb.morseWpm != 22 {
		t.Errorf("backend got text=%q wpm=%d, want the operator's exact input", fb.morseText, fb.morseWpm)
	}
	got := decodeBody[client.MorseTransmitResult](t, rr)
	if !got.Ok || got.DurationMs != 4200 {
		t.Errorf("relayed %+v, want the backend's ok:true / 4200 ms", got)
	}
}

func TestMorseTransmitRelaysARefusal(t *testing.T) {
	// A receive-only or busy radio answers ok:false with a reason. That is a
	// 200 with the reason intact, not an HTTP error — the browser branches on
	// Ok and shows the string.
	fb := &fakeBackend{morseResult: client.MorseTransmitResult{Ok: false, Error: "no transmit radio"}}
	srv := New(fb)

	rr := httptest.NewRecorder()
	srv.ServeHTTP(rr, httptest.NewRequest(http.MethodPost, "/api/morse/transmit",
		bytes.NewReader([]byte(`{"text":"CQ","wpm":18}`))))

	if rr.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (a refusal is still a well-formed answer)", rr.Code)
	}
	got := decodeBody[client.MorseTransmitResult](t, rr)
	if got.Ok || got.Error != "no transmit radio" {
		t.Errorf("relayed %+v, want ok:false with the reason preserved", got)
	}
}
