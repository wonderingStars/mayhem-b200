/*
 * mayhem-b200 — Bluetooth Low Energy advertising transmitter.
 *
 * Ported from firmware/application/apps/ble_tx_app.* and the baseband transmit
 * processor firmware/baseband/proc_ble_tx.* . The upstream app is a UI shell;
 * the whole protocol lives in BTLETxProcessor, so that is what is ported here.
 * The encoder builds an on-air advertising-channel PDU exactly as upstream does:
 *
 *   info bits  = preamble(0xAA) | access address(D6BE898E) | PDU header(16b)
 *                | AdvA(6 bytes, byte order reversed) | AdvData
 *   CRC-24     = LFSR over the PDU (header+AdvA+AdvData), preset 0x555555,
 *                appended after the PDU
 *   whitening  = 7-bit LFSR seeded {1, channel index bits}, applied to the
 *                PDU + CRC only (preamble and access address stay unwhitened)
 *   phy bits   = preamble | access address | whitened(PDU | CRC)
 *
 * Every octet is emitted least-significant-bit first, the BLE on-air order
 * (octet_hex_to_bit / convert_hex_to_bit upstream). The phy bit stream is then
 * GFSK modulated at 1 Msym/s, BT 0.5, modulation index 0.5 (±250 kHz
 * deviation) — the same numbers proc_ble_tx uses (SAMPLE_PER_SYMBOL 4,
 * MOD_IDX 0.5, its 16-tap Gaussian) — through dsp::design_gaussian_pulse +
 * dsp::FskKeyer.
 *
 * FREQUENCY: the advertising channels are 2402 / 2426 / 2480 MHz and the data
 * channels 2404–2478 MHz — the very top of a B200's 6 GHz range. The tuning
 * range is read from radio->caps() at run time and shown on screen; the app
 * never asserts a limit it has not verified.
 *
 * TRANSMIT NEVER STARTS ON ITS OWN: it begins only when the user presses TX,
 * and the built packet's bytes are shown first.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2023 TJ Baginski (original app / baseband)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_BLE_TX_H__
#define __MB200_UI_BLE_TX_H__

#include "../dsp/ring_buffer.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace radio {
class TransmitterModel;
}

namespace app {
namespace ble_tx {

/* Advertising PDU types, upstream PKT_TYPE (only the ADV subset the header
 * builder distinguishes). */
enum class PduType : uint8_t {
    ADV_IND = 0,
    ADV_DIRECT_IND = 1,
    ADV_NONCONN_IND = 2,
    SCAN_REQ = 3,
    SCAN_RSP = 4,
    CONNECT_REQ = 5,
    ADV_SCAN_IND = 6,
};

/* The advertising access address, transmitted LSB-first per octet. Assembled
 * the way a receiver assembles it (LSB-first into a 32-bit word) this is
 * 0x8E89BED6. */
inline constexpr char kAccessAddressHex[] = "D6BE898E";
inline constexpr char kPreambleHex[] = "AA";
inline constexpr uint32_t kCrcInit = 0x555555;

/* Expands a hex string to a bit vector, one uint8_t (0/1) per bit, each octet
 * least-significant-bit first (upstream octet_hex_to_bit). When `flip` is set
 * the octet order is reversed first (upstream stream_flip, used for AdvA so the
 * MAC goes out least-significant octet first). Returns empty on an odd or
 * over-limit hex length, mirroring convert_hex_to_bit's -1. */
std::vector<uint8_t> hex_to_bits(std::string_view hex, bool flip, size_t octet_limit);

/* The 16-bit advertising PDU header: 4-bit PDU type, 2 RFU, TxAdd, RxAdd,
 * 6-bit length, 2 RFU — laid out exactly as fill_adv_pdu_header. */
std::array<uint8_t, 16> adv_pdu_header(PduType type, int txadd, int rxadd, int payload_len);

/* The BLE whitening (data-channel scrambling) sequence for `channel`, `nbits`
 * long: a 7-bit LFSR (x^7 + x^4 + 1) seeded with {1, channel index bits}.
 * Port of proc_ble_tx scramble(). */
std::vector<uint8_t> whitening_sequence(int channel, size_t nbits);

