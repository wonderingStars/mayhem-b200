/*
 * mayhem-b200 — Radiosonde protocol and decoder.
 *
 * Port of firmware/common/sonde_packet.* (frame parsing) and
 * firmware/baseband/proc_sonde.* (the signal chain). The view that displays
 * this lives in ui_sonde.hpp; the split matches upstream's, and it keeps the
 * decode testable without linking the UI or the radio.
 *
 * WHAT UPSTREAM ACTUALLY SUPPORTS
 * -------------------------------
 * Upstream Mayhem's radiosonde app decodes two families, both ~400 MHz 2FSK:
 *
 *   Vaisala RS41-SG   4800 bit/s NRZ, 320-byte frame, XOR-scrambled with a
 *                     64-byte pseudo-random mask, per-block CRC-16/CCITT-FALSE.
 *   Meteomodem        4800 bit/s bi-phase-mark (so 9600 chips/s on air),
 *                     M10 / M2K2 / M20, identified from the first two decoded
 *                     bytes.
 *
 * There is NO Graw DFM support anywhere in the upstream tree (grepped for
 * "Graw", "DFM-", "dfm06", "dfm09" across firmware/ — zero hits), and no
 * Reed-Solomon decoder either: upstream reads the RS41's per-block CRC-16 and
 * ignores the 48 bytes of Reed-Solomon parity at 0x008..0x037 entirely. This
 * port therefore implements RS41 + Meteomodem and does not invent a DFM bit
 * layout or an RS decoder that upstream does not specify.
 *
 * HOW IT RUNS ON THE HOST
 * -----------------------
 * There is no M4 and no message queue. `Decoder` below is the whole of
 * proc_sonde's signal path as a plain object that a view drives from
 * on_frame_sync():
 *
 *   wideband IQ -> NCO mix to the tuned channel -> decimating FIR to ~38.4 kHz
 *     -> quadrature FSK discriminator + Gardner clock recovery (two of them,
 *        4800 sym/s for RS41 and 9600 chip/s for Meteomodem, exactly as
 *        proc_sonde runs two ClockRecovery instances off one discriminator)
 *     -> PacketBuilder with upstream's 32-bit sync words and frame lengths
 *     -> sonde::Packet: descramble / bi-phase-M decode, CRC, field extraction.
 *
 * Three deliberate departures from upstream, each marked at the point of use:
 *
 *   1. proc_sonde builds its discriminator from an AIS matched filter
 *      (baseband::ais::square_taps_38k4_1t_p) whose taps were designed for a
 *      2400 Hz deviation at a +9600 Hz channel offset; the file's own comment
 *      block admits the taps are not the ones the sonde channel wants. The host
 *      mixes the channel to DC itself, so it uses dsp::FskDemod — a true
 *      quadrature discriminator plus the same Gardner loop — instead.
 *   2. Each bit is offered to the packet builders in both senses. The firmware
 *      knows which way round its discriminator sits because the LO placement is
 *      fixed; here the LO offset is whatever the ReceiverModel chose, so the
 *      polarity of the discriminator output is not known in advance. A 32-bit
 *      sync word with at most one bit of error makes the extra path free of
 *      false positives.
 *   3. Packet::get_temp_humid() takes its RS41 calibration store by pointer
 *      instead of using file-scope statics, and bounds-checks the subframe
 *      index. Upstream writes calibytes[calfr * 16 + i] with calfr taken
 *      straight off the air into a 51*16 array, which a corrupt frame overruns.
 *
 * Everything else — mask table, block offsets, CRC, ECEF->WGS84 conversion,
 * every field position, the M10/M20 sensor maths — is upstream's, unchanged.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2014 zilog80
 * Copyright (C) 2023 joyel24 (Meteomodem M20)
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_SONDE_PACKET_H__
#define __MB200_SONDE_PACKET_H__

#include "../dsp/demod.hpp"
#include "../dsp/demod_digital.hpp"
#include "../dsp/fir.hpp"
#include "../dsp/protocol.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace app {
namespace sonde {

/* --- Decoded telemetry ---------------------------------------------------- */

