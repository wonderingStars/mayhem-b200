/*
 * mayhem-b200 — AIS (Automatic Identification System) receiver.
 *
 * Ported from the PortaPack firmware:
 *
 *   application/apps/ais_app.*  -> the recent-entries list, the per-ship detail
 *                                  page, the 87B/88B channel selector and the
 *                                  ais::format:: presentation helpers
 *   common/ais_packet.*         -> ais::Packet, the lat/lon field types, the
 *                                  per-message-id length table and crc_ok()
 *   common/ais_baseband.hpp     -> the 38.4 kHz / 2400 Hz matched-filter taps
 *                                  (reproduced exactly by
 *                                  dsp::design_matched_filter_taps(2400, 38400, 4))
 *   baseband/proc_ais.*         -> the whole receive chain: decimate to 38.4 kHz,
 *                                  matched filter, Gardner clock recovery at
 *                                  9600 baud, NRZI decode, HDLC packet builder
 *
 * The signal, per ITU-R M.1371 and as implemented upstream:
 *
 *   161.975 MHz (channel 87B) or 162.025 MHz (88B), 9600 bit/s GMSK with a
 *   +/-2400 Hz deviation (modulation index 0.5), HDLC framing. A burst is a
 *   24-bit alternating training sequence, the 0x7E start flag, the payload,
 *   a 16-bit FCS, the 0x7E end flag. The payload and FCS are bit-stuffed (a 0
 *   is inserted after five consecutive 1s) and the whole burst is NRZI encoded
 *   (a data 1 is "no transition", a data 0 is "transition"). Each octet of the
 *   payload goes out least-significant bit first, which is why the field reader
 *   below remaps bit index i to i^7 before reading a message field.
 *
 * The FCS is CRC-16/X-25 (poly 0x1021 reflected, init 0xFFFF, xorout 0xFFFF),
 * transmitted low byte first with each byte LSB-first — the same convention as
 * the payload octets. Upstream checks it by feeding the *stream* bytes (which
 * are the payload octets bit-reversed) into a non-reflected CRC-16 and
 * comparing against the FCS read in stream order; that is algebraically the
 * same test and this port keeps both spellings, with a test asserting they
 * agree.
 *
 * HOST DEVIATIONS, all deliberate:
 *
 *  1. There is no M4 and no baseband message queue. AISAppView::on_frame_sync()
 *     pulls samples from the receiver and runs the chain inline. See the note on
 *     ais::Decoder about which receiver tap this really wants.
 *  2. ais::Packet::data_length() is guarded against underflow. Upstream computes
 *     length() - 7 - 16 on a size_t and happily reads a 4-billion-bit packet if
 *     a runt frame ever reaches it.
 *  3. The country column needs the firmware's mids.db, which this project does
 *     not ship, so the detail page omits it rather than showing a permanent
 *     "No mids.db file".
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc. (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_AIS_APP_H__
#define __MB200_AIS_APP_H__

#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "../dsp/protocol.hpp"
#include "ui.hpp"
#include "ui_geomap.hpp"
#include "ui_recent_entries.hpp"
#include "ui_widget.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ais {

/* --- Field types (common/ais_packet.hpp) ---------------------------------- */

using RateOfTurn = int8_t;
using SpeedOverGround = uint16_t;
using CourseOverGround = uint16_t;
using TrueHeading = uint16_t;
using MMSI = uint32_t;

struct DateTime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

/* A signed position field of FieldSize bits, in units of 1/10000 minute.
 * DegMax bounds the valid range; NAValue is the "not available" encoding
 * (91 degrees for latitude, 181 for longitude). */
template <size_t FieldSize, int32_t DegMax, uint32_t NAValue>
struct LatLonBase {
    constexpr LatLonBase()
        : raw_{raw_not_available} {}

    constexpr LatLonBase(const int32_t raw)
        : raw_{raw} {}

    constexpr LatLonBase(const LatLonBase& other) = default;
    LatLonBase& operator=(const LatLonBase&) = default;

