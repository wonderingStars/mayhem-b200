// SPDX-License-Identifier: GPL-2.0-or-later

// Part of mayhem-b200. Originally written for sdrlink (MIT) and relicensed
// here by its author/copyright holder into this GPL-2.0-or-later project.

package web

import (
	"math"
	"math/cmplx"
	"math/rand"
	"testing"
)

func approxEqual(a, b, eps float64) bool {
	return math.Abs(a-b) <= eps
}

func naiveDFT(x []complex128) []complex128 {
	n := len(x)
	out := make([]complex128, n)
	for k := 0; k < n; k++ {
		var sum complex128
		for t := 0; t < n; t++ {
			angle := -2 * math.Pi * float64(k) * float64(t) / float64(n)
			sum += x[t] * cmplx.Rect(1, angle)
		}
		out[k] = sum
	}
	return out
}

func TestFFT_MatchesNaiveDFT(t *testing.T) {
	rng := rand.New(rand.NewSource(1))
	for _, n := range []int{1, 2, 4, 16, 64} {
		x := make([]complex128, n)
		for i := range x {
			x[i] = complex(rng.Float64()*2-1, rng.Float64()*2-1)
		}
		want := naiveDFT(x)
		got := append([]complex128(nil), x...)
		fft(got)
		for i := range want {
			if cmplx.Abs(got[i]-want[i]) > 1e-9 {
				t.Fatalf("n=%d bin %d: fft=%v naive=%v", n, i, got[i], want[i])
			}
		}
	}
}

func TestFFT_Impulse(t *testing.T) {
	n := 8
	x := make([]complex128, n)
	x[0] = 1
	fft(x)
	// DFT of a unit impulse at t=0 is flat: every bin equals 1.
	for i, c := range x {
		if cmplx.Abs(c-1) > 1e-12 {
			t.Fatalf("bin %d = %v, want 1", i, c)
		}
	}
}

func TestFFT_DC(t *testing.T) {
	n := 8
	x := make([]complex128, n)
	for i := range x {
		x[i] = 1
	}
	fft(x)
	if cmplx.Abs(x[0]-complex(float64(n), 0)) > 1e-9 {
		t.Fatalf("DC bin = %v, want %v", x[0], n)
	}
	for i := 1; i < n; i++ {
		if cmplx.Abs(x[i]) > 1e-9 {
			t.Fatalf("bin %d = %v, want ~0", i, x[i])
		}
	}
}

func TestFFT_PanicsOnNonPowerOfTwo(t *testing.T) {
	defer func() {
		if recover() == nil {
			t.Fatal("expected panic for non-power-of-two length")
		}
	}()
	fft(make([]complex128, 3))
}

func TestNextPow2(t *testing.T) {
	cases := map[int]int{0: 1, 1: 1, 2: 2, 3: 4, 4: 4, 5: 8, 1024: 1024, 1025: 2048}
	for in, want := range cases {
		if got := nextPow2(in); got != want {
			t.Errorf("nextPow2(%d) = %d, want %d", in, got, want)
		}
	}
}

func TestHannWindow(t *testing.T) {
	w := hannWindow(4)
	if !approxEqual(w[0], 0, 1e-12) {
		t.Errorf("w[0] = %v, want 0", w[0])
	}
	if !approxEqual(w[len(w)-1], 0, 1e-12) {
		t.Errorf("w[last] = %v, want 0", w[len(w)-1])
	}
	w1 := hannWindow(1)
	if len(w1) != 1 || w1[0] != 1 {
		t.Errorf("hannWindow(1) = %v, want [1]", w1)
	}
}

func TestMagnitudeDB(t *testing.T) {
	bins := []complex128{complex(2, 0), 0}
	mags := magnitudeDB(bins, 1)
	want0 := 20 * math.Log10(2.0)
	if !approxEqual(mags[0], want0, 1e-9) {
		t.Errorf("mags[0] = %v, want %v", mags[0], want0)
	}
	wantFloor := 20 * math.Log10(minFloor)
	if !approxEqual(mags[1], wantFloor, 1e-9) {
		t.Errorf("mags[1] (zero bin) = %v, want floor %v", mags[1], wantFloor)
	}
}

