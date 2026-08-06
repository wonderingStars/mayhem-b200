// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.

package webadapter

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"

	"mayhemb200/webgui/internal/sdrclient"
	"mayhemb200/webgui/internal/web"
)

// statsPollInterval is how often deviceAdapter refreshes its cached Stats
// from the server's get_stats while a device is open. State (everything
// else) never needs polling: PROTOCOL.md section 4 guarantees a device is
// open by one session at a time, so nothing but this adapter's own setters
// can change it. Stats (rx_samples, overflows) are different -- they climb
// on their own, driven by the hardware, whenever RX is streaming.
const statsPollInterval = 500 * time.Millisecond

// translateErr maps an sdrclient.CommandError carrying one of PROTOCOL.md's
// two verbatim-specified error strings (section 2's example, section 4's
// "device in use") onto this package's web.Err* sentinels, so
// errors.Is-based dispatch in internal/web/handlers.go (statusForError)
// works the same whether the underlying failure came from this network
// client or, as in sdrlink's own reference GUI, from an in-process device.
// Any other message is returned unwrapped-but-intact: CommandError's own
// Error() already names the failing command.
func translateErr(err error) error {
	if err == nil {
		return nil
	}
	var cmdErr *sdrclient.CommandError
	if errors.As(err, &cmdErr) {
		switch cmdErr.Message {
		case "no device open":
			return web.ErrNoDevice
		case "device in use":
			return web.ErrDeviceInUse
		}
	}
	return err
}

// deviceAdapter wraps the sdrclient.Client control connection for one
// opened device so it satisfies internal/web.Device. It differs from
// sdrlink's own in-process webadapter.deviceAdapter in one structural way:
// there, DeliverFrame is a push callback the underlying device.Device
// invokes; here, nothing pushes anything -- the IQ stream is a second TCP
// connection this adapter must actively dial (DialStream) and read in its
// own goroutine (streamLoop) once StartRx arms streaming, decoding each
// frame and re-publishing it through the same non-blocking fanout pattern.
type deviceAdapter struct {
	client     *sdrclient.Client
	streamAddr string
	args       string // the args string this device was opened with, for Info()

	// mu guards caps/state/stats: caps is set once at construction (a
	// device's capabilities do not change while it stays open); state is
	// updated by every successful setter plus StartRx/StopRx; stats is
	// refreshed by the periodic poll loop below plus incremented locally
	// when a stream frame's overflow flag is observed (so a UI watching
	// Stats() sees it react before the next poll tick).
	mu    sync.RWMutex
	caps  sdrclient.Caps
	state sdrclient.State
	stats sdrclient.Stats

	fanout *fanout

	streamMu sync.Mutex
	stream   *sdrclient.Stream

	pollCancel context.CancelFunc
	pollDone   chan struct{}
}

// newDeviceAdapter wraps client, already having opened a device with the
// given args (caps is that open's result). It reads the device's initial
// state and starts the background stats poll loop.
func newDeviceAdapter(client *sdrclient.Client, streamAddr, args string, caps sdrclient.Caps) (*deviceAdapter, error) {
	ctx, cancel := context.WithTimeout(context.Background(), sdrclient.DefaultReplyTimeout)
	defer cancel()
	st, err := client.GetState(ctx)
	if err != nil {
		return nil, translateErr(err)
	}
	// Stats are best-effort at construction time -- a freshly opened device
	// legitimately has nothing to report yet on some backends, and this
	// must not fail the whole Open over a still-warming-up counter.
	statsCtx, statsCancel := context.WithTimeout(context.Background(), sdrclient.DefaultReplyTimeout)
	stats, _ := client.GetStats(statsCtx)
	statsCancel()

	d := &deviceAdapter{
		client:     client,
		streamAddr: streamAddr,
		args:       args,
		caps:       caps,
		state:      st,
		stats:      stats,
		fanout:     newFanout(),
	}
	d.startStatsLoop()
	return d, nil
}

func (d *deviceAdapter) startStatsLoop() {
	ctx, cancel := context.WithCancel(context.Background())
	d.pollCancel = cancel
	done := make(chan struct{})
	d.pollDone = done

	go func() {
		defer close(done)
		ticker := time.NewTicker(statsPollInterval)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				callCtx, callCancel := context.WithTimeout(ctx, sdrclient.DefaultReplyTimeout)
				st, err := d.client.GetStats(callCtx)
				callCancel()
				if err != nil {
					continue // transient; keep the last-known stats rather than zeroing them
				}
				d.mu.Lock()
				d.stats.RXSamples = st.RXSamples
				if st.Overflows > d.stats.Overflows {
					d.stats.Overflows = st.Overflows
				}
				d.mu.Unlock()
			}
		}
	}()
}