    /* Sign-extends the FieldSize-bit field into a full int32_t. The low
     * sign_extend_shift bits of the shifted value are zero, so upstream's
     * divide is exactly an arithmetic shift; the cast through uint32_t avoids
     * shifting a 1 into the sign bit of a signed type. */
    int32_t normalized() const {
        const int32_t shifted =
            static_cast<int32_t>(static_cast<uint32_t>(raw_) << sign_extend_shift);
        return shifted / (int32_t{1} << sign_extend_shift);
    }

    int32_t raw() const { return raw_; }

    bool is_not_available() const { return raw() == raw_not_available; }

    bool is_valid() const {
        return (normalized() >= raw_valid_min) && (normalized() <= raw_valid_max);
    }

   private:
    int32_t raw_;

    static constexpr size_t sign_extend_shift = 32 - FieldSize;

    static constexpr int32_t raw_not_available = static_cast<int32_t>(NAValue);
    static constexpr int32_t raw_valid_min = -DegMax * 60 * 10000;
    static constexpr int32_t raw_valid_max = DegMax * 60 * 10000;
};

using Latitude = LatLonBase<27, 90, 0x3412140>;
using Longitude = LatLonBase<28, 180, 0x6791AC0>;

/* --- HDLC / framing primitives, exposed so they can be tested alone -------- */

/* The FCS as the standard names it: CRC-16/X-25 (a.k.a. CRC-16/IBM-SDLC), the
 * same polynomial RFC 1662 specifies for the PPP FCS-16. Check value for the
 * nine bytes "123456789" is 0x906E. */
uint16_t fcs(const uint8_t* data, size_t length);
uint16_t fcs(const std::vector<uint8_t>& data);

/* NRZI, matching baseband/symbol_coding.hpp: a data 1 leaves the line level
 * alone, a data 0 toggles it. Both sides start from a 0 level. */
std::vector<uint8_t> nrzi_decode(const std::vector<uint8_t>& symbols);
std::vector<uint8_t> nrzi_encode(const std::vector<uint8_t>& bits);

/* HDLC transparency. stuff() inserts a 0 after every five consecutive 1s;
 * destuff() removes it again. Neither is applied to the flags. */
std::vector<uint8_t> hdlc_stuff(const std::vector<uint8_t>& bits);
std::vector<uint8_t> hdlc_destuff(const std::vector<uint8_t>& bits);

/* AIS six-bit ASCII (ITU-R M.1371 Table 47), upstream's char_to_ascii:
 * 0..31 map to '@'..'_', 32..63 map to ' '..'?'. */
constexpr char six_bit_to_ascii(const uint8_t c) {
    return static_cast<char>(((c ^ 32) + 32) & 0xFF);
}

/* True if `data_bits` is a legal payload length for `message_id`
 * (common/ais_packet.cpp packet_length_range). */
bool length_valid_for(uint32_t message_id, size_t data_bits);

/* --- Packet --------------------------------------------------------------- */

/* One HDLC frame exactly as the packet builder hands it over: the destuffed
 * payload, the FCS, and the first seven bits of the closing flag (the eighth is
 * swallowed by the unstuffer, which is why upstream subtracts 7). */
class Packet {
   public:
    using Timestamp = std::chrono::system_clock::time_point;

    Packet() = default;
    explicit Packet(const dsp::Packet& packet)
        : packet_{packet} {}

    /* Builds a packet from a raw bit vector (one uint8_t of 0 or 1 per bit),
     * for tests and for replaying a logged frame. */
    static Packet from_bits(const std::vector<uint8_t>& bits);

    /* Frame bits held, including the FCS and the seven flag bits. */
    size_t length() const { return packet_.size(); }

    /* Payload bits only. Zero when the frame is too short to hold an FCS. */
    size_t data_length() const;
    size_t data_and_fcs_length() const;

    bool length_valid() const;
    bool crc_ok() const;
    bool is_valid() const { return length_valid() && crc_ok(); }

    uint32_t message_id() const { return read(0, 6); }
    MMSI user_id() const { return read(8, 30); }
    MMSI source_id() const { return read(8, 30); }

    /* An ITU-R M.1371 field: `start_bit` is the MSB of the result, in the
     * standard MSB-first numbering over the payload octets. */
    uint32_t read(size_t start_bit, size_t length_bits) const;

