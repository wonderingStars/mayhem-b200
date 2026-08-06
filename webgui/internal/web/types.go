// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200. Originally written for sdrlink (MIT) and relicensed
// here by its author/copyright holder into this GPL-2.0-or-later project.
//
// Package web implements the HTTP/JSON API and the browser-facing
// single-page GUI for driving an sdrlink-protocol server, including a
// WebSocket endpoint that streams server-side FFT spectrum data.
//
// This package does not itself speak the sdrlink wire protocol described in
// PROTOCOL.md (that is the TCP control/stream server, built elsewhere in this
// repository as internal/device, internal/proto, internal/server and
// internal/session). It only needs a narrow view of a device: read its
// capabilities and state, drive its setters, and receive its sample stream.
// That view is captured by the Backend and Device interfaces below.
//
// RECONCILIATION NOTE: at the time this package was written, internal/device
// and internal/server were still empty (under construction by another
// agent/session working in this same tree). Backend and Device are therefore
// defined here, deliberately minimal, rather than importing a type that did
// not exist yet. When the real protocol/session server lands, either:
//
//  1. make its device-manager type satisfy Backend and its device-handle type
//     satisfy Device directly (they are small interfaces, this should be
//     mechanical), or
//  2. write a thin adapter type in this package (or a new internal/webadapter
//     package) that wraps the real types and implements Backend/Device by
//     delegating to them.
//
// Either way nothing in handlers.go, websocket.go or spectrum.go should need
// to change — they only depend on the interfaces below.
package web

import (
	"context"
	"errors"
	"time"
)

// Sentinel errors. Handlers map these to specific HTTP statuses and, where
// the wording matters for a client, the strings intentionally match the
// error text used by the TCP control protocol (PROTOCOL.md §2, §4) so the
// same failure reads the same way whether a client hit it over sdrlink's own
// wire protocol or over this HTTP API.
var (
	// ErrNoDevice means a call required an open device and none is open.
	ErrNoDevice = errors.New("no device open")
	// ErrDeviceInUse means Open was called while a device is already open.
	// PROTOCOL.md §4: "A device may be opened by one session at a time."
	ErrDeviceInUse = errors.New("device in use")
	// ErrUnsupported means the attached hardware cannot do what was asked
	// (e.g. set_rx_agc on a radio with no AGC). PROTOCOL.md §2.1 notes this
	// is one of the few commands that legitimately replies ok:false.
	ErrUnsupported = errors.New("unsupported by this device")
	// ErrNotStreaming means a spectrum/stream-only operation was attempted
	// while RX streaming is not active.
	ErrNotStreaming = errors.New("rx not streaming")
)

// Range is a hardware-reported min/max/step triple. It mirrors the ranges
// inside DeviceCaps in PROTOCOL.md §2.2 exactly (same field names) so the
// frontend can use one Range renderer for every control.
//
// A zero-value Range (Min==Max==Step==0) means "this axis does not exist on
// this device" — the frontend must disable the corresponding control rather
// than rendering a degenerate 0..0 slider.
type Range struct {
	Min  float64 `json:"min"`
	Max  float64 `json:"max"`
	Step float64 `json:"step"`
}

// Supported reports whether r describes a real, usable range.
func (r Range) Supported() bool { return r.Max > r.Min }

// DeviceInfo describes a discovered-but-not-necessarily-open device, as
// returned by the protocol's list_devices (PROTOCOL.md §2.1). Args is the
// connection string to hand back to Open/OpenDevice.
type DeviceInfo struct {
	Driver string `json:"driver"`
	Label  string `json:"label"`
	Serial string `json:"serial"`
	Args   string `json:"args"`
}

// Caps mirrors DeviceCaps from PROTOCOL.md §2.2 field for field. Every range
// is meant to be read from the hardware, never hard-coded, so that a client
// (this web GUI included) can disable or clamp controls the attached radio
// genuinely cannot do.
type Caps struct {
	Driver string `json:"driver"`
	Label  string `json:"label"`
	Serial string `json:"serial"`

	RxFreq      Range `json:"rx_freq"`
	RxGain      Range `json:"rx_gain"`
	RxRate      Range `json:"rx_rate"`
	RxBandwidth Range `json:"rx_bandwidth"`

	TxFreq Range `json:"tx_freq"`
	TxGain Range `json:"tx_gain"`

	RxAntennas []string `json:"rx_antennas"`
	TxAntennas []string `json:"tx_antennas"`

	HasRx      bool `json:"has_rx"`
	HasTx      bool `json:"has_tx"`
	FullDuplex bool `json:"full_duplex"`

	MasterClockRate float64 `json:"master_clock_rate"`
}

// State mirrors get_state (PROTOCOL.md §2.1: "all current settings").
type State struct {
	RxFreqHz      float64 `json:"rx_freq_hz"`
	TxFreqHz      float64 `json:"tx_freq_hz"`
	RxRateHz      float64 `json:"rx_rate_hz"`
	TxRateHz      float64 `json:"tx_rate_hz"`
	RxGainDb      float64 `json:"rx_gain_db"`
	TxGainDb      float64 `json:"tx_gain_db"`
	RxBandwidthHz float64 `json:"rx_bandwidth_hz"`
	TxBandwidthHz float64 `json:"tx_bandwidth_hz"`
	RxAntenna     string  `json:"rx_antenna"`
	TxAntenna     string  `json:"tx_antenna"`
	LoOffsetHz    float64 `json:"lo_offset_hz"`

	RxAgc           bool `json:"rx_agc"`
	RxDcOffsetAuto  bool `json:"rx_dc_offset_auto"`
	RxIqBalanceAuto bool `json:"rx_iq_balance_auto"`

	Streaming bool   `json:"streaming"`
	Format    string `json:"format"`
}

