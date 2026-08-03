/*
 * mayhem-b200 — APRS transmitter.
 *
 * Ported from the PortaPack firmware:
 *
 *   application/apps/ui_aprs_tx.*      -> APRSTXView (the on-screen form and the
 *                                         transmit trigger)
 *   application/protocols/aprs.cpp     -> make_aprs_frame (address layout, SSID
 *                                         octets, digipeater-path parsing)
 *   application/protocols/ax25.cpp     -> AX25Frame::make_ui_frame (flags,
 *                                         callsign shift, extension bit, FCS,
 *                                         five-ones bit stuffing, NRZI)
 *   application/protocols/ax25.hpp     -> the FCS parameters
 *                                         (CRC<16,true,true>{0x1021,0xFFFF,0xFFFF})
 *   baseband/proc_afsk.cpp             -> the Bell 202 AFSK keying the frame is
 *                                         played through (mark 1200 Hz, space
 *                                         2200 Hz, 1200 baud, over NBFM)
 *
 * APRS is 1200 bps Bell 202 AFSK carried by NBFM, framed as AX.25 UI frames.
 * The transmit chain the host reproduces is:
 *
 *   fields -> AX.25 UI frame (address, control, PID, info, FCS)
 *          -> NRZI + HDLC bit-stuffing + flags        (AprsTxFrame::bits())
 *          -> Bell 202 AFSK audio (dsp::afsk_modulate)
 *          -> NBFM (radio::TransmitterModel, AudioSource) -> B200
 *
 * The encoder is the deliverable and is what tests/test_aprs_tx.cpp exercises:
 * the exact shifted address octets, the FCS against known vectors, the stuffed
 * zero after five ones, the NRZI sense, and a full round trip back through the
 * Phase A AFSK demodulator and the AX.25 decoder in ui_aprs_rx.hpp.
 *
 * FAITHFULNESS AND ONE DELIBERATE PARAMETER. The AX.25 frame this builds is
 * byte-for-byte upstream's: the same 14-byte address (dest, then src, each 6
 * callsign bytes plus an SSID octet), the same 0x03/0xF0 control/PID, the same
 * reflected CRC-16/X-25 appended low byte first, the same LSB-first bit order,
 * the same five-ones stuffing, the same NRZI. Upstream's make_ui_frame emits
 * exactly four opening flags and two closing flags and lets proc_afsk repeat the
 * whole buffer; here the opening-flag count (the TNC "TXDelay" preamble) is a
 * build() parameter that defaults to upstream's four, and the transmit path
 * sends a longer preamble so the receiver's AFSK slicer and bit clock have time
 * to settle. That is a standard TNC preamble, not a change to the frame.
 *
 * One host robustness fix: the callsign read out of the SymField is stripped of
 * spaces before encoding. SymField right-aligns and space-pads, so a pre-filled
 * or partially edited field would otherwise carry stray spaces into the address;
 * a callsign never contains one and AX.25 pads the field on the right anyway.
 * Upstream passes the raw widget string through.
 *
 * LEGALITY. Transmitting on the APRS/amateur allocations requires a valid
 * amateur-radio licence and use of an authorised frequency; the view says so on
 * screen. Nothing is radiated until the operator presses START TX, and only then
 * with a USRP B200 attached — there is no auto-transmit and no beaconing timer.
 *
 * NO HARDWARE. No B200 is attached during development, so actual radiation is
 * unverified. The encoder logic is real and tested; the RF path is not.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_APRS_TX_H__
#define __MB200_UI_APRS_TX_H__

#include "../dsp/protocol.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace app {

/* ===========================================================================
 * AprsTxFrame — the AX.25 UI-frame encoder
 *
 * Transcribed from application/protocols/aprs.cpp (make_aprs_frame) and
 * application/protocols/ax25.cpp (AX25Frame). The firmware wrote the NRZI bytes
 * straight into shared_memory.bb_data for the M4; here the same bit sequence is
 * appended to a vector, one physical line level per element, which is exactly
 * what dsp::afsk_modulate and app::Ax25Decoder consume. No shared state, so it
 * is trivially testable.
 * ===========================================================================*/

class AprsTxFrame {
   public:
    /* Builds the whole frame from its parts, the union of make_aprs_frame() and
     * make_ui_frame(). `path` is the human-readable digipeater path with dashes
     * and commas, e.g. "WIDE1-1,WIDE2-1"; it is parsed exactly as upstream does.
     *
     * lead_flags / trail_flags are the opening and closing HDLC flag counts.
     * They default to upstream's four and two; the transmit path passes a longer
     * lead-in as a TXDelay preamble. */
    void build(const std::string& src,
               uint8_t src_ssid,
               const std::string& dest,
               uint8_t dest_ssid,
               const std::string& payload,
               const std::string& path,
               size_t lead_flags = 4,
               size_t trail_flags = 2);

    /* The byte sequence the FCS is computed over and that goes on the wire
     * before the FCS: the shifted address octets, the optional shifted path,
     * the 0x03 control byte, the 0xF0 PID and the information field. Matches the
     * layout ui_aprs_rx.hpp's AprsPacket parses. */
    const std::vector<uint8_t>& frame_bytes() const { return frame_bytes_; }

    /* CRC-16/X-25 over frame_bytes(): polynomial 0x1021, reflected in and out,
     * init 0xFFFF, final XOR 0xFFFF. Transmitted low byte first. */
    uint16_t fcs() const { return fcs_; }

    /* The complete NRZI + bit-stuffed physical line-level stream, flags
     * included: one uint8_t of value 0 or 1 per transmitted bit. This is the
     * input to the AFSK modulator and, unchanged, to app::Ax25Decoder. */
    const std::vector<uint8_t>& bits() const { return bits_; }

