/*
 * mayhem-b200 — Bluetooth Low Energy advertising receiver.
 *
 * Ported from firmware/application/apps/ble_rx_app.* and the baseband receive
 * processor firmware/baseband/proc_btlerx.* . Upstream decimates the 4 Msps
 * channel to one sample per symbol, takes the phase difference between adjacent
 * samples as an FM discriminator, slides a 32-bit shift register until it
 * matches the advertising access address 0x8E89BED6, then reads the PDU header,
 * dewhitens with the per-channel scrambling sequence, reads the payload, and
 * accepts the frame only when the CRC-24 checks.
 *
 * The decoder here keeps that pipeline but works at an arbitrary integer number
 * of samples per symbol (upstream is hard-wired to one): a symbol's bit is the
 * sign of the summed phase difference across its samples, and the access-address
 * search tries every symbol phase so a burst need not arrive symbol-aligned.
 * The CRC uses the same canonical table + crc_init_reorder as proc_btlerx; the
 * dewhitening is the transmit LFSR run in reverse (XOR is its own inverse).
 *
 * FREQUENCY: 2402 / 2426 / 2480 MHz advertising channels — the top of a B200's
 * 6 GHz range; the range is read from radio->caps() and shown on screen.
 *
 * SAMPLE TAP: like the nRF receiver, this uses ReceiverModel's block-based
 * wideband tap, so a burst that straddles a block gap is missed. The decoder
 * is proved on synthesised GFSK in tests/test_ble_rx.cpp; sustained over-the-air
 * capture needs a lossless channel-rate tap that does not exist yet.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2020 Shao
 * Copyright (C) 2023 TJ Baginski (original app / baseband)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_BLE_RX_H__
#define __MB200_UI_BLE_RX_H__

#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_recent_entries.hpp"
#include "ui_widget.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace radio {
class ReceiverModel;
}

namespace app {
namespace ble_rx {

inline constexpr uint32_t kAccessAddress = 0x8E89BED6;
inline constexpr uint32_t kCrcInit = 0x555555;
inline constexpr size_t kHeaderBytes = 2;
inline constexpr size_t kCrcBytes = 3;
inline constexpr size_t kMaxPayloadBytes = 37;

/* Table-driven BLE CRC-24 (proc_btlerx crc_update / crc24_byte). `init` is the
 * reordered preset from crc_init_reorder(). */
uint32_t crc24(const uint8_t* data, size_t length, uint32_t init);

/* proc_btlerx crc_init_reorder(): byte-swap then bit-reverse the 24-bit preset,
 * so 0x555555 becomes the value the table engine expects. */
uint32_t crc_init_reorder(uint32_t crc_init);

/* BLE data-channel whitening for `channel`, applied bit-for-bit; its own
 * inverse, so this both whitens and dewhitens. */
std::vector<uint8_t> whiten(const std::vector<uint8_t>& bits, int channel);

struct Packet {
    uint8_t type{0};        /* ADV_PDU_TYPE (header byte 0 low nibble) */
    uint8_t payload_len{0}; /* PDU payload length, includes the 6 AdvA bytes */
    std::array<uint8_t, 6> mac{};      /* display order (MSB octet first) */
    std::vector<uint8_t> data{};       /* AdvData (payload_len - 6 bytes) */
    uint32_t crc{0};                   /* received CRC */
    uint32_t computed_crc{0};
    int channel{37};

    uint64_t mac_key() const;
    std::string mac_string() const;
    std::string data_string() const;
    const char* type_string() const;
};

/* Sliding access-address correlator + PDU parser over complex baseband at
 * `samples_per_symbol` samples/symbol. */
class Decoder {
   public:
    using PacketHandler = std::function<void(const Packet&)>;

    void configure(size_t samples_per_symbol, int channel);
    void set_channel(int channel) { channel_ = channel; }
    void set_on_packet(PacketHandler h) { on_packet_ = std::move(h); }
    void reset() { packets_decoded_ = 0; }

    void process(const dsp::cfloat* in, size_t count);

    size_t samples_per_symbol() const { return sps_; }
    size_t packets_decoded() const { return packets_decoded_; }

   private:
    /* Summed phase difference across the `sps_` samples of the symbol whose
     * first sample is `pos` in `diffs`. */
    float symbol_sum(const std::vector<float>& diffs, size_t pos) const;
    bool try_parse(const std::vector<float>& diffs, size_t header_start);

    size_t sps_{1};
    int channel_{37};
    size_t packets_decoded_{0};
    PacketHandler on_packet_{};
};

/* --- View ----------------------------------------------------------------- */

struct BleRecentEntry {
    using Key = uint64_t;
    static constexpr Key invalid_key = 0xFFFFFFFFFFFFFFFFULL;

    BleRecentEntry() = default;
    explicit BleRecentEntry(Key k) : mac{k} {}

    Key mac{invalid_key};
    uint32_t count{0};
    uint8_t type{0};
    uint8_t payload_len{0};
    std::string data_hex{};

    Key key() const { return mac; }
};

using BleRecentEntries = ui::RecentEntries<BleRecentEntry>;

class BleRxView : public ui::View {
   public:
    BleRxView();
    ~BleRxView() override;

    BleRxView(const BleRxView&) = delete;
    BleRxView& operator=(const BleRxView&) = delete;

    std::string title() const override { return "BLE RX"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void rebuild_front_end();
    void on_packet(const Packet& packet);

    radio::ReceiverModel& receiver_;

    bool running_{false};
    int channel_{37};
    uint32_t packets_{0};

    double configured_input_rate_{0.0};
    size_t decimation_{1};
    size_t samples_per_symbol_{2};
    dsp::FirDecimateC channel_filter_{};

    std::vector<dsp::cfloat> samples_{};
    std::vector<dsp::cfloat> chan_out_{};

    Decoder decoder_{};
    BleRecentEntries entries_{};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{0, 22}, "Ch", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};

    ui::OptionsField options_channel_{
        {40, 22},
        6,
        {{"37 adv", 37}, {"38 adv", 38}, {"39 adv", 39}}};

    ui::Button button_startstop_{{192, 20, 44, 20}, "Start"};

    ui::Text text_status_{{0, 42, 240, 16}, ""};
    ui::Text text_range_{{0, 60, 240, 16}, ""};

    ui::RecentEntriesColumns columns_{{{"MAC", 13}, {"T", 2}, {"Data", 0}}};
    ui::RecentEntriesTable<BleRecentEntries> table_{entries_, columns_};

    ui::Labels notes_{
        {{0, 268}, "Tap: wideband spectrum block", ui::Color::grey()},
        {{0, 284}, "Untested on air - no radio.", ui::Color::yellow()},
    };
};

}  // namespace ble_rx
}  // namespace app

#endif /*__MB200_UI_BLE_RX_H__*/
