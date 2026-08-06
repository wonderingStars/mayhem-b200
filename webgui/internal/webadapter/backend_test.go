// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package webadapter

import (
	"context"
	"errors"
	"testing"
	"time"

	"mayhemb200/webgui/internal/web"
)

func TestBackendListOpenCurrentCloseSessionsUptime(t *testing.T) {
	b, _, _ := newTestBackend(t, "")
	ctx := context.Background()

	infos, err := b.ListDevices(ctx)
	if err != nil {
		t.Fatalf("ListDevices: %v", err)
	}
	if len(infos) != 1 || infos[0].Driver != "fake" {
		t.Fatalf("ListDevices = %+v, want one fake device", infos)
	}

	if _, ok := b.CurrentDevice(); ok {
		t.Fatal("CurrentDevice reported a device open before OpenDevice was called")
	}

	dev, err := b.OpenDevice(ctx, infos[0].Args)
	if err != nil {
		t.Fatalf("OpenDevice: %v", err)
	}
	if dev.Caps().Driver != "fake" {
		t.Errorf("opened device Caps().Driver = %q, want fake", dev.Caps().Driver)
	}

	cur, ok := b.CurrentDevice()
	if !ok || cur.Info().Driver != "fake" {
		t.Fatalf("CurrentDevice = (%+v, %v), want the fake device", cur, ok)
	}

	if err := b.CloseDevice(); err != nil {
		t.Fatalf("CloseDevice: %v", err)
	}
	if _, ok := b.CurrentDevice(); ok {
		t.Fatal("CurrentDevice still reports a device open after CloseDevice")
	}

	sessions := b.Sessions()
	if len(sessions) != 1 {
		t.Fatalf("Sessions() = %+v, want exactly one entry (this client's own session)", sessions)
	}
	if sessions[0].Client != "test-client" {
		t.Errorf("Sessions()[0].Client = %q, want test-client", sessions[0].Client)
	}
	if sessions[0].DeviceOpen {
		t.Error("Sessions()[0].DeviceOpen = true after CloseDevice, want false")
	}

	if up := b.Uptime(); up < 0 {
		t.Errorf("Uptime() = %v, want >= 0", up)
	}
}

func TestBackendOpenDeviceLocalGuardWhenAlreadyOpen(t *testing.T) {
	b, _, _ := newTestBackend(t, "")
	ctx := context.Background()

	if _, err := b.OpenDevice(ctx, "driver=fake"); err != nil {
		t.Fatalf("first OpenDevice: %v", err)
	}
	if _, err := b.OpenDevice(ctx, "driver=fake"); !errors.Is(err, web.ErrDeviceInUse) {
		t.Fatalf("second OpenDevice error = %v, want web.ErrDeviceInUse", err)
	}
}

// TestBackendOpenDeviceTranslatesServerSideDeviceInUse simulates a device
// already held open by some OTHER session (not this Backend's own local
// guard, which never fires here since b.current starts nil): the fake
// server's own "device in use" reply must come back as web.ErrDeviceInUse,
// matchable with errors.Is, exactly as PROTOCOL.md section 4 promises for
// the wire error string.
func TestBackendOpenDeviceTranslatesServerSideDeviceInUse(t *testing.T) {
	b, _, fd := newTestBackend(t, "")
	fd.mu.Lock()
	fd.open = true // as if another session already has it
	fd.mu.Unlock()

	_, err := b.OpenDevice(context.Background(), "driver=fake")
	if !errors.Is(err, web.ErrDeviceInUse) {
		t.Fatalf("OpenDevice error = %v, want web.ErrDeviceInUse", err)
	}
	if _, ok := b.CurrentDevice(); ok {
		t.Fatal("CurrentDevice reports a device open after a failed OpenDevice")
	}
}

func TestBackendCloseDeviceClosesFanoutSubscribers(t *testing.T) {
	b, _, _ := newTestBackend(t, "")
	ctx := context.Background()

	dev, err := b.OpenDevice(ctx, "driver=fake")
	if err != nil {
		t.Fatalf("OpenDevice: %v", err)
	}
	sub, unsub := dev.Subscribe(1)
	defer unsub()

	if err := b.CloseDevice(); err != nil {
		t.Fatalf("CloseDevice: %v", err)
	}

	select {
	case _, ok := <-sub:
		if ok {
			t.Fatal("subscriber channel delivered a value after CloseDevice, want it closed")
		}
	case <-time.After(2 * time.Second):
		t.Fatal("subscriber channel was not closed by CloseDevice")
	}
}

func TestBackendCloseDeviceIsNoOpWhenNothingOpen(t *testing.T) {
	b, _, _ := newTestBackend(t, "")
	if err := b.CloseDevice(); err != nil {
		t.Fatalf("CloseDevice with nothing open: %v", err)
	}
}