func (d *deviceAdapter) Info() web.DeviceInfo {
	d.mu.RLock()
	c := d.caps
	d.mu.RUnlock()
	return web.DeviceInfo{Driver: c.Driver, Label: c.Label, Serial: c.Serial, Args: d.args}
}

func (d *deviceAdapter) Caps() web.Caps {
	d.mu.RLock()
	defer d.mu.RUnlock()
	return convertCaps(d.caps)
}

func (d *deviceAdapter) State() web.State {
	d.mu.RLock()
	defer d.mu.RUnlock()
	return convertState(d.state)
}

func (d *deviceAdapter) Stats() web.Stats {
	d.mu.RLock()
	defer d.mu.RUnlock()
	return convertStats(d.stats)
}

func (d *deviceAdapter) SetRxFreq(hz float64) (float64, error) {
	v, err := d.client.SetRxFreq(context.Background(), hz)
	if err != nil {
		return 0, translateErr(err)
	}
	d.mu.Lock()
	d.state.RXFreqHz = v
	d.mu.Unlock()
	return v, nil
}

func (d *deviceAdapter) SetTxFreq(hz float64) (float64, error) {
	v, err := d.client.SetTxFreq(context.Background(), hz)
	if err != nil {
		return 0, translateErr(err)
	}
	d.mu.Lock()
	d.state.TXFreqHz = v
	d.mu.Unlock()
	return v, nil
}

func (d *deviceAdapter) SetRxRate(hz float64) (float64, error) {
	v, err := d.client.SetRxRate(context.Background(), hz)
	if err != nil {
		return 0, translateErr(err)
	}
	d.mu.Lock()
	d.state.RXRateHz = v
	d.mu.Unlock()
	return v, nil
}

func (d *deviceAdapter) SetTxRate(hz float64) (float64, error) {
	v, err := d.client.SetTxRate(context.Background(), hz)
	if err != nil {
		return 0, translateErr(err)
	}
	d.mu.Lock()
	d.state.TXRateHz = v
	d.mu.Unlock()
	return v, nil
}

func (d *deviceAdapter) SetRxGain(db float64) (float64, error) {
	v, err := d.client.SetRxGain(context.Background(), db)
	if err != nil {
		return 0, translateErr(err)
	}
	d.mu.Lock()
	d.state.RXGainDB = v
	d.mu.Unlock()
	return v, nil
}

func (d *deviceAdapter) SetTxGain(db float64) (float64, error) {
	v, err := d.client.SetTxGain(context.Background(), db)
	if err != nil {
		return 0, translateErr(err)
	}
	d.mu.Lock()
	d.state.TXGainDB = v
	d.mu.Unlock()
	return v, nil
}

func (d *deviceAdapter) SetRxBandwidth(hz float64) (float64, error) {
	v, err := d.client.SetRxBandwidth(context.Background(), hz)
	if err != nil {
		return 0, translateErr(err)
	}
	d.mu.Lock()
	d.state.RXBandwidthHz = v
	d.mu.Unlock()
	return v, nil
}

func (d *deviceAdapter) SetTxBandwidth(hz float64) (float64, error) {
	v, err := d.client.SetTxBandwidth(context.Background(), hz)
	if err != nil {
		return 0, translateErr(err)
	}
	d.mu.Lock()
	d.state.TXBandwidthHz = v
	d.mu.Unlock()
	return v, nil
}

func (d *deviceAdapter) SetRxAntenna(name string) (string, error) {
	v, err := d.client.SetRxAntenna(context.Background(), name)
	if err != nil {
		return "", translateErr(err)
	}
	d.mu.Lock()
	d.state.RXAntenna = v
	d.mu.Unlock()
	return v, nil
}

func (d *deviceAdapter) SetTxAntenna(name string) (string, error) {
	v, err := d.client.SetTxAntenna(context.Background(), name)
	if err != nil {
		return "", translateErr(err)
	}
	d.mu.Lock()
	d.state.TXAntenna = v
	d.mu.Unlock()
	return v, nil
}

func (d *deviceAdapter) SetLoOffset(hz float64) (float64, error) {
	v, err := d.client.SetLoOffset(context.Background(), hz)
	if err != nil {
		return 0, translateErr(err)
	}
	d.mu.Lock()
	d.state.LOOffsetHz = v
	d.mu.Unlock()
	return v, nil
}

func (d *deviceAdapter) SetRxAgc(on bool) (bool, error) {
	v, err := d.client.SetRxAgc(context.Background(), on)
	if err != nil {
		return false, translateErr(err)
	}
	d.mu.Lock()
	d.state.RXAGC = v
	d.mu.Unlock()
	return v, nil
}