struct GPS_data {
    uint32_t alt{0};
    float lat{0};
    float lon{0};

    /* Upstream's validity test, verbatim: a position at the origin is what an
     * un-acquired receiver reports, so it counts as "no fix". */
    bool is_valid() const {
        if (lat >= -0.01f && lat <= 0.01f && lon >= -0.01f && lon <= 0.01f) return false;
        if (lat < -90.0f || lat > 90.0f) return false;
        if (lon < -180.0f || lon > 180.0f) return false;
        return true;
    }
};

struct temp_humid {
    float temp{0};
    float humid{0};
};

struct FormattedSymbols {
    std::string data;
    std::string errors;
};

/* The RS41 ships its sensor calibration a 16-byte subframe at a time, one per
 * frame, so temperature and humidity only become computable after the right
 * subframes have all been seen. Upstream keeps this in two file-scope arrays;
 * here it is an object the app owns, so tests get a clean one and a corrupt
 * subframe index cannot walk off the end. */
struct Rs41Calibration {
    static constexpr size_t subframe_count = 51;
    static constexpr size_t subframe_size = 16;

    uint8_t calibytes[subframe_count * subframe_size]{};
    uint8_t calfrchk[subframe_count]{};

    void reset();
    /* Stores one 16-byte subframe. Returns false (and stores nothing) if the
     * frame advertised a slot outside the table. */
    bool store(uint8_t slot, const uint8_t* bytes);
    bool have(uint8_t slot) const {
        return (slot < subframe_count) && (calfrchk[slot] != 0);
    }
};

/* --- Packet --------------------------------------------------------------- */

class Packet {
   public:
    using Timestamp = std::chrono::system_clock::time_point;

    enum class Type : uint32_t {
        Unknown = 0,
        Meteomodem_unknown = 1,
        Meteomodem_M10 = 2,
        Meteomodem_M2K2 = 3,
        Vaisala_RS41_SG = 4,
        Meteomodem_M20 = 5,
    };

    /* `cal` may be null; RS41 temperature/humidity then read as zero, which is
     * exactly what upstream reports before the calibration subframes arrive. */
    Packet(const dsp::Packet& packet, Type type, Rs41Calibration* cal = nullptr);

    /* Decoded symbol count (bi-phase-M symbols for Meteomodem, and for the RS41
     * whatever upstream's decoder_.symbols_count() would have said: bits / 2). */
    size_t length() const;
    Timestamp received_at() const;

    Type type() const { return type_; }
    std::string type_string() const;

    std::string serial_number() const;
    uint32_t battery_voltage() const; /* millivolts */
    GPS_data get_GPS_data() const;
    uint32_t frame() const;
    temp_humid get_temp_humid() const;
    float get_pressure() const; /* hPa, M20 only */

    FormattedSymbols symbols_formatted() const;

    bool crc_ok() const;

    /* --- Exposed for tests; these are the two pieces the RS41 decode rests on.
     *
     * vaisala_descramble(pos) returns payload byte `pos` after XOR with the
     * 64-byte mask. `pos` is relative to the start of the captured payload,
     * which begins 4 bytes into the on-air frame (the sync word is consumed by
     * the packet builder), hence the +4 on the mask index. */
    uint8_t vaisala_descramble(uint32_t pos) const;
    bool crc16rs41(uint32_t field_start) const;

    /* Raw payload bits as captured, and the bi-phase-M decoded bit stream. */
    const dsp::Packet& raw() const { return packet_; }
    const std::vector<uint8_t>& biphase_bits() const { return bi_m_bits_; }

