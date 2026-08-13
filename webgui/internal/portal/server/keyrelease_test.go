// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package server

import (
	"strings"
	"testing"
	"time"
)

// A key press is two events. The browser sends the release on blur, pagehide
// and unmount, but it cannot send anything down a socket that has already
// dropped — and a tab crashing or a link failing mid-press is exactly when a
// key is most likely to be down. The device's long-press timer is fed by both
// transitions, so a down with no up leaves ui::key_is_long_pressed() true for
// that key until someone presses and releases it again. These pin the release
// coming from the hub instead.
//
// In its own file, and using this package's existing wsClient/fakeBackend
// helpers rather than adding to screen_test.go, which another change-set may
// be editing.

// waitForEvent polls the fake backend until one of its received events
// contains want, or the deadline passes. Returns everything it saw, so a
// failure can print what did arrive instead of just "timed out".
func waitForEvent(t *testing.T, fb *fakeBackend, want string, timeout time.Duration) (bool, []string) {
	t.Helper()
	deadline := time.Now().Add(timeout)
	var seen []string
	for time.Now().Before(deadline) {
		seen = fb.inputEvents()
		for _, e := range seen {
			if strings.Contains(e, want) {
				return true, seen
			}
		}
		time.Sleep(2 * time.Millisecond)
	}
	return false, seen
}

func TestScreenWS_ReleasesAHeldKeyWhenTheLastClientDisconnects(t *testing.T) {
	// The case that matters most: one tab, one key down, socket gone. The
	// input loop is stopped as the last client leaves, so the release cannot
	// go through the queue — leave() sends it directly.
	fb := &fakeBackend{}
	ts := newScreenServer(t, fb)
	defer ts.Close()

	c := dialScreenWS(t, ts, "?deflate=0")
	if st := c.nextStatus(2 * time.Second); !st.Controlling {
		t.Fatalf("first client should be the controller, got %+v", st)
	}

	c.sendEvents(`{"events":[{"type":"key","key":"select","down":true}]}`)
	if ok, seen := waitForEvent(t, fb, `"down":true`, 2*time.Second); !ok {
		t.Fatalf("the key-down never reached the backend; saw %q", seen)
	}

	c.close()

	ok, seen := waitForEvent(t, fb, `"key":"select","down":false`, 2*time.Second)
	if !ok {
		t.Fatalf("no key-up for the held key after the socket dropped; backend saw %q", seen)
	}
}

func TestScreenWS_ReleasesHeldKeysWhenTheControllerHandsOver(t *testing.T) {
	// With another client still attached the loops stay up, so the releases
	// go through the ordinary input queue. Two keys, to pin that all of them
	// are released and not just the last one recorded.
	fb := &fakeBackend{}
	ts := newScreenServer(t, fb)
	defer ts.Close()

	a := dialScreenWS(t, ts, "?deflate=0")
	if st := a.nextStatus(2 * time.Second); !st.Controlling {
		t.Fatalf("first client should be the controller, got %+v", st)
	}
	b := dialScreenWS(t, ts, "?deflate=0")
	defer b.close()
	if st := b.nextStatus(2 * time.Second); st.Controlling {
		t.Fatalf("second client should be read-only, got %+v", st)
	}

	a.sendEvents(`{"events":[{"type":"key","key":"up","down":true},` +
		`{"type":"key","key":"select","down":true}]}`)
	if ok, seen := waitForEvent(t, fb, `"key":"select","down":true`, 2*time.Second); !ok {
		t.Fatalf("the key-downs never reached the backend; saw %q", seen)
	}

	a.close()

	if ok, seen := waitForEvent(t, fb, `"key":"select","down":false`, 2*time.Second); !ok {
		t.Fatalf("select was never released; backend saw %q", seen)
	}
	if ok, seen := waitForEvent(t, fb, `"key":"up","down":false`, 2*time.Second); !ok {
		t.Fatalf("up was never released; backend saw %q", seen)
	}

	// And the promoted client is told it is now in charge.
	if st := b.nextStatus(2 * time.Second); !st.Controlling {
		t.Fatalf("longest-connected client was not promoted, got %+v", st)
	}
}

func TestScreenWS_DoesNotReleaseAKeyTheClientAlreadyReleased(t *testing.T) {
	// The tracking has to be a set, not a counter of presses: a clean
	// down/up pair must leave nothing to undo, or every disconnect would
	// press-and-release keys the operator had finished with.
	fb := &fakeBackend{}
	ts := newScreenServer(t, fb)
	defer ts.Close()

	c := dialScreenWS(t, ts, "?deflate=0")
	c.nextStatus(2 * time.Second)

	c.sendEvents(`{"events":[{"type":"key","key":"back","down":true},` +
		`{"type":"key","key":"back","down":false}]}`)
	if ok, seen := waitForEvent(t, fb, `"key":"back","down":false`, 2*time.Second); !ok {
		t.Fatalf("the down/up pair never reached the backend; saw %q", seen)
	}
	before := len(fb.inputEvents())

	c.close()
	// Long enough that a synthesized release would have shown up.
	time.Sleep(150 * time.Millisecond)

	if after := len(fb.inputEvents()); after != before {
		t.Fatalf("disconnect sent %d extra event(s) for a key that was already "+
			"released: %q", after-before, fb.inputEvents()[before:])
	}
}

func TestScreenWS_DoesNotReleaseKeysForAReadOnlyViewer(t *testing.T) {
	// A non-controlling client's input is ignored, so it never had a key
	// down on the device and must not synthesize a release for one — that
	// would let any spectator inject input by connecting and leaving.
	fb := &fakeBackend{}
	ts := newScreenServer(t, fb)
	defer ts.Close()

	a := dialScreenWS(t, ts, "?deflate=0")
	defer a.close()
	a.nextStatus(2 * time.Second)

	b := dialScreenWS(t, ts, "?deflate=0")
	b.nextStatus(2 * time.Second)
	b.sendEvents(`{"events":[{"type":"key","key":"select","down":true}]}`)
	time.Sleep(100 * time.Millisecond)
	b.close()
	time.Sleep(150 * time.Millisecond)

	for _, e := range fb.inputEvents() {
		if strings.Contains(e, "select") {
			t.Fatalf("a read-only viewer's key reached the backend: %q", e)
		}
	}
}