    /* The same bits in transmission order, with no per-octet reversal. Only the
     * FCS check needs this. */
    uint32_t read_stream(size_t start_bit, size_t length_bits) const;

    std::string text(size_t start_bit, size_t character_count) const;
    DateTime datetime(size_t start_bit) const;
    Latitude latitude(size_t start_bit) const;
    Longitude longitude(size_t start_bit) const;

    Timestamp timestamp() const { return packet_.timestamp(); }
    void set_timestamp(const Timestamp& t) { packet_.set_timestamp(t); }
    /* Local wall clock, "YYYY-MM-DD HH:MM:SS". */
    std::string received_at() const;

    /* The AISLogger line format: one lowercase hex nibble per four bits. */
    std::string to_hex_nibbles() const;

    const dsp::Packet& raw() const { return packet_; }

   private:
    dsp::Packet packet_{};

    static constexpr size_t fcs_length = 16;
};

/* --- Presentation (ais::format:: upstream) -------------------------------- */

namespace format {

std::string latlon_abs_normalized(int32_t normalized, const char suffixes[2]);
std::string latlon(const Latitude& latitude, const Longitude& longitude);
float latlon_float(int32_t normalized);
std::string mmsi(const MMSI& mmsi);
/* Strips the '@' padding AIS uses for unused text characters. */
std::string text(const std::string& text);
std::string navigational_status(unsigned int value);
std::string rate_of_turn(RateOfTurn value);
std::string speed_over_ground(SpeedOverGround value);
std::string course_over_ground(CourseOverGround value);
std::string true_heading(TrueHeading value);

}  // namespace format

/* --- Frame construction ---------------------------------------------------
 *
 * The transmit side of the framing, which exists so the decoder can be tested
 * against a signal whose contents are known. There is no AIS transmitter app:
 * transmitting AIS is illegal for anyone but a licensed station, and upstream
 * has no such app either. */

/* Payload octets -> the destuffed bit vector the packet builder would produce
 * (payload LSB-first per octet, then the FCS, then seven flag bits). */
std::vector<uint8_t> build_packet_bits(const std::vector<uint8_t>& payload);

/* Payload octets -> the complete on-air bit stream after NRZI encoding:
 * training, start flag, stuffed payload and FCS, end flag. */
std::vector<uint8_t> build_air_bits(const std::vector<uint8_t>& payload,
                                    size_t training_bits = 24);

/* --- Decoder --------------------------------------------------------------
 *
 * proc_ais.cpp's chain, minus the two decimation stages, which on the host are
 * the app's job because the capture rate is not fixed at 2.4576 Msps.
 *
 *   complex baseband at channel_rate, AIS carrier at DC
 *     -> matched filter (rect window x a 2400 Hz sinusoid, 1 symbol long,
 *        decimating to 2 samples/symbol; the output is an FSK discriminator)
 *     -> Gardner clock recovery to 9600 baud
 *     -> slice at zero
 *     -> NRZI decode
 *     -> HDLC packet builder: preamble 0101010101111110 (1 bit of slack),
 *        unstuff on 111110, end on 01111110
 *     -> ais::Packet -> length check -> FCS check -> handler
 *
 * IDEAL TAP: this wants a continuous, gap-free stream of the tuned channel —
 * a ReceiverModel sink delivering every post-channel-filter sample, or at least
 * a lossless ring the app can drain. What exists today is
 * take_spectrum_samples(), a periodically-overwritten 4096-sample snapshot of
 * the wideband input, so at any capture rate above ~250 kHz there are gaps
 * between frames and a 26.7 ms AIS burst can straddle one. The chain below is
 * correct when fed samples; the tap is what limits it, and the app says so on
 * screen. */
class Decoder {
   public:
    using PacketHandler = std::function<void(const Packet&)>;

    static constexpr float symbol_rate = 9600.0f;
    static constexpr float deviation_hz = 2400.0f;

    Decoder();

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;
    Decoder(Decoder&&) = delete;
    Decoder& operator=(Decoder&&) = delete;

