// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200. Ported from sdrlink's internal/webadapter (MIT) and
// relicensed here by its author/copyright holder into this
// GPL-2.0-or-later project; unchanged apart from the import path.

package webadapter

import (
	"testing"
	"time"

	"mayhemb200/webgui/internal/web"
)

func TestFanoutTwoSubscribersBothReceive(t *testing.T) {
	f := newFanout()
	subA, unsubA := f.subscribe(4)
	defer unsubA()
	subB, unsubB := f.subscribe(4)
	defer unsubB()

	want := web.Samples{RateHz: 1, SeqHz: 2}
	f.broadcast(want)

	for name, ch := range map[string]<-chan web.Samples{"A": subA, "B": subB} {
		select {
		case got := <-ch:
			if got.RateHz != want.RateHz || got.SeqHz != want.SeqHz {
				t.Errorf("subscriber %s got %+v, want %+v", name, got, want)
			}
		case <-time.After(time.Second):
			t.Errorf("subscriber %s: timed out waiting for broadcast", name)
		}
	}
}

// TestFanoutSlowSubscriberDropsWithoutBlockingProducer checks the mandatory
// property from the task: a subscriber that isn't draining its channel has
// batches DROPPED for it once the buffer is full, and broadcast to it never
// blocks the producer -- verified here by bounding the whole broadcast
// sequence with a short deadline instead of letting a hang time out the
// whole test suite.
func TestFanoutSlowSubscriberDropsWithoutBlockingProducer(t *testing.T) {
	f := newFanout()
	slow, unsubSlow := f.subscribe(1) // buf=1, never drained during the test
	defer unsubSlow()
	fast, unsubFast := f.subscribe(4)
	defer unsubFast()

	done := make(chan struct{})
	go func() {
		defer close(done)
		f.broadcast(web.Samples{SeqHz: 1}) // fills slow's buffer
		f.broadcast(web.Samples{SeqHz: 2}) // must be dropped for slow, not block
		f.broadcast(web.Samples{SeqHz: 3}) // must also be dropped, not block
	}()

	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("broadcast blocked on a slow subscriber instead of dropping")
	}

	// slow only ever got the first batch; the buffer never drained so the
	// second and third were dropped by the non-blocking select in broadcast.
	select {
	case got := <-slow:
		if got.SeqHz != 1 {
			t.Errorf("slow subscriber's only batch has SeqHz=%v, want 1 (the first)", got.SeqHz)
		}
	default:
		t.Fatal("slow subscriber received nothing, want the first batch")
	}
	select {
	case extra := <-slow:
		t.Errorf("slow subscriber received a second batch (SeqHz=%v); dropped batches must not be queued", extra.SeqHz)
	default:
		// Expected: nothing further queued for the slow subscriber.
	}

	// fast, by contrast, received all three since its buffer never filled.
	for i, want := range []float64{1, 2, 3} {
		select {
		case got := <-fast:
			if got.SeqHz != want {
				t.Errorf("fast subscriber batch %d: SeqHz=%v, want %v", i, got.SeqHz, want)
			}
		default:
			t.Errorf("fast subscriber missing batch %d (SeqHz=%v)", i, want)
		}
	}
}

func TestFanoutUnsubscribeIsIdempotentAndClosesChannel(t *testing.T) {
	f := newFanout()
	sub, unsub := f.subscribe(1)

	unsub()
	unsub() // must not panic

	select {
	case _, ok := <-sub:
		if ok {
			t.Fatal("channel delivered a value after unsubscribe, want it closed with no value")
		}
	case <-time.After(time.Second):
		t.Fatal("channel was not closed by unsubscribe")
	}

	// A broadcast after unsubscribe must not panic (send-on-closed-channel
	// would panic if the fanout still held a reference to it).
	f.broadcast(web.Samples{})
}
