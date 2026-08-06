// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package server

import (
	"context"
	"errors"

	"mayhemb200/webgui/internal/portal/client"
)

// fakeBackend is an in-package double for Backend. Handler tests use it for
// the cases that don't need to exercise real *client.Error translation
// (that's covered separately in handlers_test.go against a real
// *client.Client pointed at an httptest server, since writeBackendError's
// whole job is unwrapping that concrete type).
type fakeBackend struct {
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
func (f *fakeBackend) Panel(ctx context.Context) (client.Panel, error) {
	return f.panel, f.panelErr
}
func (f *fakeBackend) Status(ctx context.Context) (client.Status, error) {
	return f.status, f.statusErr
}

var _ Backend = (*fakeBackend)(nil)
var errBoom = errors.New("boom")
