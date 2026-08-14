/*
 * mayhem-b200 — about / help screen.
 *
 * Copyright (C) 2026 mayhem-b200 contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_ABOUT_APP_H__
#define __MB200_ABOUT_APP_H__

#include "ui.hpp"
#include "ui_widget.hpp"

#include <string>

namespace app {

/* Project version, shown here and in the window title.
 * 0.9.0: full Mayhem app suite ported (~100 apps across all categories),
 * software-complete and tested, not yet verified against a physical B200.
 * 0.9.1: ADS-B frame amplitude is referred to the measured noise floor and
 * carried as a float, so the Amp column and the portal's Sig column report a
 * real signal strength on a B200 instead of zero.
 * 0.9.2: the web portal streams the 240x320 framebuffer (GET /api/screen) and
 * routes input back (POST /api/input), so every app is operable from a
 * browser rather than only the handful with a structured panel.
 * 0.9.3: the portal now reads which app is open off the navigation stack
 * instead of remembering the last launch, so navigating with the keys no
 * longer serves one app's panel under another app's title; GET /api/panel
 * honours ?have_image_rev=N; and a bare GET /api/screen keeps working past
 * frame 2^31.
 * 0.10.0: the receiver's sample tap is a gapless ring buffer rather than a
 * destructive snapshot, and the rate is chosen from the attached radio's
 * reported capabilities instead of being fixed at the HackRF's. Measured on
 * live 1090 MHz traffic in paired alternating windows: 0.485 -> 22.88 accepted
 * ADS-B frames/sec, 2-3 -> 7-11 aircraft tracked. The web portal is
 * redesigned around the same work.
 * 0.11.0: a dropped sdrlink connection recovers instead of ending the
 * session. The control link and the IQ stream both re-establish themselves
 * with backed-off retries, the session's tuning is replayed onto the new
 * connection, and GET /api/status carries a "link" field so a link that is
 * coming back is distinguishable from one that has given up.
 * 0.11.1: a keepalive ping, so an IDLE client notices a dead link at all.
 * Verified against a live sdrlink server and a B200: without it, killing the
 * server left the client reporting "connected" indefinitely.
 * 0.11.2: GET /api/status carries can_transmit, from the attached radio's
 * DeviceCaps.has_tx, so the portal can lock the transmit apps on a
 * receive-only SDR instead of offering ~28 apps that cannot work. Sent only
 * while a device is open: with nothing attached there is no honest answer.
 * 0.11.3: rebuild the RX streamer when UHD moves the B200's master clock.
 * Changing the sample rate while streaming re-initialises the radio under the
 * streamer already handed out, and the receiver then goes silently deaf --
 * which is why an app opened directly from another app could fail to
 * receive.
 * 0.11.4: hardware-gated regression tests (MB200_HW_TESTS=1) run the
 * clock-move and transmit paths against a real USRP; first hardware evidence
 * the TX plumbing works. set_rx_rate keeps caps().master_clock_rate honest on
 * every call. Test-suite hardening: no fixture collides across concurrent
 * processes, no detached thread can dangle a stack frame, and scheduling
 * assumptions under load became guarantees.
 * 0.12.0: the capability round. GET /api/apps carries panel_kind so the grid
 * badges apps with native views; analog RX bandwidth follows the chosen rate
 * and gain is validated against the attached radio's caps (capability_policy,
 * beside rate_policy); FPV Detect's retune storm and frozen level are fixed
 * (first reading now ~1 s, was 10-15 s); the sdrlink reconnect replays gain
 * in dB rather than Hz; and the USRP RX thread survives the app-switch race
 * (start-command retry across streamer generations, plus a starvation
 * watchdog that re-kicks a stream the hardware accepted but never fed).
 * 0.12.3: the spectrum and receiver panel payloads speak the names PANELS.md
 * documents and the renderers read (center_hz/sample_rate_hz/type/floor_db/
 * ceil_db; level_db and real gain bounds from caps) -- the old dialect left
 * every spectrum panel reading 0.000 MHz over live bins. The panel provider
 * PEEKS the spectrum snapshot instead of consuming it, so it no longer
 * steals frames from sweeping apps. Golden per-kind payload fixtures pin
 * every panel kind''s wire shape.
 * 0.12.1: every test fixture is unique per process. Fixed temp names and a
 * fixed FREQMAN stem let two concurrently-running suites (one per git
 * worktree) delete each other's live fixtures mid-test; verified clean with
 * two suites looping side by side, 12 rounds each.
 * 0.12.2: the transmit frequency is verified on hardware for the first time.
 * A full-duplex self-loopback test (TX/RX port to RX2, internal leakage, no
 * antenna, minimum gain) transmits a +100 kHz tone and finds it in the
 * received spectrum within one FFT bin, at 433.92 MHz and 2.45 GHz. */
/* 0.13.0: AIS gets an ADS-B-grade vessel chart. A dedicated "ais" panel
 * kind carries everything the app decodes per ship (name, callsign,
 * destination, SOG, COG, true heading, nav status, message count), each
 * field omitted until genuinely received; ais.js draws hull-shaped targets
 * rotated to heading on OSM water, course/speed vectors, per-MMSI trails,
 * nav-status colours, and a chart-linked sortable table. */
