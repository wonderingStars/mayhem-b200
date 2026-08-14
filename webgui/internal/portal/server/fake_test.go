// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package server

import (
	"context"
	"encoding/json"
	"errors"
	"sync"

	"mayhemb200/webgui/internal/portal/client"
)

// fakeBackend is an in-package double for Backend. Handler tests use it for
// the cases that don't need to exercise real *client.Error translation
// (that's covered separately in handlers_test.go against a real
// *client.Client pointed at an httptest server, since writeBackendError's
// whole job is unwrapping that concrete type).
type fakeBackend struct {
	morseResult client.MorseTransmitResult
	morseErr    error
	morseText   string
	morseWpm    int
	apps       []client.App
	appsErr    error
	current    client.CurrentApp
	currentErr error
	launchErr  error
	lastLaunch string
	homeErr    error
	homeCalled bool
	panel      client.Panel
	panelErr   error
	status     client.Status
	statusErr  error

	// Screen/input state for the live-screen bridge (screen.go). Guarded by
	// a mutex because, unlike every other field here, these are touched from
	// the hub's own goroutines while the test's goroutine reads them.
	mu sync.Mutex
	// lastPanelRev records the have_image_rev the server forwarded.
	lastPanelRev string
	// screenFn, when set, answers each Screen call; nil means "no frame
	// yet" (204), which is what a backend with nothing drawn returns.
	screenFn func(ctx context.Context, after uint32, waitMS int) (client.ScreenFrame, bool, error)
	// inputBatches records every batch that reached the backend.
	inputBatches [][]json.RawMessage
	inputErr     error
}

func (f *fakeBackend) Apps(ctx context.Context) ([]client.App, error) {
	return f.apps, f.appsErr
}
func (f *fakeBackend) CurrentApp(ctx context.Context) (client.CurrentApp, error) {
	return f.current, f.currentErr
}
func (f *fakeBackend) Launch(ctx context.Context, id string) (client.CurrentApp, error) {
	f.lastLaunch = id
	if f.launchErr != nil {
		return client.CurrentApp{}, f.launchErr
	}
	return client.CurrentApp{ID: id, Title: id, CanGoBack: true}, nil
}
func (f *fakeBackend) Home(ctx context.Context) (client.CurrentApp, error) {
	f.homeCalled = true
	return client.CurrentApp{Title: "Home"}, f.homeErr
}
func (f *fakeBackend) Panel(ctx context.Context, haveImageRev string) (client.Panel, error) {
	f.mu.Lock()
	f.lastPanelRev = haveImageRev
	f.mu.Unlock()
	return f.panel, f.panelErr
}
func (f *fakeBackend) Status(ctx context.Context) (client.Status, error) {
	return f.status, f.statusErr
}

func (f *fakeBackend) MorseTransmit(ctx context.Context, text string, wpm int) (client.MorseTransmitResult, error) {
	f.morseText = text
	f.morseWpm = wpm
	return f.morseResult, f.morseErr
}

func (f *fakeBackend) Screen(ctx context.Context, after uint32, waitMS int) (client.ScreenFrame, bool, error) {
	f.mu.Lock()
	fn := f.screenFn
	f.mu.Unlock()
	if fn == nil {
		return client.ScreenFrame{}, false, nil
	}
	return fn(ctx, after, waitMS)
}

func (f *fakeBackend) Input(ctx context.Context, events []json.RawMessage) (client.InputResult, error) {
	f.mu.Lock()
	f.inputBatches = append(f.inputBatches, events)
	err := f.inputErr
	f.mu.Unlock()
	if err != nil {
		return client.InputResult{}, err
	}
	return client.InputResult{Queued: len(events)}, nil
}

// panelRev returns the have_image_rev last forwarded to Panel.
func (f *fakeBackend) panelRev() string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.lastPanelRev
}

// inputEvents returns every event that reached the backend, flattened.
func (f *fakeBackend) inputEvents() []string {
	f.mu.Lock()
	defer f.mu.Unlock()
	var out []string
	for _, batch := range f.inputBatches {
		for _, e := range batch {
			out = append(out, string(e))
		}
	}
	return out
}

var _ Backend = (*fakeBackend)(nil)
var errBoom = errors.New("boom")
