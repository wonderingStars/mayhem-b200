# Mayhem B200

PortaPack Mayhem's interface and apps, running on your PC against an
**Ettus USRP B200**.

![status](https://img.shields.io/badge/status-alpha-orange) ![licence](https://img.shields.io/badge/licence-GPL--2.0--or--later-blue)

---

## What this actually is

Mayhem is firmware. It runs on the PortaPack's LPC43xx — a dual-core Cortex-M
with a 240x320 LCD, a five-way switch and an SD card — and it drives a HackRF
One over an SGPIO bus.

**A USRP B200 has none of that.** It is a USB peripheral: a Spartan-6 FPGA, a
Cypress FX3 USB controller and an AD9364 transceiver. There is no application
processor to run firmware on, no screen, and no buttons. You cannot flash
Mayhem onto a B200, and anything claiming to do so is claiming something the
hardware cannot do.

So this is the other half of the idea: Mayhem's *UI and apps*, rebuilt as a host
application, with UHD and the B200 standing in for the SGPIO bus and the
HackRF's front end. The window is a real 240x320 framebuffer drawn with Mayhem's
own fonts, palette and widget layout; your keyboard and mouse are the five-way
switch, the encoder and the touch panel.

The parts of Mayhem that were hardware-independent — geometry, fonts, painter,
focus manager, theme, the spectrum palette — are used unchanged. See
[NOTICE.md](NOTICE.md) for exactly which files, and what was modified.

### What you gain over a PortaPack

- **Up to 56 MHz of instantaneous bandwidth** instead of the HackRF's 20 MHz,
  so the spectrum app shows a live wide span rather than a swept one.
- **A real CPU for the DSP**, so the channel filters are designed at run time
  for whatever sample rate you pick instead of being fixed tap tables.
- **LO offset tuning**, which moves the direct-conversion LO spike out of the
  displayed band — the PortaPack has the same artefact and no way to avoid it.
- **Capture while listening.** Recording taps the same samples the demodulator
  is already using, so IQ recording is not an exclusive mode.

### What you lose

It is not pocket-sized and it needs a host. That was always the trade.

---

## Requirements

| | |
|---|---|
| Hardware | Ettus USRP B200 (B210, B200mini and B205mini should also work — every limit is read from the device, not hard-coded) |
| OS | Windows 10/11 x64 |
| Compiler | MSVC 2022 (Build Tools are enough) |
| UHD | 4.x with headers and `uhd.lib` — the stock Windows installer |
| Boost | headers only; UHD's public headers include `<boost/format.hpp>` |
| CMake | 3.20+ |

No other dependencies. The window is plain Win32/GDI and the audio is WinMM, so
there is no SDL, Qt or FFTW to install.

## Building

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
```

If UHD or Boost are not in the default locations, point at them:

```bash
cmake -S . -B build -G Ninja -DUHD_ROOT="C:/Program Files/UHD" -DBOOST_INCLUDE_DIR="C:/vcpkg/installed/x64-windows/include"
```

Then build and test:

```bash
cmake --build build
```

```bash
ctest --test-dir build --output-on-failure
```

`uhd.dll` is copied next to the binary automatically, so the build tree runs
as-is.

## Running

```bash
build\mayhem-b200.exe
```

| Option | Meaning |
|---|---|
| `--args=<uhd args>` | Pick a device, e.g. `--args=type=b200,serial=31C9297` |
| `--scale=<1..6>` | Window magnification (default 2) |
| `--list` | List attached USRPs and exit |
| `--help` | Usage |

The app starts and stays usable with no radio attached — it shows `no dev` in
the status bar, and **Radio setup → Reconnect** picks the device up when you
plug it in.

### Controls

| Input | PortaPack equivalent |
|---|---|
| Arrow keys | five-way switch |
| Enter / Space | Select |
| Escape / Backspace | Back |
| Mouse wheel | rotary encoder |
| PageUp / PageDown | encoder, one detent |
| Left mouse button | touch panel |
| F11 | cycle window scale |

In the audio app, Left/Right tune by the current step no matter which control
has focus. Select on the frequency field cycles the step size
(1 Hz through 1 MHz, including 6.25 / 12.5 / 25 kHz).

## Apps

| App | State |
|---|---|
| **Receive audio** | AM (DSB 9k/6k, USB, LSB, CW), NFM (8k5/11k/16k), WFM (200k/180k), spectrum-only. Squelch, volume, hardware AGC, audio AGC, de-emphasis, waterfall, tune-by-tap. |
| **Spectrum** | Live wide span up to the device's maximum rate, trace + waterfall, peak hold, tune-by-tap. |
| **Capture IQ** | `.C16` interleaved int16 IQ plus a Mayhem-format `.TXT` sidecar. Runs alongside audio. |
| **Radio setup** | Sample rate, antenna port, LO offset, DC-offset and IQ-balance correction, device capabilities, overflow/drop counters. |
| **About** | Version, controls, credits. |

Recordings and frequency lists go under
`Documents\mayhem-b200\CAPTURES` and `...\FREQMAN`.

### Not implemented yet

Transmit (the UHD TX path and audio capture exist and are tested, but no TX app
is wired up), the protocol decoders (ADS-B, AIS, POCSAG, TPMS, ERT, BLE...),
Recon/scanner, the frequency manager UI, replay from file, and the Looking Glass
sweep. The receive chain and widget set they would build on are in place.

## How it fits together

```
             UHD  ──►  UsrpRadio  ──►  lock-free ring  ──►  ReceiverModel
                       (streaming                            (NCO → channel FIR
                        threads)                               → demod → audio
                                                                filter → de-emph
                                                                → AGC → resample)
                                                                      │
                            ┌─────────────────────────────────────────┤
                            ▼                                         ▼
                       waveOut                                  spectrum tap
                                                                      │
   Win32 window ◄── framebuffer ◄── Painter ◄── widget tree ◄── FFT ───┘
   (keyboard/mouse)   240x320       (Mayhem)     (Mayhem)
```

- `src/ui` — Mayhem's UI core plus the host display, window and input
- `src/dsp` — filters, FFT, demodulators, resampling, ring buffers
- `src/radio` — the UHD backend and the receive chain
- `src/audio` — WinMM output and capture
- `src/apps` — navigation and the app views
- `src/core` — string formatting, paths

The DSP is not a copy of Mayhem's fixed-point M4 code; it is float, and the
filters are designed at run time so they track the sample rate. The modes and
their bandwidths deliberately match Mayhem's labels.

## Tests

86 tests, no framework dependency:

```bash
build\tests\mb200_tests.exe
```

They cover the string formatting (locked to the firmware's exact output,
including its slightly surprising `-0042` sign padding), the DSP against
analytically-known results (FM deviation scaling, SSB opposite-sideband
rejection, FFT bin placement, resampling ratios, squelch hysteresis), the
display's scroll-region arithmetic, widget behaviour, and every no-device path
in the radio layer.

## Status and caveats

**This has not been run against a physical B200.** No USRP was attached to the
machine it was developed on, so the UHD calls, streaming threads, tuning, gain
control and IQ capture are verified by construction and by their no-device
paths, not by observed RF. Expect the hardware bring-up to need a pass. The UI,
DSP and file handling are exercised directly by the tests and by running the
application.

## Licence

GPL-2.0-or-later, inherited from PortaPack Mayhem. See [NOTICE.md](NOTICE.md)
for attribution and [LICENSE.GPL-2.0-or-later](LICENSE.GPL-2.0-or-later) for
terms. No warranty.

You are responsible for what you transmit. Most of the B200's tuning range is
licensed to somebody.
