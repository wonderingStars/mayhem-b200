# Full app inventory

Every Mayhem app, its upstream source, category, and porting status. The goal is
all of them. Apps whose entire purpose is PortaPack-only hardware are ported as
honest "not applicable on B200" screens explaining why, never faked.

Legend: [x] done · [~] in progress · [ ] pending · [HW] hardware-limited on B200

## Built-in — menus & core (upstream: application/)

- [x] Audio (analog_audio_app) — receiver
- [x] Capture (capture_app)
- [x] Spectrum / Looking Glass (host spectrum_app stands in; full GlassView pending)
- [x] Radio setup (device_app, host-specific)
- [x] About (host)
- [ ] Receive / Transmit / Transceiver / Utilities / Games / Settings menu views (icon grid)
- [ ] Recon (ui_recon)
- [ ] Replay / Playlist (ui_playlist)
- [ ] Search (ui_search)
- [ ] Settings screens (ui_settings)
- [ ] File Manager (ui_fileman)
- [ ] Freq. Manager (ui_freqman)
- [ ] IQ Trim (ui_iq_trim)
- [ ] Notepad / Text editor (ui_text_editor)
- [ ] Debug menu (ui_debug) [HW: much is LPC/CPLD/pmem — port what maps]
- [ ] Flash Utility (ui_flash_utility) [HW: flashes PortaPack firmware — N/A screen]

## Built-in — RX (upstream: application/apps/)

- [ ] ADS-B RX (ui_adsb_rx)
- [ ] AIS Boats (ais_app)
- [ ] APRS RX (ui_aprs_rx)
- [ ] BLE Rx (ble_rx_app) [HW-ish: 2.4 GHz is at the B200's ceiling — works, note it]
- [ ] POCSAG (pocsag_app)
- [ ] Radiosonde (ui_sonde)
- [ ] SubGhzD (ui_subghzd)
- [ ] Weather (ui_weather / TPMS-style)

## Built-in — TX (upstream: application/apps/)

- [ ] APRS TX (ui_aprs_tx)
- [ ] BLE Tx (ble_tx_app)
- [ ] OOK / Encoders (ui_encoders)
- [ ] RDS (ui_rds)
- [ ] TouchTune (ui_touchtunes)
- [ ] Mic TX (ui_mictx)

## External — RX decoders (upstream: application/external/)

- [ ] ACARS (acars_rx)
- [ ] ADS-B — external variant folds into built-in
- [ ] AFSK (afsk_rx)
- [ ] Analog TV (analogtv)
- [ ] Detector (detector_rx)
- [ ] EPIRB RX (epirb_rx)
- [ ] ERT Meter (ert)
- [ ] FLEX RX (flex_rx)
- [ ] FM Radio (fmradio)
- [ ] Fox hunt (foxhunt)
- [ ] FPV Detect (fpv_detect)
- [ ] Level / signal level (level)
- [ ] Morse RX (morse_radio)
- [ ] NOAA APT (noaaapt_rx)
- [ ] NRF (nrf_rx)
- [ ] ProtoView (protoview)
- [ ] RTTY RX (rtty_rx)
- [ ] Scanner (scanner)
- [ ] Signal Hunter (signal_hunter)
- [ ] SSTV RX (sstvrx)
- [ ] SubCar (subcarrx)
- [ ] Tetra (tetra_rx)
- [ ] Time Sink (time_sink)
- [ ] TPMS RX (tpmsrx)
- [ ] Two-Tone RX (two_tone_rx)
- [ ] VOR RX (vor_rx)
- [ ] WeFax (wefax_rx)
- [ ] gfxeq (audio spectrum visualiser)

## External — TX (upstream: application/external/)

- [ ] ADSB-TX (adsbtx)
- [ ] Adult Toys (adult_toys_controller)
- [ ] BHT TX (bht_tx)
- [ ] BLESpam (blespam)
- [ ] BurgerPgr / coaster pager (coasterp)
- [ ] CVS Spam (cvs_spam)
- [ ] EPIRB TX (epirb_tx)
- [ ] FLEX TX (flex_tx)
- [ ] FlipperTx (flippertx)
- [ ] GPS Sim (gpssim)
- [ ] Hopper (hopper)
- [ ] Jammer (jammer) [emits deliberate interference — implement with a clear legality warning]
- [ ] KeeLoq TX (keeloqtx)
- [ ] Keyfob (keyfob)
- [ ] LGE (lge)
- [ ] TEDI/LCR (lcr)
- [ ] MDC TX (mdc_tx)
- [ ] Morse TX (morse_tx, morseradiotx)
- [ ] OOK Brute (ookbrute)
- [ ] OOK Editor (ook_editor)
- [ ] P25 TX (p25_tx)
- [ ] POCSAG TX (pocsag_tx)
- [ ] RTTY TX (rtty_tx)
- [ ] SAME TX (same_tx) [EAS alert tones — legality warning]
- [ ] Security+ (secplustx)
- [ ] Shopping cart lock (shoppingcart_lock)
- [ ] Signal gen (siggen)
- [ ] Soundboard (soundboard)
- [ ] SSTV TX (sstvtx)
- [ ] Space Painter (spainter)
- [ ] Two-Tone pager (two_tone_pager)
- [ ] VOR TX (vor_tx)
- [ ] Coaster / restaurant pager family (coasterp)

## External — TRX

- [ ] KISS TNC (kiss_tnc)

## External — Utilities

- [ ] Antenna Length (antenna_length)
- [ ] Calculator (calculator)
- [ ] Metronome (metronome)
- [ ] Playlist Editor (playlist_editor)
- [ ] Random Password (random_password)
- [ ] Shopping cart lock — see TX
- [ ] Stopwatch (stopwatch)
- [ ] Tuner (tuner)
- [ ] WAV Viewer (wav_view)
- [ ] Wardrive Map (wardrivemap)
- [ ] Waterfall Designer (waterfall_designer)
- [ ] SD over USB (sdusb) [HW: N/A screen]
- [ ] Wipe SD card (sd_wipe) [HW: N/A screen]

## External — Games

- [ ] 2048 (game2048)
- [ ] Battleship (battleship)
- [ ] Blackjack (blackjack)
- [ ] Breakout (breakout)
- [ ] Dino game (dinogame)
- [ ] DOOM (doom) [large; port the shell + note scope]
- [ ] Morse practice (morse_practice)
- [ ] Snake (snake)
- [ ] Space Invaders (spaceinv)
- [ ] Tetris (tetris)

## External — Debug / host-mapped

- [ ] App Manager (app_manager) [N/A: apps are compiled in, not loaded from SD]
- [ ] Audio Test (audio_test)
- [ ] Debug PMem (debug_pmem) [maps to core::Settings dump]
- [ ] Ext Sensors (extsensors) [HW: I2C sensors — N/A screen]
- [ ] Font Viewer (font_viewer)
- [ ] Hard Reset (hard_reset) [maps to "reset settings"]
- [ ] MCU Temperature (mcu_temperature) [HW: LPC sensor — show host info instead or N/A]
- [ ] Remote (remote)

## Deliberate scope notes

- Apps that transmit (jammer, spam family, SAME, EAS) are ported because they are
  in Mayhem; each carries an on-screen legality warning and none transmit without
  an explicit user action. Nothing here auto-transmits.
- 2.4 GHz apps (BLE, some NRF) sit at the top of the B200's 6 GHz range and work,
  but coverage/quality differs from a dedicated 2.4 GHz radio — noted in-app.
- DOOM and Analog TV are large; port the working shell and state clearly what is
  and is not wired up.
