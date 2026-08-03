/*
 * mayhem-b200 — KISS TNC (AX.25 / APRS, 1200-baud AFSK).
 *
 * Ported from firmware/application/external/kiss_tnc/ui_kiss_tnc.* and
 * firmware/application/protocols/ax25.* . A KISS TNC bridges a host (running
 * something like Dire Wolf, Xastir or an APRS client) to the radio: KISS frames
 * arrive over a serial link, are unwrapped, turned into an AX.25 frame, and
 * transmitted as 1200-baud Bell-202 AFSK; received AX.25 frames go back the
 * other way, KISS-wrapped.
 *
 * This port provides the protocol pieces, all host-testable:
 *   - the KISS framing codec (FEND 0xC0 delimiters; FESC 0xDB escaping of FEND
 *     as DB DC and FESC as DB DD), encode and a streaming decoder;
 *   - the AX.25 encoder (four opening 0x7E flags, HDLC bit stuffing after five
 *     ones, the X.25 FCS = CRC-16/X-25, NRZI, two closing flags), both
 *     make_frame_from_raw (wrap a raw AX.25 frame from a KISS payload) and
 *     make_ui_frame (compose a UI frame from callsigns + text);
 *   - AFSK modulation of the resulting NRZI bit stream through the transmitter.
 *
 * HOST MAPPING: on the PortaPack the KISS bytes travel over the device's USB
 * serial (usb_serial). This host build has no serial/TCP bridge wired, so the RX
 * -> host path is not present; the view documents this and offers a manual
 * compose-and-transmit path instead. Wiring a real endpoint means feeding bytes
 * to KissDecoder and calling kiss_encode() on decoded APRS packets.
 *
 * Transmit starts only on an explicit TX press.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek (ax25)
 * Copyright (C) 2024 Sarah Rose (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_KISS_TNC_H__
#define __MB200_UI_KISS_TNC_H__

#include "../dsp/protocol.hpp"
#include "../dsp/ring_buffer.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace radio {
class TransmitterModel;
}

namespace app {
namespace kiss {

inline constexpr uint8_t FEND = 0xC0;
inline constexpr uint8_t FESC = 0xDB;
inline constexpr uint8_t TFEND = 0xDC;
inline constexpr uint8_t TFESC = 0xDD;

/* KISS-wrap `data`: FEND, command byte (port<<4 | cmd), FESC-escaped payload,
 * FEND. Default command 0x00 is a data frame on port 0 (upstream). */
std::vector<uint8_t> kiss_encode(const uint8_t* data, size_t len, uint8_t command = 0x00);
std::vector<uint8_t> kiss_encode(const std::vector<uint8_t>& data, uint8_t command = 0x00);

/* Streaming KISS decoder, the state machine from ui_kiss_tnc.cpp. Feed raw
 * bytes; each completed data frame's unescaped payload (command byte stripped)
 * is handed to the handler. */
class KissDecoder {
   public:
    using FrameHandler = std::function<void(const std::vector<uint8_t>&)>;

    void set_on_frame(FrameHandler h) { on_frame_ = std::move(h); }
    void reset();
    void feed(const uint8_t* data, size_t len);
    void feed(const std::vector<uint8_t>& data) { feed(data.data(), data.size()); }

   private:
    enum class State : uint8_t { Idle, Cmd, Data, Esc };
    void emit();

    State state_{State::Idle};
    std::vector<uint8_t> buf_{};
    FrameHandler on_frame_{};
};

/* X.25 frame-check sequence (CRC-16/X-25) over `data`, the AX.25 FCS. */
uint16_t ax25_fcs(const uint8_t* data, size_t len);

/* AX.25 UI-frame encoder. Produces the on-air NRZI bit stream (one uint8_t per
 * bit, transmit order) ready for the AFSK modulator. */
class AX25Frame {
   public:
    /* Wrap an already-formed AX.25 frame (address..info, no flags/FCS) — the
     * KISS payload path. */
    void make_frame_from_raw(const uint8_t* data, size_t len);

    /* Compose a UI frame: `address` is 14 bytes (dest 6+SSID, source 6+SSID),
     * control 0x03, protocol 0xF0, then `info`. */
    void make_ui_frame(const uint8_t* address, uint8_t control, uint8_t protocol,
                       const std::string& info);

    const std::vector<uint8_t>& bits() const { return bits_; }
    /* NRZI bits packed most-significant-bit first, the byte order upstream
     * hands to its modem. */
    std::vector<uint8_t> bytes() const;

   private:
    void begin();
    void nrzi_add_bit(uint32_t bit);
    void add_byte(uint8_t byte, bool is_flag, bool is_data);
    void add_data(uint8_t byte) { add_byte(byte, false, true); }
    void add_flag() { add_byte(0x7E, true, false); }
    void add_checksum();

    std::vector<uint8_t> bits_{};
    uint8_t current_bit_{0};
    uint8_t ones_{0};
    dsp::Crc<16, true, true> crc_{0x1021, 0xFFFF, 0xFFFF};
};

/* Builds the 14-byte AX.25 address field (dest then source) from callsigns and
 * SSIDs, un-shifted (make_ui_frame shifts each byte left one). */
std::vector<uint8_t> make_address(const std::string& dest, uint8_t dest_ssid,
                                  const std::string& source, uint8_t source_ssid);

/* --- View ----------------------------------------------------------------- */

class KissTncView : public ui::View {
   public:
    KissTncView();
    ~KissTncView() override;

    KissTncView(const KissTncView&) = delete;
    KissTncView& operator=(const KissTncView&) = delete;

    std::string title() const override { return "KISS TNC"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void start_tx();
    void stop_tx();

    radio::TransmitterModel* transmitter_{nullptr};
    bool transmitting_{false};

    static constexpr float kAudioRate = 48'000.0f;
    static constexpr float kMarkHz = 1200.0f;   /* Bell 202 mark = bit 1 */
    static constexpr float kSpaceHz = 2200.0f;  /* Bell 202 space = bit 0 */
    static constexpr float kBaud = 1200.0f;
    static constexpr float kDeviationHz = 3500.0f;

    dsp::RingBuffer<float> audio_ring_{1u << 18};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{0, 24}, "Src", ui::Color::light_grey()},
        {{0, 46}, "Dst", ui::Color::light_grey()},
        {{0, 68}, "Info", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};

    ui::TextField field_source_{{40, 24, 12 * 8, 16}, "MB200"};
    ui::TextField field_dest_{{40, 46, 12 * 8, 16}, "APRS"};
    ui::TextField field_info_{{40, 68, 24 * 8, 16}, ">host KISS bridge not wired"};

    ui::Button button_tx_{{160, 18, 72, 28}, "TX"};

    ui::Text text_status_{{0, 92, 240, 16}, "Idle"};
    ui::Console console_{{0, 112, 240, 136}};

    ui::Labels notes_{
        {{0, 268}, "KISS=USB serial on hardware;", ui::Color::grey()},
        {{0, 284}, "no host bridge - manual TX.", ui::Color::yellow()},
    };
};

}  // namespace kiss
}  // namespace app

#endif /*__MB200_UI_KISS_TNC_H__*/