// Stats mirrors get_stats (PROTOCOL.md §2.1).
type Stats struct {
	RxSamples     uint64 `json:"rx_samples"`
	TxSamples     uint64 `json:"tx_samples"`
	Overflows     uint64 `json:"overflows"`
	Underflows    uint64 `json:"underflows"`
	DroppedFrames uint64 `json:"dropped_frames"`
	// BytesPerSecond is a rolling estimate of IQ throughput to the/from the
	// hardware, for the status panel's "sample throughput" readout.
	BytesPerSecond float64 `json:"bytes_per_second"`
}

// SessionInfo describes one connected control session, for the status panel.
// It deliberately mirrors what a control-protocol session naturally has:
// a session_id (PROTOCOL.md §1), the client name given to hello, and whether
// it currently holds the device open / is streaming.
type SessionInfo struct {
	ID          string    `json:"id"`
	Client      string    `json:"client"`
	RemoteAddr  string    `json:"remote_addr"`
	ConnectedAt time.Time `json:"connected_at"`
	DeviceOpen  bool      `json:"device_open"`
	Streaming   bool      `json:"streaming"`
}

// ServerStatus is the payload for GET /api/status.
type ServerStatus struct {
	UptimeSeconds float64       `json:"uptime_seconds"`
	Sessions      []SessionInfo `json:"sessions"`
	DeviceOpen    bool          `json:"device_open"`
	Stats         *Stats        `json:"stats,omitempty"`
}

// Samples is one batch of IQ samples delivered to a spectrum subscriber.
// RateHz is the sample rate in force when the batch was captured, so the
// spectrum engine can label frequency axes correctly even if the rate
// changes between batches.
type Samples struct {
	IQ     []complex64
	RateHz float64
	SeqHz  float64 // centre frequency (rx_freq_hz) at capture time
}

// Device is the minimal surface the web GUI needs from an open SDR device.
// Every setter returns the value the hardware actually accepted, never the
// bare request — this is the mandatory rule from PROTOCOL.md §2.1: "Every
// setter's result is the value actually in force." Implementations must
// enforce that themselves; this package only ever forwards what Device
// returns.
//
// Implementations must be safe for concurrent use: handlers may call these
// methods from multiple HTTP requests concurrently, and the spectrum engine
// reads Subscribe() from its own goroutine at the same time.
type Device interface {
	Info() DeviceInfo
	Caps() Caps
	State() State
	Stats() Stats

	SetRxFreq(hz float64) (float64, error)
	SetTxFreq(hz float64) (float64, error)
	SetRxRate(hz float64) (float64, error)
	SetTxRate(hz float64) (float64, error)
	SetRxGain(db float64) (float64, error)
	SetTxGain(db float64) (float64, error)
	SetRxBandwidth(hz float64) (float64, error)
	SetTxBandwidth(hz float64) (float64, error)
	SetRxAntenna(name string) (string, error)
	SetTxAntenna(name string) (string, error)
	SetLoOffset(hz float64) (float64, error)
	SetRxAgc(on bool) (bool, error)
	SetRxDcOffsetAuto(on bool) (bool, error)
	SetRxIqBalanceAuto(on bool) (bool, error)

	// StartRx begins streaming in the given sample format ("cf32", "ci16" or
	// "ci8" — PROTOCOL.md §3) and returns the format actually in effect.
	StartRx(format string) (string, error)
	StopRx() error

	// Subscribe registers a new listener for IQ sample batches. buf sizes
	// the channel; a slow subscriber drops batches rather than blocking the
	// device (mirrors the wire protocol's tolerance of seq gaps, §3). The
	// returned function unsubscribes and must be safe to call more than
	// once.
	Subscribe(buf int) (<-chan Samples, func())
}

// Backend is the minimal surface the web GUI needs from the sdrlink server
// as a whole: device discovery/lifecycle plus the process-wide counters the
// status panel shows. See the package doc for how this reconciles with the
// real protocol/session server once that package exists.
type Backend interface {
	// ListDevices runs hardware discovery (protocol: list_devices).
	ListDevices(ctx context.Context) ([]DeviceInfo, error)

	// OpenDevice opens a device for exclusive use. args is a driver
	// connection string as returned in DeviceInfo.Args (e.g.
	// "driver=uhd,serial=EDR04ZDB2"). Returns ErrDeviceInUse if a device is
	// already open.
	OpenDevice(ctx context.Context, args string) (Device, error)

	// CurrentDevice returns the presently open device, if any.
	CurrentDevice() (Device, bool)

	// CloseDevice closes whatever device is open. No-op if none is open.
	CloseDevice() error

	// Sessions returns the currently connected control sessions.
	Sessions() []SessionInfo

	// Uptime is time since the server started serving.
	Uptime() time.Duration
}