func TestFFTShift(t *testing.T) {
	in := []float64{0, 1, 2, 3, 4, 5, 6, 7}
	got := fftShift(in)
	want := []float64{4, 5, 6, 7, 0, 1, 2, 3}
	for i := range want {
		if got[i] != want[i] {
			t.Fatalf("fftShift(%v) = %v, want %v", in, got, want)
		}
	}
}

func TestReduceBins(t *testing.T) {
	mags := []float64{0, 0, 0, 0, 0, 0, 0, 10}
	got := reduceBins(mags, 4)
	if len(got) != 4 {
		t.Fatalf("len = %d, want 4", len(got))
	}
	want := []float64{0, 0, 0, 10 * math.Log10((1+10)/2.0)}
	for i := range want {
		if !approxEqual(got[i], want[i], 1e-9) {
			t.Errorf("bin %d = %v, want %v", i, got[i], want[i])
		}
	}
}

func TestReduceBins_PassthroughWhenTargetCoversAll(t *testing.T) {
	mags := []float64{1, 2, 3}
	if got := reduceBins(mags, 3); len(got) != 3 || got[0] != 1 || got[2] != 3 {
		t.Fatalf("expected passthrough, got %v", got)
	}
	if got := reduceBins(mags, 10); len(got) != 3 {
		t.Fatalf("expected passthrough for target > n, got %v", got)
	}
	if got := reduceBins(mags, 0); len(got) != 3 {
		t.Fatalf("expected passthrough for target <= 0, got %v", got)
	}
}

// TestComputeSpectrumBins_TonePeaksAtExpectedBin builds a pure complex tone
// at an exact FFT bin (no spectral leakage from a fractional bin) and checks
// the reduced, shifted output peaks where fftShift's own mapping predicts.
func TestComputeSpectrumBins_TonePeaksAtExpectedBin(t *testing.T) {
	const n = 64
	const k0 = 10 // < n/2: a "positive frequency" bin
	iq := make([]complex64, n)
	for i := 0; i < n; i++ {
		angle := 2 * math.Pi * float64(k0) * float64(i) / float64(n)
		iq[i] = complex64(complex(math.Cos(angle), math.Sin(angle)))
	}

	bins, floor, ceil := computeSpectrumBins(iq, n, n) // target==n: no reduction
	if len(bins) != n {
		t.Fatalf("len(bins) = %d, want %d", len(bins), n)
	}
	if ceil <= floor {
		t.Fatalf("ceil (%v) should be > floor (%v)", ceil, floor)
	}

	peakIdx := 0
	for i, v := range bins {
		if v > bins[peakIdx] {
			peakIdx = i
		}
	}
	expected := ((k0-n/2)%n + n) % n
	if peakIdx != expected {
		t.Fatalf("peak at bin %d, want %d (bins=%v)", peakIdx, expected, bins)
	}
}

func TestComputeSpectrumBins_PadsShortInput(t *testing.T) {
	// Fewer samples than fftSize must be zero-padded, not panic or truncate
	// fftSize itself.
	iq := []complex64{1, 1, 1}
	bins, _, _ := computeSpectrumBins(iq, 16, 16)
	if len(bins) != 16 {
		t.Fatalf("len(bins) = %d, want 16", len(bins))
	}
}

func TestComputeSpectrumBins_RoundsFFTSizeUpToPow2(t *testing.T) {
	iq := make([]complex64, 100)
	bins, _, _ := computeSpectrumBins(iq, 100, 100)
	// nextPow2(100) = 128, and target(100) < 128 so bins get reduced to 100.
	if len(bins) != 100 {
		t.Fatalf("len(bins) = %d, want 100", len(bins))
	}
}
