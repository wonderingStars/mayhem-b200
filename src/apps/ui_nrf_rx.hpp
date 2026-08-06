/*
 * mayhem-b200 — Nordic nRF24L01+ (Enhanced ShockBurst) receiver.
 *
 * Ported from firmware/application/external/nrf_rx/ and
 * firmware/baseband/proc_nrfrx.cpp. Upstream FM-demodulates the 2.4 GHz GFSK
 * channel on the M4 and runs a sliding-window correlator over the resulting
 * discriminator samples: it looks for the alternating preamble, slices every
 * bit against the mean of the first eight bit periods, and accepts the frame
 * only when the received CRC matches.
 *
 * On-air frame, in the bit order the decoder walks (offsets are bit indices
 * from the start of the detection window, each bit `samples_per_bit` samples):
 *
 *      0 ..   7   preamble        (0xAA when the address MSB is 1, else 0x55)
 *      8 ..  47   address         5 bytes, first byte on air is the MSB
 *     48 ..  56   packet control  6-bit payload length | 2-bit PID | 1-bit NO_ACK
 *     57 ..       payload         `length` bytes
 *     ..  +16     CRC-16          poly 0x1021, init 0xFFFF, over bits 8..56
 *                                 plus the payload
 *
 * Upstream computes that CRC byte-wise over the 49 header bits left-padded to
 * seven bytes, using an initial remainder of 0x3C18. That is not a different
 * CRC: 0x3C18 is exactly the state that becomes 0xFFFF after seven zero bits,
 * so the byte-wise form is bit-for-bit the real ESB CRC. Both forms are
 * cross-checked in tests/test_nrf_rx.cpp.
 *
 * Two documented departures from proc_nrfrx.cpp, both in its ring-buffer
 * bookkeeping rather than in the protocol:
 *
 *   1. Upstream reads bit k at `rb_buf[(rb_head + k * g_srate) % RB_SIZE]`,
 *      where rb_head holds the *newest* sample. For k >= 1 that walks forward
 *      in time from the oldest sample, which is the intent; for k == 0 it
 *      lands on the newest sample instead — a whole ring away from the rest of
 *      the window. Bit 0 is one of the eight preamble bits the transition
 *      count is taken over, so upstream's first preamble bit is read from
 *      unrelated data. Here every bit, including bit 0, is read from the
 *      window.
 *   2. Upstream's ring is a fixed 1000 samples. A maximum-length frame
 *      (8 + 40 + 9 + 256 + 16 = 329 bits) needs 1316 samples at 4 samples per
 *      bit, so long frames wrap and decode garbage — and its 50-byte
 *      `packet_packed` scratch overflows for any length above 43. The ring
 *      here is sized from the maximum frame, and a length above 32 (the
 *      nRF24L01+ maximum) is rejected rather than partially processed.
 *
 * FREQUENCY: nRF24 sits in 2.400-2.525 GHz, 1 MHz channel spacing. That is
 * inside the B200's tuning range rather than at the edge of it — the range is
 * read from radio->caps() at run time and shown on screen, so the app never
 * asserts a limit it has not verified.
 *
 * SAMPLE TAP — see the note in ui_nrf_rx.cpp: ReceiverModel offers only a
 * block-based wideband tap, so bursts can fall in the gaps between blocks.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2020 Shao (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_NRF_RX_H__
#define __MB200_UI_NRF_RX_H__

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
namespace nrf {

/* Enhanced ShockBurst limits. */
inline constexpr size_t kAddressBytes = 5;
inline constexpr size_t kMaxPayloadBytes = 32;
inline constexpr size_t kPreambleBits = 8;
inline constexpr size_t kAddressBits = kAddressBytes * 8;
inline constexpr size_t kPcfBits = 9;
inline constexpr size_t kCrcBits = 16;
inline constexpr size_t kMaxFrameBits =
    kPreambleBits + kAddressBits + kPcfBits + kMaxPayloadBytes * 8 + kCrcBits;  /* 329 */

/* CRC-16/CCITT (poly 0x1021) over packed bytes, MSB-first, no reflection and
 * no final XOR. `init` defaults to upstream's 0x3C18, which is the register
 * state 0xFFFF reaches after the seven zero bits that pad the 49-bit header up
 * to a whole number of bytes. */
uint16_t crc16(const uint8_t* data, size_t length, uint16_t init = 0x3C18);

/* The same CRC computed the way the datasheet defines it: init 0xFFFF, over
 * the 40 address bits, then the 9 packet-control bits, then the payload. */
uint16_t esb_crc(uint64_t address, uint16_t pcf9, const uint8_t* payload, size_t length);

struct Packet {
    uint64_t address{0};          /* 40 bits, first on-air byte is the MSB */
    uint16_t pcf{0};              /* the raw 9-bit packet control field      */
    uint8_t payload_length{0};    /* PCF bits 8..3                          */
    uint8_t pid{0};               /* PCF bits 2..1                          */
    bool no_ack{false};           /* PCF bit 0                              */
    uint16_t crc{0};              /* as received                            */
    uint16_t computed_crc{0};
    std::array<uint8_t, kMaxPayloadBytes> payload{};