/* XORs `bits` with the whitening sequence for `channel`. Its own inverse. */
std::vector<uint8_t> whiten(const std::vector<uint8_t>& bits, int channel);

/* CRC-24 over `pdu_bits` (the header+AdvA+AdvData bit stream), preset `init`,
 * returned as the 24 bits in transmit order. Port of proc_ble_tx crc24(). */
std::array<uint8_t, 24> crc24(const std::vector<uint8_t>& pdu_bits, uint32_t init = kCrcInit);

struct AdvPacket {
    int channel{37};
    PduType type{PduType::ADV_IND};
    std::string mac{"010203040506"};  /* 12 hex chars, natural (display) order */
    std::string adv_data{"02011A"};   /* AD structures, hex */
    int txadd{1};                     /* Tx address is random (upstream default) */
    int rxadd{0};
};

/* The info bits: preamble | access address | header | AdvA | AdvData, before
 * CRC and whitening. Empty on invalid MAC/AdvData hex. */
std::vector<uint8_t> build_info_bits(const AdvPacket& pkt);

/* The full transmit bit stream: preamble | access address (unwhitened) |
 * whitened(PDU | CRC). Empty on invalid input. */
std::vector<uint8_t> build_phy_bits(const AdvPacket& pkt);

/* Advertising-channel centre frequency for a BLE channel index (0..39). */
uint64_t channel_frequency(int channel);

/* --- View ----------------------------------------------------------------- */

class BleTxView : public ui::View {
   public:
    BleTxView();
    ~BleTxView() override;

    BleTxView(const BleTxView&) = delete;
    BleTxView& operator=(const BleTxView&) = delete;

    std::string title() const override { return "BLE TX"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void build_packet();
    void update_preview();
    void start_tx();
    void stop_tx();

    radio::TransmitterModel* transmitter_{nullptr};

    bool transmitting_{false};
    AdvPacket packet_{};
    std::vector<std::complex<float>> burst_{};  /* one modulated packet + gap */

    static constexpr double kSampleRate = 4'000'000.0;  /* proc_ble_tx rate */
    static constexpr float kSymbolRate = 1'000'000.0f;
    static constexpr float kDeviation = 250'000.0f;     /* MOD_IDX 0.5 */
    static constexpr float kGaussianBt = 0.5f;

    /* ~0.25 s of headroom at 4 Msps between the refill in on_frame_sync() and
     * the transmitter's pull. */
    dsp::RingBuffer<std::complex<float>> ring_{1u << 20};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{0, 24}, "Ch", ui::Color::light_grey()},
        {{0, 46}, "Type", ui::Color::light_grey()},
        {{0, 68}, "MAC", ui::Color::light_grey()},
        {{0, 90}, "Data", ui::Color::light_grey()},
        {{0, 112}, "Gain", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};

    ui::OptionsField options_channel_{
        {40, 24},
        6,
        {{"37 adv", 37}, {"38 adv", 38}, {"39 adv", 39}, {"0 data", 0}, {"11 dat", 11}}};

    ui::OptionsField options_type_{
        {40, 46},
        12,
        {{"ADV_IND", 0},
         {"ADV_NONCONN", 2},
         {"ADV_SCAN_IND", 6},
         {"SCAN_RSP", 4}}};

    ui::SymField field_mac_{{40, 68}, 12, ui::SymField::Type::Hex, true};
    ui::SymField field_data_{{40, 90}, 40, ui::SymField::Type::Hex, true};

    ui::NumberField field_gain_{{40, 112}, 3, {0, 89}, 1, ' '};

    ui::Checkbox check_loop_{{140, 108}, 4, "Loop"};

    ui::Button button_tx_{{160, 24, 72, 28}, "TX"};

    ui::Text text_status_{{0, 134, 240, 16}, "Idle"};
    ui::Console console_{{0, 152, 240, 96}};

    ui::Labels notes_{
        {{0, 252}, "TX only on TX press.", ui::Color::yellow()},
        {{0, 268}, "2.4 GHz: top of B200 range.", ui::Color::grey()},
        {{0, 284}, "Untested on air - no radio.", ui::Color::yellow()},
    };
};

}  // namespace ble_tx
}  // namespace app

#endif /*__MB200_UI_BLE_TX_H__*/
