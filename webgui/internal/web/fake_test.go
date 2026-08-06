// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200. Originally written for sdrlink (MIT) and relicensed
// here by its author/copyright holder into this GPL-2.0-or-later project.

package web

import (
	"context"
	"fmt"
	"math"
	"sync"
	"time"
)

// fakeDevice and fakeBackend are minimal, in-package test doubles for
// Backend/Device. They deliberately do NOT import sdrlink/internal/simdevice
// (which itself imports this package to implement these interfaces — doing
// so here would be an import cycle) and are intentionally simpler than
// simdevice: enough hardware-like quantisation behaviour to prove the
// "return what was accepted" rule, nothing more. End-to-end tests against
// the real simdevice.Backend (including the spectrum WebSocket feed with
// actual generated samples) live in integration_test.go, an external
// (package web_test) test file that can import both without a cycle.
var fakeCaps = Caps{
	Driver:      "fake",
	Label:       "Fake Test Radio",
	Serial:      "FAKE0001",
	RxFreq:      Range{Min: 42e6, Max: 6008e6, Step: 1},
	RxGain:      Range{Min: 0, Max: 76, Step: 1},
	RxRate:      Range{Min: 0.2e6, Max: 16e6, Step: 1},
	RxBandwidth: Range{Min: 0.2e6, Max: 56e6, Step: 1},
	TxFreq:      Range{Min: 42e6, Max: 6008e6, Step: 1},
	TxGain:      Range{Min: 0, Max: 89.8, Step: 0.2},
	RxAntennas:  []string{"TX/RX", "RX2"},
	TxAntennas:  []string{"TX/RX"},
	HasRx:       true, HasTx: true, FullDuplex: true,
	MasterClockRate: 16e6,
}

func fakeQuantize(v, min, max, step float64) float64 {
	if step <= 0 {
		step = 1
	}
	if v < min {
		v = min
	}
	if v > max {
		v = max
	}
	q := min + math.Round((v-min)/step)*step
	if q > max {
		q = max
	}
	return q
}

type fakeDevice struct {
	mu sync.Mutex

	st  State
	sts Stats
}

func newFakeDevice() *fakeDevice {
	return &fakeDevice{
		st: State{
			RxFreqHz: 100e6, TxFreqHz: 100e6,
			RxRateHz: 2e6, TxRateHz: 2e6,
			RxGainDb: 30, TxGainDb: 10,
			RxBandwidthHz: 2e6, TxBandwidthHz: 2e6,
			RxAntenna: "RX2", TxAntenna: "TX/RX",
			Format: "cf32",
		},
	}
}

func (d *fakeDevice) Info() DeviceInfo {
	return DeviceInfo{Driver: "fake", Label: fakeCaps.Label, Serial: fakeCaps.Serial, Args: "driver=fake"}
}
func (d *fakeDevice) Caps() Caps { return fakeCaps }
func (d *fakeDevice) State() State {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.st
}
func (d *fakeDevice) Stats() Stats {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.sts
}