    /* Upstream's vaisala_mask, from rs1729/RS rs41sg.c. */
    static constexpr uint8_t vaisala_mask[64] = {
        0x96, 0x83, 0x3E, 0x51, 0xB1, 0x49, 0x08, 0x98,
        0x32, 0x05, 0x59, 0x0E, 0xF9, 0x44, 0xC6, 0x26,
        0x21, 0x60, 0xC2, 0xEA, 0x79, 0x5D, 0x6D, 0xA1,
        0x54, 0x69, 0x47, 0x0C, 0xDC, 0xE8, 0x5C, 0xF1,
        0xF7, 0x76, 0x82, 0x7F, 0x07, 0x99, 0xA2, 0x2C,
        0x93, 0x7C, 0x30, 0x63, 0xF5, 0x10, 0x2E, 0x61,
        0xD0, 0xBC, 0xB4, 0xB6, 0x06, 0xAA, 0xF4, 0x23,
        0x78, 0x6E, 0x3B, 0xAE, 0xBF, 0x7B, 0x4C, 0xC1};

    /* Block offsets, in payload bytes. Upstream's #defines, which are the
     * rs41sg.c offsets minus the 4 sync bytes. */
    static constexpr uint32_t block_status = 0x35;  /* 40 data bytes */
    static constexpr uint32_t block_gpspos = 0x10E; /* 21 data bytes */
    static constexpr uint32_t block_meas = 0x61;    /* 42 data bytes */
    static constexpr uint32_t pos_FrameNb = 0x37;   /* 2 bytes  */
    static constexpr uint32_t pos_SondeID = 0x39;   /* 8 bytes  */
    static constexpr uint32_t pos_Voltage = 0x41;   /* 1 byte, volts * 10 */
    static constexpr uint32_t pos_CalData = 0x4E;   /* subframe index + 16 bytes */
    static constexpr uint32_t pos_temp = 0x63;      /* 12 x 3 bytes */
    static constexpr uint32_t pos_GPSecefX = 0x110; /* 3 x int32, little-endian, cm */

   private:
    /* FieldReader<BiphaseMDecoder, BitRemapNone>::read — `start_bit` becomes
     * the MSB of the result. Signed, because upstream relies on that for the
     * M10 latitude/longitude. */
    int32_t read_bi_m(size_t start_bit, size_t length) const;

    uint8_t getFwVerM20() const;
    bool crc_ok_M10() const;
    bool crc_ok_RS41() const;
    bool check_ok_M20() const;

    dsp::Packet packet_{};
    std::vector<uint8_t> bi_m_bits_{};
    std::vector<uint8_t> bi_m_errors_{};
    Rs41Calibration* cal_{nullptr};
    Type type_{Type::Unknown};
};

/* --- Decoder -------------------------------------------------------------- */

/* proc_sonde's signal chain, minus the M4. Feed it complex baseband; it calls
 * the packet handler once per accepted frame.
 *
 * NOTE ON THE SAMPLE TAP: the ideal input is a continuous, gap-free tap at the
 * channel rate — the samples that ReceiverModel's DSP thread already has just
 * after nco_.mix() and channel_filter_.process(). ReceiverModel does not expose
 * one; take_spectrum_samples() is a 4096-sample wideband snapshot that is
 * refreshed per DSP block and only read at UI frame rate, so a view driving
 * this decoder from it sees roughly a tenth of the stream in disjoint bursts.
 * A whole RS41 frame is 533 ms long, so it can never be assembled that way.
 * The decoder itself is rate-agnostic and correct for any contiguous stream —
 * see tests/test_sonde.cpp, which feeds it complete synthesised frames. */
class Decoder {
   public:
    using PacketHandler = std::function<void(const Packet&)>;

    /* Sync words are in on-air bit order (least-significant bit first within
     * each byte for the RS41), taken verbatim from proc_sonde.hpp. */

    /* Raw RS41 header bytes 0x10 0xB6 0xCA 0x11 — which descramble to
     * 0x86 0x35 0xF4 0x40, the first half of the RS41 frame signature. */
    static constexpr uint64_t rs41_sync = 0x086D5388ull;
    static constexpr size_t rs41_sync_bits = 32;
    static constexpr size_t rs41_frame_bits = 320 * 8;
    static constexpr float rs41_baud = 4800.0f;
    static constexpr float rs41_deviation = 2400.0f;

