// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.
//
// Package sdrclient is a Go client for the sdrlink wire protocol described
// in PROTOCOL.md: a newline-delimited JSON control connection plus a binary
// IQ stream connection. It is a fresh implementation written against that
// published, open specification (PROTOCOL.md explicitly invites independent
// implementations "in any language, under any licence") — this package does
// not import sdrlink's own Go module (its internal/proto and internal/frame
// packages are Go-internal to that module and cannot be imported from here
// regardless), it re-derives the same wire shapes from the spec so that any
// sdrlink-protocol server, not just the reference one, can be driven by
// this client.
package sdrclient

// Range describes a hardware-reported min/max/step triple, as documented in
// PROTOCOL.md section 2.2 (DeviceCaps). Every range comes from the server,
// which reads it from the hardware; this type never hard-codes one.
type Range struct {
	Min  float64 `json:"min"`
	Max  float64 `json:"max"`
	Step float64 `json:"step"`
}

// Supported reports whether r describes a real, usable range (mirrors the
// convention used by the reference web GUI: a zero-value Range means "this
// axis does not exist on this device").
func (r Range) Supported() bool { return r.Max > r.Min }

// Caps is DeviceCaps from PROTOCOL.md section 2.2, the result of `open` and
// `get_caps`.
type Caps struct {
	Driver string `json:"driver"`
	Label  string `json:"label"`
	Serial string `json:"serial"`

	RXFreq      Range `json:"rx_freq"`
	RXGain      Range `json:"rx_gain"`
	RXRate      Range `json:"rx_rate"`
	RXBandwidth Range `json:"rx_bandwidth"`

	TXFreq Range `json:"tx_freq"`
	TXGain Range `json:"tx_gain"`

	RXAntennas []string `json:"rx_antennas"`
	TXAntennas []string `json:"tx_antennas"`

	HasRX      bool `json:"has_rx"`
	HasTX      bool `json:"has_tx"`
	FullDuplex bool `json:"full_duplex"`

	MasterClockRate float64 `json:"master_clock_rate"`
}

// Info is one entry of list_devices' "devices" array (PROTOCOL.md section
// 2.1): enough to construct an `open` args string before anything is
// opened.
type Info struct {
	Driver string `json:"driver"`
	Label  string `json:"label"`
	Serial string `json:"serial"`
	Args   string `json:"args"`
}

// State is the get_state snapshot of every current setting (PROTOCOL.md
// section 2.1: "all current settings").
type State struct {
	Open            bool    `json:"open"`
	RXFreqHz        float64 `json:"rx_freq_hz"`
	TXFreqHz        float64 `json:"tx_freq_hz"`
	RXRateHz        float64 `json:"rx_rate_hz"`
	TXRateHz        float64 `json:"tx_rate_hz"`
	RXGainDB        float64 `json:"rx_gain_db"`
	TXGainDB        float64 `json:"tx_gain_db"`
	RXBandwidthHz   float64 `json:"rx_bandwidth_hz"`
	TXBandwidthHz   float64 `json:"tx_bandwidth_hz"`
	RXAntenna       string  `json:"rx_antenna"`
	TXAntenna       string  `json:"tx_antenna"`
	LOOffsetHz      float64 `json:"lo_offset_hz"`
	RXAGC           bool    `json:"rx_agc"`
	RXDCOffsetAuto  bool    `json:"rx_dc_offset_auto"`
	RXIQBalanceAuto bool    `json:"rx_iq_balance_auto"`
	Streaming       bool    `json:"streaming"`
	StreamFormat    string  `json:"stream_format,omitempty"`
}

// Stats is the get_stats reply body (PROTOCOL.md section 2.1).
type Stats struct {
	RXSamples uint64 `json:"rx_samples"`
	Overflows uint64 `json:"overflows"`
}

// HelloResult is the result of the `hello` command (PROTOCOL.md section
// 2.1): the session id this client must present to open the stream
// connection, plus the server's self-reported name and protocol version.
type HelloResult struct {
	SessionID string `json:"session_id"`
	Server    string `json:"server"`
	Proto     int    `json:"proto"`
}

// Event is an unsolicited server->client control-connection line
// (PROTOCOL.md section 2): it carries no id and is not a reply to any
// request. The only event PROTOCOL.md defines today is "overflow".
type Event struct {
	Event string         `json:"event"`
	Count int            `json:"count,omitempty"`
	Raw   map[string]any `json:"-"`
}