    std::string address_string() const;
    std::string payload_string() const;
};

/* Sliding-window Enhanced ShockBurst decoder over FM-discriminator samples.
 *
 * `samples_per_bit` is upstream's g_srate: the demodulated sample rate divided
 * by the on-air bit rate (4 at 1 Msps / 250 kbps, which is what proc_nrfrx
 * runs). Samples are the host's normalised discriminator output, nominally
 * +/-1.0 at full deviation. */
class Decoder {
   public:
    using PacketHandler = std::function<void(const Packet&)>;

    /* `dc_limit` is upstream's `abs(g_threshold) < 15500` sanity check on the
     * window mean, rescaled from int16 full scale to the host's +/-1.0
     * discriminator output. */
    void configure(size_t samples_per_bit, float dc_limit = 15500.0f / 32768.0f);
    void reset();

    void set_on_packet(PacketHandler handler) { on_packet_ = std::move(handler); }

    void process(const float* samples, size_t count);
    void process_sample(float sample);

    size_t samples_per_bit() const { return samples_per_bit_; }
    size_t ring_size() const { return ring_.size(); }
    size_t packets_decoded() const { return packets_decoded_; }
    /* Window mean from the most recent decode attempt; useful in tests. */
    float last_threshold() const { return last_threshold_; }

   private:
    /* Sample of bit `k` of the candidate window. */
    float bit_sample(size_t k) const;
    /* Sample `offset` positions after the window start. */
    float window_sample(size_t offset) const;
    void attempt_decode();

    std::vector<float> ring_{};
    size_t head_{0};          /* index of the newest sample */
    size_t filled_{0};
    size_t samples_per_bit_{4};
    float dc_limit_{15500.0f / 32768.0f};
    int skip_samples_{0};
    float last_threshold_{0.0f};
    size_t packets_decoded_{0};
    PacketHandler on_packet_{};
};

}  // namespace nrf

/* --- View ----------------------------------------------------------------- */

struct NrfRecentEntry {
    using Key = uint64_t;
    static constexpr Key invalid_key = 0xFFFFFFFFFFFFFFFFULL;

    NrfRecentEntry() = default;
    explicit NrfRecentEntry(Key k) : address{k} {}

    Key address{invalid_key};
    uint32_t count{0};
    uint8_t payload_length{0};
    uint8_t pid{0};
    std::string payload_hex{};

    Key key() const { return address; }
};

using NrfRecentEntries = ui::RecentEntries<NrfRecentEntry>;

class NrfRxView : public ui::View {
   public:
    NrfRxView();
    ~NrfRxView() override;

    NrfRxView(const NrfRxView&) = delete;
    NrfRxView& operator=(const NrfRxView&) = delete;

    std::string title() const override { return "NRF"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

    /* Read-only view of the decoded-packet list, for the web portal's panel
     * provider (src/remote/provider_nrf_rx.cpp). Same idiom as
     * AprsTableView::entries(); the app itself never calls it and nothing can
     * mutate the list through it. */
    const NrfRecentEntries& entries() const { return entries_; }

   private:
    void rebuild_front_end();
    void on_packet(const nrf::Packet& packet);

    radio::ReceiverModel& receiver_;

    bool running_{false};
    uint32_t bitrate_kbps_{250};
    uint32_t packets_{0};

    double configured_input_rate_{0.0};
    size_t decimation_{1};
    float demod_rate_{1'000'000.0f};
    dsp::FirDecimateC channel_filter_{};
    dsp::FmDemod fm_{};

    std::vector<dsp::cfloat> samples_{};
    std::vector<dsp::cfloat> channel_{};
    std::vector<float> audio_{};

    nrf::Decoder decoder_{};
    NrfRecentEntries entries_{};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
        {{0, 22}, "Rate", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};
    ui::FrequencyStepView step_view_{{132, 2}, field_frequency_};

    ui::OptionsField options_bitrate_{
        {40, 22},
        8,
        {{"250 kbps", 250}, {"1 Mbps  ", 1000}, {"2 Mbps  ", 2000}}};

    ui::Button button_startstop_{{192, 20, 44, 20}, "Start"};

    ui::Text text_status_{{0, 42, 240, 16}, ""};
    ui::Text text_range_{{0, 60, 240, 16}, ""};

    ui::RecentEntriesColumns columns_{{{"Address", 12}, {"Len", 4}, {"Payload", 0}}};
    ui::RecentEntriesTable<NrfRecentEntries> table_{entries_, columns_};

    ui::Labels notes_{
        {{0, 252}, "Tap: wideband spectrum block", ui::Color::grey()},
        {{0, 268}, "(no channel tap; see .cpp).", ui::Color::grey()},
        {{0, 288}, "Untested on air - no radio.", ui::Color::yellow()},
    };
};

}  // namespace app

#endif /*__MB200_UI_NRF_RX_H__*/
