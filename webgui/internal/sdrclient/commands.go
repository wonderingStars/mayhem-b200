// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200.
//
// This file wraps every command in PROTOCOL.md section 2.1 as a typed Go
// method. Every setter returns exactly the value the reply's "result"
// carries -- the value the server says the hardware actually accepted --
// never the value passed in. PROTOCOL.md is explicit that this is
// mandatory: "A client that assumes its request was honoured will
// mis-tune."

package sdrclient

import "context"

// ListDevices runs hardware discovery without opening anything (`
// list_devices`).
func (c *Client) ListDevices(ctx context.Context) ([]Info, error) {
	var res struct {
		Devices []Info `json:"devices"`
	}
	if err := c.call(ctx, "list_devices", nil, 0, &res); err != nil {
		return nil, err
	}
	return res.Devices, nil
}

// Open opens a device for this session (`open`). It uses the client's
// (longer) open timeout rather than the ordinary reply timeout -- see
// DefaultOpenTimeout's doc comment for why that distinction is mandatory.
func (c *Client) Open(ctx context.Context, args string) (Caps, error) {
	reqArgs := struct {
		Args string `json:"args"`
	}{Args: args}
	var res struct {
		Caps Caps `json:"caps"`
	}
	if err := c.call(ctx, "open", reqArgs, c.openTimeout, &res); err != nil {
		return Caps{}, err
	}
	return res.Caps, nil
}

// CloseDevice closes whatever device this session has open (`close`).
// Named CloseDevice, not Close, because Close is the Client's own method
// for tearing down the TCP connection itself.
func (c *Client) CloseDevice(ctx context.Context) error {
	return c.call(ctx, "close", nil, 0, nil)
}

// GetCaps re-reads the open device's capabilities (`get_caps`).
func (c *Client) GetCaps(ctx context.Context) (Caps, error) {
	var res struct {
		Caps Caps `json:"caps"`
	}
	if err := c.call(ctx, "get_caps", nil, 0, &res); err != nil {
		return Caps{}, err
	}
	return res.Caps, nil
}

// GetState reads every current setting (`get_state`).
func (c *Client) GetState(ctx context.Context) (State, error) {
	var res State
	if err := c.call(ctx, "get_state", nil, 0, &res); err != nil {
		return State{}, err
	}
	return res, nil
}

// GetStats reads the running sample/overflow counters (`get_stats`).
func (c *Client) GetStats(ctx context.Context) (Stats, error) {
	var res Stats
	if err := c.call(ctx, "get_stats", nil, 0, &res); err != nil {
		return Stats{}, err
	}
	return res, nil
}

// Ping is a keepalive / RTT probe (`ping`). It returns the server's
// millisecond timestamp from the reply's "t" field.
func (c *Client) Ping(ctx context.Context) (int64, error) {
	var res struct {
		T int64 `json:"t"`
	}
	if err := c.call(ctx, "ping", nil, 0, &res); err != nil {
		return 0, err
	}
	return res.T, nil
}

type hzArgs struct {
	Hz float64 `json:"hz"`
}
type hzResult struct {
	Hz float64 `json:"hz"`
}

func (c *Client) setHz(ctx context.Context, cmd string, hz float64) (float64, error) {
	var res hzResult
	if err := c.call(ctx, cmd, hzArgs{Hz: hz}, 0, &res); err != nil {
		return 0, err
	}
	return res.Hz, nil
}

// SetRxFreq sets the RX centre frequency (`set_rx_freq`).
func (c *Client) SetRxFreq(ctx context.Context, hz float64) (float64, error) {
	return c.setHz(ctx, "set_rx_freq", hz)
}

// SetTxFreq sets the TX centre frequency (`set_tx_freq`).
func (c *Client) SetTxFreq(ctx context.Context, hz float64) (float64, error) {
	return c.setHz(ctx, "set_tx_freq", hz)
}

// SetRxRate sets the RX sample rate (`set_rx_rate`).
func (c *Client) SetRxRate(ctx context.Context, hz float64) (float64, error) {
	return c.setHz(ctx, "set_rx_rate", hz)
}

// SetTxRate sets the TX sample rate (`set_tx_rate`).
func (c *Client) SetTxRate(ctx context.Context, hz float64) (float64, error) {
	return c.setHz(ctx, "set_tx_rate", hz)
}

