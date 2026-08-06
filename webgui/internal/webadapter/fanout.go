// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200. Ported from sdrlink's internal/webadapter (MIT) and
// relicensed here by its author/copyright holder into this
// GPL-2.0-or-later project; unchanged apart from the import path.

package webadapter

import (
	"sync"

	"mayhemb200/webgui/internal/web"
)

// fanout distributes one producer's sample batches to any number of
// subscribers, registered and removed at any time. deviceAdapter's own
// stream-reading goroutine (streamLoop, in device.go) is the sole producer;
// web.Device.Subscribe callers (today: only the spectrum WebSocket, but the
// interface allows more) are the consumers.
//
// A slow subscriber's channel fills up and further batches for it are
// dropped rather than blocking broadcast -- and therefore never blocking
// the stream-reading goroutine, exactly as web.Device.Subscribe's doc
// requires. This mirrors PROTOCOL.md's own tolerance of seq gaps on the
// wire stream.
type fanout struct {
	mu   sync.Mutex
	subs map[chan web.Samples]struct{}
}

func newFanout() *fanout {
	return &fanout{subs: make(map[chan web.Samples]struct{})}
}

// subscribe registers a new listener with a buf-sized channel. The returned
// unsubscribe function removes and closes the channel; it is safe to call
// more than once (idempotent via sync.Once).
func (f *fanout) subscribe(buf int) (<-chan web.Samples, func()) {
	if buf <= 0 {
		buf = 1
	}
	ch := make(chan web.Samples, buf)

	f.mu.Lock()
	f.subs[ch] = struct{}{}
	f.mu.Unlock()

	var once sync.Once
	unsub := func() {
		once.Do(func() {
			f.mu.Lock()
			if _, ok := f.subs[ch]; ok {
				delete(f.subs, ch)
				close(ch)
			}
			f.mu.Unlock()
		})
	}
	return ch, unsub
}

// broadcast delivers s to every current subscriber, non-blocking: a
// subscriber whose channel is full has this batch dropped for it.
func (f *fanout) broadcast(s web.Samples) {
	f.mu.Lock()
	defer f.mu.Unlock()
	for ch := range f.subs {
		select {
		case ch <- s:
		default:
			// Slow subscriber: drop this batch rather than block the
			// stream-reading goroutine.
		}
	}
}

// closeAll removes and closes every current subscriber channel. Called
// when the underlying device is closed, so any subscriber blocked on a
// range over the channel observes it closing rather than stalling forever.
func (f *fanout) closeAll() {
	f.mu.Lock()
	defer f.mu.Unlock()
	for ch := range f.subs {
		delete(f.subs, ch)
		close(ch)
	}
}
