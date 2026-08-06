// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200. Originally written for sdrlink (MIT) and relicensed
// here by its author/copyright holder into this GPL-2.0-or-later project.

package web

import (
	"math"
	"math/cmplx"
	"net/http"
	"time"
)

// --- FFT -------------------------------------------------------------------
//
// A small, dependency-free iterative radix-2 Cooley-Tukey FFT. sdrlink's web
// GUI computes spectra server-side (raw IQ never goes to the browser, see
// package doc in server.go), so this is the one piece of DSP math the web
// package owns. len(x) must be a power of two; computeSpectrumBins below
// takes care of padding/truncating a live sample buffer to that shape.

// fft computes the discrete Fourier transform of x in place.
// Requires len(x) to be a power of two (panics otherwise, since this is only
// ever called internally with sizes this package controls).
func fft(x []complex128) {
	n := len(x)
	if n <= 1 {
		return
	}
	if n&(n-1) != 0 {
		panic("web: fft: length must be a power of two")
	}

	// Bit-reversal permutation.
	for i, j := 1, 0; i < n; i++ {
		bit := n >> 1
		for ; j&bit != 0; bit >>= 1 {
			j ^= bit
		}
		j ^= bit
		if i < j {
			x[i], x[j] = x[j], x[i]
		}
	}

	// Iterative Cooley-Tukey butterflies.
	for size := 2; size <= n; size <<= 1 {
		half := size / 2
		theta := -2 * math.Pi / float64(size)
		wStep := cmplx.Rect(1, theta)
		for start := 0; start < n; start += size {
			w := complex(1.0, 0.0)
			for k := 0; k < half; k++ {
				a := x[start+k]
				b := x[start+k+half] * w
				x[start+k] = a + b
				x[start+k+half] = a - b
				w *= wStep
			}
		}
	}
}

// nextPow2 returns the smallest power of two that is >= n, with a floor of 1.
func nextPow2(n int) int {
	if n <= 1 {
		return 1
	}
	p := 1
	for p < n {
		p <<= 1
	}
	return p
}

// hannWindow returns an n-point Hann window.
func hannWindow(n int) []float64 {
	w := make([]float64, n)
	if n == 1 {
		w[0] = 1
		return w
	}
	for i := 0; i < n; i++ {
		w[i] = 0.5 - 0.5*math.Cos(2*math.Pi*float64(i)/float64(n-1))
	}
	return w
}

// minFloor is the smallest magnitude fft treats as non-zero, to keep dB
// finite for a true-zero bin (e.g. the impulse test, or a padded tail).
const minFloor = 1e-12

// magnitudeDB converts an FFT output (bins, from an fftSize-point transform)
// into per-bin magnitude in dB (20*log10(|X|/N)).
func magnitudeDB(bins []complex128, fftSize int) []float64 {
	out := make([]float64, len(bins))
	norm := 1.0 / float64(fftSize)
	for i, c := range bins {
		mag := cmplx.Abs(c) * norm
		if mag < minFloor {
			mag = minFloor
		}
		out[i] = 20 * math.Log10(mag)
	}
	return out
}

// fftShift reorders FFT-order bins (0..N-1 representing 0, +1/N .. +(N/2-1)/N,
// -N/2/N .. -1/N of the sample rate) into frequency order, DC in the middle,
// so the array can be plotted left-to-right as -Fs/2..+Fs/2.
func fftShift(bins []float64) []float64 {
	n := len(bins)
	out := make([]float64, n)
	half := n / 2
	// Second half of FFT-order (negative frequencies) goes first.
	copy(out[:n-half], bins[half:])
	copy(out[n-half:], bins[:half])
	return out
}

// reduceBins downsamples mags (dB values) to exactly target bins by
// averaging each group of source bins in the LINEAR POWER domain (averaging
// dB values directly would understate energy). If target <= 0 or target
// covers every source bin already, mags is returned unchanged.
func reduceBins(mags []float64, target int) []float64 {
	n := len(mags)
	if target <= 0 || target >= n {
		return mags
	}
	out := make([]float64, target)
	for i := 0; i < target; i++ {
		start := i * n / target
		end := (i + 1) * n / target
		if end <= start {
			end = start + 1
		}
		var sum float64
		cnt := 0
		for j := start; j < end && j < n; j++ {
			sum += math.Pow(10, mags[j]/10)
			cnt++
		}
		avg := sum / float64(cnt)
		if avg < minFloor*minFloor {
			avg = minFloor * minFloor
		}
		out[i] = 10 * math.Log10(avg)
	}
	return out
}

func minMaxFloat(vals []float64) (min, max float64) {
	if len(vals) == 0 {
		return 0, 0
	}
	min, max = vals[0], vals[0]
	for _, v := range vals[1:] {
		if v < min {
			min = v
		}
		if v > max {
			max = v
		}
	}
	return min, max
}