func (d *deviceAdapter) SetRxDcOffsetAuto(on bool) (bool, error) {
	v, err := d.client.SetRxDcOffsetAuto(context.Background(), on)
	if err != nil {
		return false, translateErr(err)
	}
	d.mu.Lock()
	d.state.RXDCOffsetAuto = v
	d.mu.Unlock()
	return v, nil
}

func (d *deviceAdapter) SetRxIqBalanceAuto(on bool) (bool, error) {
	v, err := d.client.SetRxIqBalanceAuto(context.Background(), on)
	if err != nil {
		return false, translateErr(err)
	}
	d.mu.Lock()
	d.state.RXIQBalanceAuto = v
	d.mu.Unlock()
	return v, nil
}

// StartRx arms streaming control-side (start_rx) and then dials the second,
// binary IQ stream connection (PROTOCOL.md section 3), reading it in a new
// goroutine that decodes each frame and re-publishes it through fanout to
// every current Subscribe caller. If the stream connection can't be
// established, streaming is unwound (stop_rx) so State().Streaming does not
// lie about there being live data.
func (d *deviceAdapter) StartRx(format string) (string, error) {
	ctx := context.Background()
	got, err := d.client.StartRx(ctx, format)
	if err != nil {
		return "", translateErr(err)
	}

	d.mu.Lock()
	d.state.Streaming = true
	d.state.StreamFormat = got
	d.mu.Unlock()

	if err := d.startStream(got); err != nil {
		_ = d.client.StopRx(ctx)
		d.mu.Lock()
		d.state.Streaming = false
		d.mu.Unlock()
		return "", err
	}
	return got, nil
}

func (d *deviceAdapter) startStream(format string) error {
	d.streamMu.Lock()
	defer d.streamMu.Unlock()
	if d.stream != nil {
		_ = d.stream.Close()
		d.stream = nil
	}

	ctx, cancel := context.WithTimeout(context.Background(), sdrclient.DefaultDialTimeout)
	defer cancel()
	s, err := sdrclient.DialStream(ctx, d.streamAddr, d.client.SessionID(), format)
	if err != nil {
		return fmt.Errorf("webadapter: open IQ stream connection: %w", err)
	}
	d.stream = s
	go d.streamLoop(s)
	return nil
}

// streamLoop reads frames until the stream connection errors (including a
// clean close from StopRx/close) and republishes each one through fanout.
// It never blocks on a slow subscriber (fanout.broadcast's own contract)
// and it never blocks StartRx/StopRx/close on itself: those only ever
// Close() the *sdrclient.Stream, which unblocks the in-flight ReadFrame and
// lets this goroutine exit on its own.
func (d *deviceAdapter) streamLoop(s *sdrclient.Stream) {
	for {
		fr, err := s.ReadFrame()
		if err != nil {
			return
		}
		d.mu.RLock()
		rate := d.state.RXRateHz
		freq := d.state.RXFreqHz
		d.mu.RUnlock()

		d.fanout.broadcast(web.Samples{IQ: fr.IQ, RateHz: rate, SeqHz: freq})

		if fr.Header.Overflow() {
			d.mu.Lock()
			d.stats.Overflows++
			d.mu.Unlock()
		}
	}
}

func (d *deviceAdapter) StopRx() error {
	ctx := context.Background()
	err := d.client.StopRx(ctx)

	d.streamMu.Lock()
	if d.stream != nil {
		_ = d.stream.Close()
		d.stream = nil
	}
	d.streamMu.Unlock()

	d.mu.Lock()
	d.state.Streaming = false
	d.mu.Unlock()

	if err != nil {
		return translateErr(err)
	}
	return nil
}

// Subscribe implements web.Device.
func (d *deviceAdapter) Subscribe(buf int) (<-chan web.Samples, func()) {
	return d.fanout.subscribe(buf)
}

// close releases the underlying device, stops the background stats poll
// and any active stream connection, and closes out every subscriber
// channel so a caller ranging over a Subscribe channel observes it close
// instead of stalling forever. Called by Backend.CloseDevice.
func (d *deviceAdapter) close() error {
	d.pollCancel()
	<-d.pollDone

	d.streamMu.Lock()
	if d.stream != nil {
		_ = d.stream.Close()
		d.stream = nil
	}
	d.streamMu.Unlock()

	ctx, cancel := context.WithTimeout(context.Background(), sdrclient.DefaultReplyTimeout)
	defer cancel()
	err := d.client.CloseDevice(ctx)

	d.fanout.closeAll()
	return translateErr(err)
}