    /* Meteomodem bi-phase-mark preamble, 32 chips. */
    static constexpr uint64_t meteomodem_sync = 0b00110011001100110101100110110011ull;
    static constexpr size_t meteomodem_sync_bits = 32;
    static constexpr size_t meteomodem_frame_chips = 88 * 2 * 8;
    static constexpr float meteomodem_chip_rate = 9600.0f;
    static constexpr float meteomodem_deviation = 4800.0f;

    /* Upstream allows one bit of error in both sync words. */
    static constexpr size_t sync_tolerance = 1;

    /* proc_sonde's post-decimation rate. */
    static constexpr double channel_rate_target = 38400.0;

    Decoder();

    void set_packet_handler(PacketHandler handler) { handler_ = std::move(handler); }
    void set_calibration(Rs41Calibration* cal) { cal_ = cal; }

    /* Wideband input at `input_rate_hz`, with the wanted channel `offset_hz`
     * above the centre of the captured band. Installs the mixer and the
     * decimating channel filter. */
    void configure(double input_rate_hz, double offset_hz);

    /* Input is already at baseband and at the channel rate — no mixer, no
     * channel filter. */
    void configure_channel(double channel_rate_hz);

    void reset();

    void feed(const dsp::cfloat* in, size_t count);
    void feed_channel(const dsp::cfloat* in, size_t count);

    /* Hard bits straight into the packet builders, bypassing the demodulators.
     * Both senses are still tried, exactly as feed_channel() does. */
    void feed_bits_rs41(const uint8_t* bits, size_t count);
    void feed_bits_meteomodem(const uint8_t* chips, size_t count);

    double input_rate() const { return input_rate_; }
    double channel_rate() const { return channel_rate_; }
    double offset() const { return offset_; }
    size_t decimation() const { return decimation_; }
    bool front_end_enabled() const { return front_end_; }

    uint64_t samples_fed() const { return samples_fed_; }
    uint64_t bits_rs41() const { return bits_rs41_; }
    uint64_t bits_meteomodem() const { return bits_meteomodem_; }
    uint64_t packets_rs41() const { return packets_rs41_; }
    uint64_t packets_meteomodem() const { return packets_meteomodem_; }
    uint64_t packets_total() const { return packets_rs41_ + packets_meteomodem_; }

   private:
    void build_packet_builders();
    void configure_demods();
    void emit(const dsp::Packet& packet, Packet::Type type, bool is_rs41);

    dsp::Nco nco_{};
    dsp::FirDecimateC channel_filter_{};
    bool front_end_{false};
    size_t decimation_{1};
    double input_rate_{0.0};
    double channel_rate_{channel_rate_target};
    double offset_{0.0};

    dsp::FskDemod demod_rs41_{};
    dsp::FskDemod demod_meteomodem_{};

    /* Two builders per protocol, one per bit sense — see departure 2 in the
     * file header. */
    std::unique_ptr<dsp::FixedLengthPacketBuilder> rs41_[2]{};
    std::unique_ptr<dsp::FixedLengthPacketBuilder> meteomodem_[2]{};

    std::vector<dsp::cfloat> mix_scratch_{};
    std::vector<dsp::cfloat> channel_scratch_{};
    std::vector<uint8_t> bits_scratch_{};

    PacketHandler handler_{};
    Rs41Calibration* cal_{nullptr};

    uint64_t samples_fed_{0};
    uint64_t bits_rs41_{0};
    uint64_t bits_meteomodem_{0};
    uint64_t packets_rs41_{0};
    uint64_t packets_meteomodem_{0};
};

/* Formats a packet timestamp as "YYYY-MM-DD HH:MM:SS" in local time. Upstream
 * uses to_string_datetime(packet.received_at(), YMDHMS) off the RTC. */
std::string format_timestamp(Packet::Timestamp t);

/* "geo:<lat>,<lon>" with five decimal places (~1 m), the URI upstream renders
 * into a QR code. There is no QR renderer on the host, so the app shows the
 * URI itself. */
std::string geo_uri(float lat, float lon);

}  // namespace sonde
}  // namespace app

#endif /*__MB200_SONDE_PACKET_H__*/