// computeSpectrumBins windows and transforms up to fftSize samples from iq
// (zero-padded if there are fewer, truncated if there are more), converts to
// shifted, dB magnitude, and reduces to targetBins output bins. It returns
// the reduced bins plus their min/max, which the frontend uses to auto-scale
// the waterfall colour map.
func computeSpectrumBins(iq []complex64, fftSize, targetBins int) (bins []float64, floorDb, ceilDb float64) {
	if fftSize <= 0 {
		fftSize = 1
	}
	fftSize = nextPow2(fftSize)

	win := hannWindow(fftSize)
	buf := make([]complex128, fftSize)
	m := len(iq)
	for i := 0; i < fftSize; i++ {
		if i < m {
			s := iq[i]
			buf[i] = complex(float64(real(s))*win[i], float64(imag(s))*win[i])
		}
	}

	fft(buf)
	mags := magnitudeDB(buf, fftSize)
	mags = fftShift(mags)
	bins = reduceBins(mags, targetBins)
	floorDb, ceilDb = minMaxFloat(bins)
	return bins, floorDb, ceilDb
}

// --- WebSocket spectrum feed -------------------------------------------------
//
// GET /ws/spectrum pushes one JSON text frame per tick: either a
// SpectrumFrame (device streaming, enough samples accumulated) or an
// idleFrame explaining why there isn't one yet (no device open, or open but
// not streaming). The browser never sees raw IQ — only the reduced,
// magnitude-in-dB bins, matching the task's "compute the FFT server-side"
// requirement and keeping the socket bandwidth-cheap regardless of the
// radio's sample rate.
//
// Status (sessions/uptime/throughput) is deliberately NOT pushed over this
// socket — it is served by GET /api/status instead, polled by the frontend.
// That mirrors PROTOCOL.md's own rationale (§1) for splitting control from
// the IQ firehose: mixing a slow-changing status payload into the
// high-frequency spectrum stream is one more thing that can stall it.

// SpectrumFrame is one WebSocket message carrying a computed spectrum.
type SpectrumFrame struct {
	Type         string    `json:"type"` // "spectrum"
	TimestampMs  int64     `json:"timestamp_ms"`
	CenterHz     float64   `json:"center_hz"`
	SampleRateHz float64   `json:"sample_rate_hz"`
	Bins         []float64 `json:"bins_db"`
	FloorDb      float64   `json:"floor_db"`
	CeilDb       float64   `json:"ceil_db"`
}

// idleFrame is sent when there is nothing to compute a spectrum from yet.
type idleFrame struct {
	Type   string `json:"type"` // "idle"
	Reason string `json:"reason"`
}

func newIdleFrame(reason string) idleFrame { return idleFrame{Type: "idle", Reason: reason} }

// handleSpectrumWS upgrades the connection and streams spectrum frames until
// the client disconnects or the server shuts the request down.
func (s *Server) handleSpectrumWS(w http.ResponseWriter, r *http.Request) {
	conn, err := wsUpgrade(w, r)
	if err != nil {
		http.Error(w, err.Error(), http.StatusBadRequest)
		return
	}
	defer conn.Close()

	// Drain client frames in the background so we notice a close/ping
	// promptly and answer pings; any read error (including a clean close)
	// ends the connection.
	closed := make(chan struct{})
	go func() {
		defer close(closed)
		for {
			opcode, payload, err := conn.ReadMessage()
			if err != nil {
				return
			}
			switch opcode {
			case wsOpClose:
				return
			case wsOpPing:
				if err := conn.writeFrame(wsOpPong, payload); err != nil {
					return
				}
			}
		}
	}()

	interval := s.spectrumInterval
	if interval <= 0 {
		interval = 50 * time.Millisecond
	}
	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	var (
		sub    <-chan Samples
		unsub  func()
		curDev Device
	)
	defer func() {
		if unsub != nil {
			unsub()
		}
	}()

	buf := make([]complex64, 0, s.fftSize)

	for {
		select {
		case <-closed:
			return
		case <-r.Context().Done():
			return
		case <-ticker.C:
			dev, ok := s.backend.CurrentDevice()
			if !ok {
				if unsub != nil {
					unsub()
					unsub, sub, curDev = nil, nil, nil
				}
				buf = buf[:0]
				if err := conn.WriteJSON(newIdleFrame(ErrNoDevice.Error())); err != nil {
					return
				}
				continue
			}

			if dev != curDev {
				if unsub != nil {
					unsub()
				}
				sub, unsub = dev.Subscribe(64)
				curDev = dev
				buf = buf[:0]
			}

			st := dev.State()
			if !st.Streaming {
				if err := conn.WriteJSON(newIdleFrame(ErrNotStreaming.Error())); err != nil {
					return
				}
				continue
			}

			// Drain whatever batches are ready without blocking; if that's
			// not enough for a full FFT window yet, wait for the next tick.
		drain:
			for len(buf) < s.fftSize {
				select {
				case batch, ok := <-sub:
					if !ok {
						break drain
					}
					buf = append(buf, batch.IQ...)
				default:
					break drain
				}
			}
			if len(buf) < s.fftSize {
				continue
			}

			bins, floor, ceil := computeSpectrumBins(buf, s.fftSize, s.targetBins)
			buf = buf[:0]

			frame := SpectrumFrame{
				Type:         "spectrum",
				TimestampMs:  time.Now().UnixMilli(),
				CenterHz:     st.RxFreqHz,
				SampleRateHz: st.RxRateHz,
				Bins:         bins,
				FloorDb:      floor,
				CeilDb:       ceil,
			}
			if err := conn.WriteJSON(frame); err != nil {
				return
			}
		}
	}
}