/* 0.14.0: every geo-capable app has a map. Radiosonde RX gains its first
 * panel and EPIRB RX upgrades from table to geotable — both with hard
 * validity gates (EPIRB additionally fixes upstream's is_unknown() blind
 * spot: the 7-bit latitude "not available" sentinel decoded as 127 deg N,
 * a fabricated distress position). The shared map renderer draws the OSM
 * street basemap under its markers (WardriveMap, APRS, and the geotable
 * hosts), true Web Mercator, honest offline degrade to the graticule. */
/* 0.14.1: leaving an app leaves the radio idle. Navigation stops the
 * receiver and transmitter centrally when it lands at the menu -- the old
 * behaviour muted only the speaker while the radio kept streaming into a
 * demodulator nobody was watching. App-to-app switches do not bounce the
 * stream. */
/* 0.14.2: the audio output goes idle when nothing feeds it. Pure decoder
 * apps (ERT, ADS-B, POCSAG...) and the menu produce no audio, and the DAC
 * used to clock digital silence through the speakers forever anyway; the
 * feeder now parks after a buffer's worth of dry blocks and one write()
 * wakes it. */
/* 0.15.0: only the apps that need sound make it. A speaker-monitor flag on
 * ReceiverModel (default on, re-asserted by every set_mode) lets the eight
 * pure data/image decoders — ACARS, FLEX, POCSAG, APRS, AFSK, NOAA APT,
 * WEFAX, 2-Tone — skip the demod-to-speaker chain entirely; they decode from
 * their own tap and the audio was only modem tones. Listening apps (FM,
 * Audio, VOR's Morse ident, Morse, Foxhunt, Scanner...) are untouched.
 * GET /api/status carries audio_monitor. Keyfob's TX-readiness messaging is
 * honest now (it works on a B200 — verified on hardware — and reports the
 * radio's real error on failure instead of always blaming an absent B200). */
/* 0.16.0: the Morse app gets a native browser panel — a live CW decoder
 * (received text, WPM, tone) AND a text-to-Morse transmitter. Typing shows the
 * dots/dashes and can play them locally (Web Audio); a Transmit button keys
 * the radio for real over the air via POST /api/morse/transmit. It is the
 * portal's one interactive-transmit path: every safety gate is server-side
 * (a transmit-capable idle radio, encodable text, a frequency in the device's
 * TX range — so HF CW below the B200's ~70 MHz floor is refused honestly), the
 * keying runs on the UI thread and auto-stops, and the panel carries the
 * licensing warning. Verified on the B200: CW keyed at 144.2 MHz. */
/* 0.17.0: weak-signal front-end tuning. A per-app RX analog-bandwidth hint
 * (set_rx_bandwidth_hint, cleared per rate change) narrows the filter to suit
 * the signal instead of the sample rate: ADS-B 4 MHz (from ~8), AIS 150 kHz
 * (from 307), radiosonde 800 kHz (from 2.46) — 3-7 dB less noise into the
 * front end, which recovers the weak distant frames these apps live on.
 * Verified non-regressive on the B200 (ADS-B still decodes weak aircraft; AIS
 * and sonde still stream). The browser Morse transmit now keys at a defined
 * conservative gain (default 30 dB, clamped to caps) rather than inheriting
 * whatever a previous app left on the transmitter. */
/* 0.17.1: the map panels remember where you are. Each map (ADS-B, AIS, and
 * the shared renderer behind WardriveMap / APRS / EPIRB / Radiosonde) used to
 * open at 0,0 off Africa and only re-centre once the first positioned marker
 * arrived, so with sparse fixes you stared at the ocean and panned up every
 * time. The view (centre + zoom) now persists to localStorage on pan/zoom/fit
 * and restores on open — so after the first fit it always reopens on your
 * signals, wherever in the world you are. First-ever open still fits to the
 * first marker as before. */
/* 0.18.0: anonymous, opt-out usage counting. A Cloudflare Worker
 * (analytics-worker/) counts distinct installs via Analytics Engine; the app
 * sends one ping a day carrying only a random local install id, the version
 * and the OS name (no PII, no IP logged), on a detached fail-silent thread,
 * disableable with --no-telemetry and inert unless the build sets the
 * endpoint. Disclosed in the README and at startup. */
/* 0.18.1: the usage-counter Worker is deployed and the endpoint is now wired
 * to it (mayhem-b200-usage.bondvpn.workers.dev), so builds actually count.
 * End-to-end verified against the live Worker; README query syntax corrected
 * (count(DISTINCT index1), not uniq). */
/* 0.19.0: runs as a background appliance. A real shutdown path (core/quit.hpp)
 * means Ctrl+C, closing the console and Windows logoff now run the same
 * teardown the window's X always did — before, all of those were hard kills
 * that skipped the transmit end-of-burst and left the B200 claimed. Teardown
 * also idles TX gain explicitly. --hidden runs with no GUI window (the browser
 * portal is the UI), --log-file keeps the diagnostics a hidden console would
 * swallow, and a tray icon carries Show window / Quit — the local control a
 * hidden window would otherwise have none of. The launchers start everything
 * in the background. */
constexpr const char* kVersion = "0.19.0";

class AboutView : public ui::View {
   public:
    AboutView();

    std::string title() const override { return "About"; }

    void on_show() override;

   private:
    ui::Console console_{{0, 4, 240, 272}};
    ui::Button button_done_{{72, 280, 96, 24}, "Back"};
};

}  // namespace app

#endif /*__MB200_ABOUT_APP_H__*/