// SetRxBandwidth sets the RX analog filter bandwidth (`set_rx_bandwidth`).
func (c *Client) SetRxBandwidth(ctx context.Context, hz float64) (float64, error) {
	return c.setHz(ctx, "set_rx_bandwidth", hz)
}

// SetTxBandwidth sets the TX analog filter bandwidth (`set_tx_bandwidth`).
func (c *Client) SetTxBandwidth(ctx context.Context, hz float64) (float64, error) {
	return c.setHz(ctx, "set_tx_bandwidth", hz)
}

// SetLoOffset sets the LO offset (`set_lo_offset`).
func (c *Client) SetLoOffset(ctx context.Context, hz float64) (float64, error) {
	return c.setHz(ctx, "set_lo_offset", hz)
}

type dbArgs struct {
	Db float64 `json:"db"`
}
type dbResult struct {
	Db float64 `json:"db"`
}

func (c *Client) setDb(ctx context.Context, cmd string, db float64) (float64, error) {
	var res dbResult
	if err := c.call(ctx, cmd, dbArgs{Db: db}, 0, &res); err != nil {
		return 0, err
	}
	return res.Db, nil
}

// SetRxGain sets the RX gain (`set_rx_gain`).
func (c *Client) SetRxGain(ctx context.Context, db float64) (float64, error) {
	return c.setDb(ctx, "set_rx_gain", db)
}

// SetTxGain sets the TX gain (`set_tx_gain`).
func (c *Client) SetTxGain(ctx context.Context, db float64) (float64, error) {
	return c.setDb(ctx, "set_tx_gain", db)
}

type nameArgs struct {
	Name string `json:"name"`
}
type nameResult struct {
	Name string `json:"name"`
}

func (c *Client) setName(ctx context.Context, cmd string, name string) (string, error) {
	var res nameResult
	if err := c.call(ctx, cmd, nameArgs{Name: name}, 0, &res); err != nil {
		return "", err
	}
	return res.Name, nil
}

// SetRxAntenna selects the RX antenna port (`set_rx_antenna`).
func (c *Client) SetRxAntenna(ctx context.Context, name string) (string, error) {
	return c.setName(ctx, "set_rx_antenna", name)
}

// SetTxAntenna selects the TX antenna port (`set_tx_antenna`).
func (c *Client) SetTxAntenna(ctx context.Context, name string) (string, error) {
	return c.setName(ctx, "set_tx_antenna", name)
}

type onArgs struct {
	On bool `json:"on"`
}
type onResult struct {
	On bool `json:"on"`
}

func (c *Client) setOn(ctx context.Context, cmd string, on bool) (bool, error) {
	var res onResult
	if err := c.call(ctx, cmd, onArgs{On: on}, 0, &res); err != nil {
		return false, err
	}
	return res.On, nil
}

// SetRxAgc toggles RX automatic gain control (`set_rx_agc`). Replies
// ok:false if the attached hardware has no AGC.
func (c *Client) SetRxAgc(ctx context.Context, on bool) (bool, error) {
	return c.setOn(ctx, "set_rx_agc", on)
}

// SetRxDcOffsetAuto toggles RX automatic DC offset correction
// (`set_rx_dc_offset_auto`).
func (c *Client) SetRxDcOffsetAuto(ctx context.Context, on bool) (bool, error) {
	return c.setOn(ctx, "set_rx_dc_offset_auto", on)
}

// SetRxIqBalanceAuto toggles RX automatic IQ balance correction
// (`set_rx_iq_balance_auto`).
func (c *Client) SetRxIqBalanceAuto(ctx context.Context, on bool) (bool, error) {
	return c.setOn(ctx, "set_rx_iq_balance_auto", on)
}

// StartRx begins streaming on the stream connection in the given sample
// format ("cf32", "ci16" or "ci8"; PROTOCOL.md section 3) and returns the
// format actually in effect (`start_rx`).
func (c *Client) StartRx(ctx context.Context, format string) (string, error) {
	args := struct {
		Format string `json:"format"`
	}{Format: format}
	var res struct {
		Format string `json:"format"`
	}
	if err := c.call(ctx, "start_rx", args, 0, &res); err != nil {
		return "", err
	}
	return res.Format, nil
}

// StopRx stops streaming (`stop_rx`).
func (c *Client) StopRx(ctx context.Context) error {
	return c.call(ctx, "stop_rx", nil, 0, nil)
}