    /* `channel_rate_hz` is the rate of the samples handed to feed(). Must be a
     * multiple of 19200 for the matched filter to land on 2 samples/symbol;
     * anything else is rounded to the nearest integer decimation. */
    void configure(float channel_rate_hz);
    void set_packet_handler(PacketHandler handler) { packet_handler_ = std::move(handler); }
    void reset();

    void feed(const dsp::cfloat* in, size_t count);

    float channel_rate() const { return channel_rate_; }
    size_t frames_seen() const { return frames_seen_; }
    size_t packets_valid() const { return packets_valid_; }
    size_t crc_failures() const { return crc_failures_; }
    size_t length_failures() const { return length_failures_; }

   private:
    void consume_symbol(float raw_symbol);
    void on_frame(const dsp::Packet& packet);

    dsp::MatchedFilter mf_{};
    dsp::ClockRecovery<dsp::FixedErrorFilter> clock_recovery_{};
    dsp::NrziDecoder nrzi_{};
    dsp::PacketBuilder<dsp::BitPattern, dsp::BitPattern, dsp::BitPattern> builder_{};

    PacketHandler packet_handler_{};
    float channel_rate_{0.0f};
    size_t frames_seen_{0};
    size_t packets_valid_{0};
    size_t crc_failures_{0};
    size_t length_failures_{0};
};

}  // namespace ais

namespace app {

/* --- Results table -------------------------------------------------------- */

struct AISPosition {
    std::string timestamp{};
    ais::Latitude latitude{};
    ais::Longitude longitude{};
    ais::RateOfTurn rate_of_turn{-128};
    ais::SpeedOverGround speed_over_ground{1023};
    ais::CourseOverGround course_over_ground{3600};
    ais::TrueHeading true_heading{511};
};

struct AISRecentEntry {
    using Key = ais::MMSI;

    static constexpr Key invalid_key = 0xffffffff;

    ais::MMSI mmsi{0};
    std::string name{};
    std::string call_sign{};
    std::string destination{};
    AISPosition last_position{};
    size_t received_count{0};
    int8_t navigational_status{-1};

    AISRecentEntry() = default;
    explicit AISRecentEntry(const ais::MMSI& mmsi_)
        : mmsi{mmsi_} {}

    Key key() const { return mmsi; }

    /* Applies one packet's fields. Message ids 1/2/3 (position report),
     * 4 (base station), 5 (static and voyage data), 18 (class B position),
     * 21 (aid to navigation) and 24 (class B static) — the same set upstream
     * handles, at the same bit offsets. */
    void update(const ais::Packet& packet);
};

using AISRecentEntries = ui::RecentEntries<AISRecentEntry>;

/* Appends one line per received frame to <data>/LOGS/AIS.TXT, in upstream's
 * format: the timestamp, then the frame as lowercase hex nibbles. */
class AISLogger {
   public:
    bool open();
    void close();
    bool is_open() const { return file_.is_open(); }
    const std::string& path() const { return path_; }
    const std::string& error() const { return error_; }

    void on_packet(const ais::Packet& packet);

   private:
    std::ofstream file_{};
    std::string path_{};
    std::string error_{};
};

/* --- Detail page ---------------------------------------------------------- */

class AISRecentEntryDetailView : public ui::View {
   public:
    std::function<void(void)> on_close{};

    AISRecentEntryDetailView();

    AISRecentEntryDetailView(const AISRecentEntryDetailView&) = delete;
    AISRecentEntryDetailView& operator=(const AISRecentEntryDetailView&) = delete;

    void set_entry(const AISRecentEntry& new_entry);
    const AISRecentEntry& entry() const { return entry_; }

    void update_position();
    void update_map_markers(const AISRecentEntries& entries);
    bool add_map_marker(const AISRecentEntry& entry);

    void focus() override;
    void paint(ui::Painter& painter) override;

   private:
    /* The map view lives on the navigation stack, which owns it and may destroy
     * it in any order relative to this view. The link is shared with the map's
     * on_close callback, so a stale pointer is impossible. */
    struct MapLink {
        ui::GeoMapView* view{nullptr};
    };

    ui::Rect draw_field(ui::Painter& painter,
                        const ui::Rect& draw_rect,
                        const ui::Style& style,
                        const std::string& label,
                        const std::string& value);