    /* --- helpers, exposed for the view and the tests --- */

    /* The 14-byte AX.25 address block BEFORE the left-shift: destination
     * callsign (6, space-padded) + destination SSID octet (ssid | 0x30), then
     * the source callsign (6) + source SSID octet. */
    static std::string build_address(const std::string& dest,
                                     uint8_t dest_ssid,
                                     const std::string& src,
                                     uint8_t src_ssid);

    /* Parses a "CALL-n,CALL-n" path into packed 7-byte address groups, each six
     * upper-cased space-padded callsign bytes plus an (ssid | 0x30) octet, with
     * no dashes or commas — the form make_ui_frame() expects. */
    static std::string fix_path(const std::string& path);

    /* Port of APRSTXView::process_coordinates: signed degrees to the APRS
     * uncompressed "DDMM.mmN/DDDMM.mmE" position string. */
    static std::string format_coordinates(float latitude, float longitude);

   private:
    void reset();
    void nrzi_emit(uint8_t bit);
    void add_byte(uint8_t byte, bool is_flag, bool is_data);
    void add_flag() { add_byte(0x7E, true, false); }
    void add_data(uint8_t byte) { add_byte(byte, false, true); }
    void make_extended_field(const uint8_t* data, size_t length, bool is_last);
    void add_checksum();

    std::vector<uint8_t> bits_{};
    std::vector<uint8_t> frame_bytes_{};
    dsp::Crc<16, true, true> crc_{0x1021, 0xFFFF, 0xFFFF};
    uint16_t fcs_{0};
    uint8_t level_{0};  /* NRZI line state */
    uint8_t ones_{0};   /* consecutive-ones counter for bit stuffing */
};

/* ===========================================================================
 * AprsTxView
 * ===========================================================================*/

class AprsTxView : public ui::View {
   public:
    AprsTxView();
    ~AprsTxView() override;

    AprsTxView(const AprsTxView&) = delete;
    AprsTxView& operator=(const AprsTxView&) = delete;

    std::string title() const override { return "APRS TX"; }

    void focus() override;
    void on_frame_sync() override;

   private:
    /* Bell 202 AFSK, 1200 baud. */
    static constexpr float kMarkHz = 1200.0f;
    static constexpr float kSpaceHz = 2200.0f;
    static constexpr float kBaud = 1200.0f;
    /* Opening flags sent as a TXDelay preamble (~0.3 s at 1200 baud). */
    static constexpr size_t kPreambleFlags = 48;
    /* APRS on 2 m NBFM: ~3.5 kHz deviation, the Medium11k channel. */
    static constexpr double kDeviationHz = 3500.0;

    void build_frame();
    void update_preview();
    std::string tnc2_line() const;
    std::string current_payload() const;  /* with ?GPS? expanded */

    void start_tx();
    void stop_tx();
    void log(std::string_view line);

    /* --- transmit state (read on the DSP thread) --- */
    std::vector<float> tx_audio_{};
    std::atomic<size_t> tx_pos_{0};
    std::atomic<bool> transmitting_{false};
    uint32_t tail_frames_{0};

    /* --- model fields --- */
    std::string src_call_{""};
    std::string dest_call_{"APRS"};
    std::string path_{"WIDE1-1"};
    std::string payload_{""};
    std::string prompt_buffer_{};

    AprsTxFrame frame_{};

    /* --- widgets --- */
    ui::Labels labels_{
        {{0, 0}, "Src", ui::Color::light_grey()},
        {{88, 0}, "SSID", ui::Color::light_grey()},
        {{0, 16}, "Dst", ui::Color::light_grey()},
        {{88, 16}, "SSID", ui::Color::light_grey()},
        {{0, 34}, "Path", ui::Color::light_grey()},
        {{0, 50}, "Info", ui::Color::light_grey()},
        {{0, 66}, "Pos", ui::Color::light_grey()},
        {{0, 98}, "Freq", ui::Color::light_grey()},
        {{0, 114}, "Gain", ui::Color::light_grey()},
    };

    ui::SymField sym_source_{{32, 0}, 6, ui::SymField::Type::Alpha};
    ui::NumberField num_ssid_source_{{128, 0}, 2, {0, 15}, 1, ' '};
    ui::SymField sym_dest_{{32, 16}, 6, ui::SymField::Type::Alpha};
    ui::NumberField num_ssid_dest_{{128, 16}, 2, {0, 15}, 1, ' '};

    ui::TextField field_path_{{40, 34, 200, 16}, "WIDE1-1"};
    ui::TextField field_payload_{{40, 50, 200, 16}, "(set info)"};

    ui::FloatField field_lat_{{32, 66}, 7, {-90.0f, 90.0f}, 0.0001f, ' ', false, 4};
    ui::FloatField field_lon_{{104, 66}, 9, {-180.0f, 180.0f}, 0.0001f, ' ', false, 4};
    ui::Button button_gps_{{192, 64, 44, 20}, "Set"};
    ui::Text text_gps_{{0, 82, 240, 16}, "-"};

    ui::FrequencyField field_frequency_{{40, 98}};
    ui::FrequencyStepView field_step_{{160, 98}, field_frequency_};
    ui::NumberField field_gain_{{40, 114}, 3, {0, 89}, 1, ' '};

    ui::Labels legality_{
        {{0, 134}, "TX needs an amateur licence", ui::Color::yellow()},
        {{0, 150}, "and an authorised frequency.", ui::Color::yellow()},
    };

    ui::Console console_{{0, 170, 240, 98}};

    ui::Button button_tx_{{0, 272, 116, 30}, "START TX"};
    ui::Button button_stop_{{124, 272, 116, 30}, "STOP"};
};

}  // namespace app

#endif /*__MB200_UI_APRS_TX_H__*/