func (d *fakeDevice) SetRxFreq(hz float64) (float64, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.st.RxFreqHz = fakeQuantize(hz, fakeCaps.RxFreq.Min, fakeCaps.RxFreq.Max, fakeCaps.RxFreq.Step)
	return d.st.RxFreqHz, nil
}
func (d *fakeDevice) SetTxFreq(hz float64) (float64, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.st.TxFreqHz = fakeQuantize(hz, fakeCaps.TxFreq.Min, fakeCaps.TxFreq.Max, fakeCaps.TxFreq.Step)
	return d.st.TxFreqHz, nil
}
func (d *fakeDevice) SetRxRate(hz float64) (float64, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.st.RxRateHz = fakeQuantize(hz, fakeCaps.RxRate.Min, fakeCaps.RxRate.Max, fakeCaps.RxRate.Step)
	return d.st.RxRateHz, nil
}
func (d *fakeDevice) SetTxRate(hz float64) (float64, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.st.TxRateHz = fakeQuantize(hz, fakeCaps.RxRate.Min, fakeCaps.RxRate.Max, fakeCaps.RxRate.Step)
	return d.st.TxRateHz, nil
}
func (d *fakeDevice) SetRxGain(db float64) (float64, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.st.RxGainDb = fakeQuantize(db, fakeCaps.RxGain.Min, fakeCaps.RxGain.Max, fakeCaps.RxGain.Step)
	return d.st.RxGainDb, nil
}
func (d *fakeDevice) SetTxGain(db float64) (float64, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.st.TxGainDb = fakeQuantize(db, fakeCaps.TxGain.Min, fakeCaps.TxGain.Max, fakeCaps.TxGain.Step)
	return d.st.TxGainDb, nil
}
func (d *fakeDevice) SetRxBandwidth(hz float64) (float64, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.st.RxBandwidthHz = fakeQuantize(hz, fakeCaps.RxBandwidth.Min, fakeCaps.RxBandwidth.Max, fakeCaps.RxBandwidth.Step)
	return d.st.RxBandwidthHz, nil
}
func (d *fakeDevice) SetTxBandwidth(hz float64) (float64, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.st.TxBandwidthHz = fakeQuantize(hz, fakeCaps.RxBandwidth.Min, fakeCaps.RxBandwidth.Max, fakeCaps.RxBandwidth.Step)
	return d.st.TxBandwidthHz, nil
}
func (d *fakeDevice) SetRxAntenna(name string) (string, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	for _, a := range fakeCaps.RxAntennas {
		if a == name {
			d.st.RxAntenna = a
			return a, nil
		}
	}
	return "", fmt.Errorf("unknown rx antenna %q", name)
}
func (d *fakeDevice) SetTxAntenna(name string) (string, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	for _, a := range fakeCaps.TxAntennas {
		if a == name {
			d.st.TxAntenna = a
			return a, nil
		}
	}
	return "", fmt.Errorf("unknown tx antenna %q", name)
}
func (d *fakeDevice) SetLoOffset(hz float64) (float64, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.st.LoOffsetHz = hz
	return hz, nil
}
func (d *fakeDevice) SetRxAgc(on bool) (bool, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.st.RxAgc = on
	return on, nil
}
func (d *fakeDevice) SetRxDcOffsetAuto(on bool) (bool, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.st.RxDcOffsetAuto = on
	return on, nil
}
func (d *fakeDevice) SetRxIqBalanceAuto(on bool) (bool, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.st.RxIqBalanceAuto = on
	return on, nil
}
func (d *fakeDevice) StartRx(format string) (string, error) {
	d.mu.Lock()
	defer d.mu.Unlock()
	switch format {
	case "cf32", "ci16", "ci8":
		d.st.Format = format
	case "":
		d.st.Format = "cf32"
	default:
		return "", fmt.Errorf("unsupported format %q", format)
	}
	d.st.Streaming = true
	return d.st.Format, nil
}
func (d *fakeDevice) StopRx() error {
	d.mu.Lock()
	defer d.mu.Unlock()
	d.st.Streaming = false
	return nil
}
func (d *fakeDevice) Subscribe(buf int) (<-chan Samples, func()) {
	ch := make(chan Samples, 1)
	return ch, func() {}
}

type fakeBackend struct {
	startedAt time.Time
	mu        sync.Mutex
	current   *fakeDevice
}

func newFakeBackend() *fakeBackend {
	return &fakeBackend{startedAt: time.Now()}
}

func (b *fakeBackend) ListDevices(ctx context.Context) ([]DeviceInfo, error) {
	return []DeviceInfo{{Driver: "fake", Label: fakeCaps.Label, Serial: fakeCaps.Serial, Args: "driver=fake"}}, nil
}
func (b *fakeBackend) OpenDevice(ctx context.Context, args string) (Device, error) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.current != nil {
		return nil, ErrDeviceInUse
	}
	b.current = newFakeDevice()
	return b.current, nil
}
func (b *fakeBackend) CurrentDevice() (Device, bool) {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.current == nil {
		return nil, false
	}
	return b.current, true
}
func (b *fakeBackend) CloseDevice() error {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.current = nil
	return nil
}
func (b *fakeBackend) Sessions() []SessionInfo {
	b.mu.Lock()
	defer b.mu.Unlock()
	if b.current == nil {
		return nil
	}
	return []SessionInfo{{ID: "fake-session", Client: "test", ConnectedAt: b.startedAt, DeviceOpen: true, Streaming: b.current.State().Streaming}}
}
func (b *fakeBackend) Uptime() time.Duration { return time.Since(b.startedAt) }