    AISRecentEntry entry_{};
    std::shared_ptr<MapLink> map_link_{std::make_shared<MapLink>()};

    ui::Button button_see_map_{{8, 208, 104, 28}, "See on map"};
    ui::Button button_done_{{128, 208, 104, 28}, "Done"};
};

/* --- App ------------------------------------------------------------------ */

class AISAppView : public ui::View {
   public:
    AISAppView();
    ~AISAppView() override;

    AISAppView(const AISAppView&) = delete;
    AISAppView& operator=(const AISAppView&) = delete;

    std::string title() const override { return "AIS RX"; }

    void set_parent_rect(const ui::Rect new_parent_rect) override;
    void paint(ui::Painter&) override {}
    void focus() override;
    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

    /* Channel 87B / 88B, per ITU-R M.1371. */
    static constexpr uint64_t channel_87b_hz = 161'975'000;
    static constexpr uint64_t channel_88b_hz = 162'025'000;

    /* Capture rate the app asks the receiver for. An exact multiple of 38400 so
     * the channel decimation is an integer, and low enough that one snapshot of
     * the spectrum tap covers a useful slice of a 26.7 ms burst. */
    static constexpr double capture_rate_hz = 307'200.0;

    /* Read-only view of the ships heard, for src/remote/provider_ais.cpp to
     * publish to the web portal. Const by design: the portal only ever reads.
     * Same idiom as AprsTableView::entries() (ui_aprs_rx.hpp). */
    const AISRecentEntries& entries() const { return recent_; }

    /* Frames that passed both the length table and the FCS — the same number
     * update_status() paints as "P<n>" on the device's own status line, read
     * off the same decoder rather than counted a second time here. Exposed for
     * the same reason and with the same const-by-design rule as entries(). */
    size_t packets_valid() const { return decoder_.packets_valid(); }

   private:
    void on_packet(const ais::Packet& packet);
    void on_show_list();
    void on_show_detail(const AISRecentEntry& entry);
    void configure_front_end();
    void process_samples();
    void update_status();

    static constexpr int header_height = 32;

    AISRecentEntries recent_{};
    AISLogger logger_{};

    ui::RecentEntriesColumns columns_{{"MMSI", 9}, {"Name/Call", 0}};
    ui::RecentEntriesView<AISRecentEntries> recent_entries_view_{columns_, recent_};
    AISRecentEntryDetailView recent_entry_detail_view_{};

    ui::Labels labels_{
        {{0, 0}, "Ch", ui::Color::light_grey()},
    };

    ui::OptionsField options_channel_{
        {24, 0},
        3,
        {{"87B", static_cast<int32_t>(channel_87b_hz)},
         {"88B", static_cast<int32_t>(channel_88b_hz)}}};

    ui::Text text_level_{{56, 0, 72, 16}, ""};
    ui::Text text_counts_{{128, 0, 112, 16}, ""};
    ui::Text text_note_{{0, 16, 240, 16}, ""};

    /* Receive chain: wideband snapshot -> NCO -> channel FIR -> ais::Decoder. */
    ais::Decoder decoder_{};
    dsp::Nco nco_{};
    dsp::FirDecimateC channel_filter_{};
    std::vector<dsp::cfloat> samples_{};
    std::vector<dsp::cfloat> channel_{};

    double configured_rate_{0.0};
    double configured_offset_{1.0e30};  /* impossible, forces first configure */

    /* The wideband tap can hand back the same samples twice when the capture
     * rate is low enough that a snapshot outlives one UI frame. A frame
     * identical to the previous one within this window is a re-read, not a
     * second transmission — an AIS position report carries a UTC-second field
     * that changes every second, so a true repeat is never bit-identical this
     * quickly. */
    std::string last_frame_hex_{};
    std::chrono::steady_clock::time_point last_frame_time_{};

    size_t max_samples_per_frame_{4096};

    uint64_t previous_frequency_{0};
    double previous_sample_rate_{0.0};
    int previous_mode_{-1};
    bool receiver_was_running_{false};
};

}  // namespace app

#endif /*__MB200_AIS_APP_H__*/
