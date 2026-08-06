// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.
//
// This file bridges the naming mismatch between internal/sdrclient's wire
// types (RXFreqHz, SetRXFreq's return value, ...), spelled to match
// PROTOCOL.md's own field names, and internal/web's independently-declared
// Caps/State/Stats/Range shapes (RxFreqHz, ...), spelled Go-idiomatically.
// Both packages describe the same protocol, so every field lines up 1:1 --
// only the Go identifier casing differs -- but Go does not let one named
// struct satisfy another's shape implicitly, so the conversions are spelled
// out once, here. (This mirrors sdrlink's own internal/webadapter/convert.go,
// which exists for exactly the same reason between its internal/proto and
// internal/web.)

package webadapter

import (
	"mayhemb200/webgui/internal/sdrclient"
	"mayhemb200/webgui/internal/web"
)

func convertRange(r sdrclient.Range) web.Range {
	return web.Range{Min: r.Min, Max: r.Max, Step: r.Step}
}

func convertCaps(c sdrclient.Caps) web.Caps {
	return web.Caps{
		Driver:      c.Driver,
		Label:       c.Label,
		Serial:      c.Serial,
		RxFreq:      convertRange(c.RXFreq),
		RxGain:      convertRange(c.RXGain),
		RxRate:      convertRange(c.RXRate),
		RxBandwidth: convertRange(c.RXBandwidth),
		TxFreq:      convertRange(c.TXFreq),
		TxGain:      convertRange(c.TXGain),
		RxAntennas:  c.RXAntennas,
		TxAntennas:  c.TXAntennas,
		HasRx:       c.HasRX,
		HasTx:       c.HasTX,
		FullDuplex:  c.FullDuplex,

		MasterClockRate: c.MasterClockRate,
	}
}

func convertState(s sdrclient.State) web.State {
	return web.State{
		RxFreqHz:      s.RXFreqHz,
		TxFreqHz:      s.TXFreqHz,
		RxRateHz:      s.RXRateHz,
		TxRateHz:      s.TXRateHz,
		RxGainDb:      s.RXGainDB,
		TxGainDb:      s.TXGainDB,
		RxBandwidthHz: s.RXBandwidthHz,
		TxBandwidthHz: s.TXBandwidthHz,
		RxAntenna:     s.RXAntenna,
		TxAntenna:     s.TXAntenna,
		LoOffsetHz:    s.LOOffsetHz,

		RxAgc:           s.RXAGC,
		RxDcOffsetAuto:  s.RXDCOffsetAuto,
		RxIqBalanceAuto: s.RXIQBalanceAuto,

		Streaming: s.Streaming,
		Format:    s.StreamFormat,
	}
}

func convertStats(s sdrclient.Stats) web.Stats {
	// sdrclient.Stats (PROTOCOL.md get_stats) only carries RXSamples and
	// Overflows; web.Stats has a few extra fields (TxSamples, Underflows,
	// DroppedFrames, BytesPerSecond) that PROTOCOL.md's get_stats does not
	// define. They are left at their zero value rather than fabricated.
	return web.Stats{
		RxSamples: s.RXSamples,
		Overflows: s.Overflows,
	}
}

func convertInfo(i sdrclient.Info) web.DeviceInfo {
	return web.DeviceInfo{Driver: i.Driver, Label: i.Label, Serial: i.Serial, Args: i.Args}
}
