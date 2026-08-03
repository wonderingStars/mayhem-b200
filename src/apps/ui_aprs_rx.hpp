/*
 * mayhem-b200 — APRS receiver.
 *
 * Ported from the PortaPack firmware:
 *
 *   application/apps/ui_aprs_rx.*      -> AprsRxView / AprsStreamView /
 *                                         AprsTableView / AprsDetailsView
 *   baseband/proc_aprsrx.*             -> AprsChannelDecoder + Ax25Decoder
 *                                         (channel filter, FM + AFSK demod,
 *                                          NRZI, HDLC de-stuffing, FCS)
 *   common/aprs_packet.hpp             -> AprsPacket (address, digipeater and
 *                                         position parsing, incl. Mic-E and
 *                                         compressed formats)
 *   application/protocols/ax25.*       -> the FCS parameters and the on-air bit
 *                                         order that the decoder has to invert
 *
 * APRS is 1200 bps Bell 202 AFSK (mark 1200 Hz, space 2200 Hz) carried by NBFM,
 * framed as AX.25 UI frames. The receive chain is:
 *
 *   IQ -> NCO to channel centre -> decimating LPF (~12 kHz channel, ~24 kHz out)
 *      -> FM discriminator -> Bell 202 AFSK demod (dsp::AfskDemod)
 *      -> NRZI decode -> HDLC flag sync + de-stuffing -> byte assembly
 *      -> CRC-16/X-25 FCS check -> AX.25 address/information parse
 *
 * WHERE THE SAMPLES COME FROM — and the honest limitation.
 *
 * The firmware runs this pipeline on the M4 against every sample the radio
 * produces. The host ReceiverModel does not (yet) expose a continuous channel
 * tap: the only tap an app can read is take_spectrum_samples(), which hands
 * back a *snapshot* of the most recent samples and is refilled by the DSP
 * thread in blocks larger than the snapshot buffer. Samples are therefore
 * dropped between UI frames, and an AX.25 frame that spans a gap fails its FCS.
 *
 * The pipeline below is complete and correct when fed a continuous stream —
 * that is what tests/test_aprs_rx.cpp proves, by feeding AprsChannelDecoder a
 * synthesised Bell 202 signal. What it is NOT is a working live receiver, and
 * the view says so on screen rather than sitting there silently receiving
 * nothing. The tap this app wants is a callback (or ring) carrying every
 * channel-rate sample, e.g.
 *
 *     receiver->set_channel_sink([](const cfloat* p, size_t n){ ... });
 *
 * Departures from upstream, each justified where it appears in the .cpp:
 *
 *   1. Ax25Decoder clears its byte buffer after emitting a frame. Upstream
 *      leaves packet_buffer_size set, so two frames that share a single flag
 *      (the closing flag of one being the opening flag of the next) concatenate
 *      into one over-long buffer that then fails its FCS.
 *   2. The compressed-position decoders subtract 33 from the *fourth* base-91
 *      digit as the APRS 1.0.1 specification requires. Upstream omits it, which
 *      biases every compressed report by a fixed ~9.6 m in latitude and ~19 m
 *      in longitude.
 *
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_APRS_RX_H__
#define __MB200_UI_APRS_RX_H__

#include "../dsp/demod_digital.hpp"
#include "../dsp/fir.hpp"
#include "../dsp/protocol.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_geomap.hpp"
#include "ui_recent_entries.hpp"
#include "ui_widget.hpp"
#include "ui_widget_extra.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace app {

/* ===========================================================================
 * AX.25 frame check sequence
 *
 * CRC-16/X-25: polynomial 0x1021, reflected in and out, init 0xFFFF, final XOR
 * 0xFFFF. Those are exactly the parameters upstream's transmitter uses
 * (application/protocols/ax25.hpp: `CRC<16, true, true> crc_ccitt{0x1021,
 * 0xFFFF, 0xFFFF}`), and the receiver in baseband/proc_aprsrx.cpp is the same
 * CRC expressed as a reflected 0x8408 table.
 * ===========================================================================*/

/* The FCS a transmitter appends to `data`. It goes on the wire low byte first,
 * each byte least-significant bit first, and is NOT itself covered by the CRC. */
uint16_t ax25_fcs(const uint8_t* data, size_t length);

/* True when `frame` (address..information *plus* its two FCS bytes) checks out.
 *
 * Upstream runs its reflected-domain register over data+FCS and tests for the
 * residue 0xF0B8. dsp::Crc keeps the same register in the un-reflected domain,
 * where that constant is its bit reversal, 0x1D0F. Same test, same bits. */
bool ax25_frame_valid(const uint8_t* frame, size_t length);

/* ===========================================================================
 * APRS packet (port of common/aprs_packet.hpp)
 * ===========================================================================*/

/* Shortest thing that can be a frame: 14 bytes of address, control, PID, 2 FCS. */
constexpr size_t kAprsMinLength = 18;

/* Upstream's packet_buffer is 256 bytes and its index is a uint8_t. */
constexpr size_t kAprsMaxLength = 256;

struct AprsPosition {
    float latitude{0.0f};
    float longitude{0.0f};
    uint8_t symbol_code{0};
    uint8_t sym_table_id{0};
};

enum class AprsAddressType : uint8_t {
    Source,
    Destination,
    Repeater,
};

class AprsPacket {
   public:
    AprsPacket() = default;

    void clear();
    void set(size_t index, uint8_t data);
    void set_bytes(const uint8_t* data, size_t length);

    uint8_t operator[](size_t index) const;
    size_t size() const { return payload_size_; }

    void set_valid_checksum(bool valid) { valid_checksum_ = valid; }
    bool is_valid_checksum() const { return valid_checksum_; }

    /* The seven raw address bytes of the source callsign, packed little-endian.
     * Upstream uses this as the RecentEntries key: it is unique per
     * callsign+SSID and needs no string compare. */
    uint64_t get_source() const;

    std::string get_source_formatted() const;
    std::string get_destination_formatted() const;
    /* "" when there are no digipeaters, otherwise ",CALL-n,CALL-n". */
    std::string get_digipeaters_formatted() const;
    size_t get_number_of_digipeaters() const;

    /* First byte of the information field: past the addresses, the control byte
     * and the PID byte. */
    size_t get_information_start_index() const;
    std::string get_information_text_formatted() const;

    /* "SRC>DEST,DIGI:information", the line the stream console shows. */
    std::string get_stream_text() const;

    char get_data_type_identifier() const;
    bool has_position() const;
    AprsPosition get_position() const;

   private:
    static constexpr size_t kDestinationStart = 0;
    static constexpr size_t kSourceStart = 7;
    static constexpr size_t kDigipeaterStart = 14;
    static constexpr size_t kAddressSize = 7;

    /* Decodes one shifted AX.25 address into `out`, returning the HDLC
     * extension bit: true when another address follows. */
    bool parse_address(size_t start, AprsAddressType type, std::string& out) const;

    void parse_mic_e_format(AprsPosition& pos) const;

    static float parse_lat_str(const std::string& lat_str);
    static float parse_lng_str(const std::string& lng_str);
    static float parse_lat_str_cmp(const std::string& lat_str);
    static float parse_lng_str_cmp(const std::string& lng_str);
    static uint32_t parse_digits(const std::string& str);

    static uint8_t mic_e_lat_digit(uint8_t ascii);
    static bool mic_e_lat_is_north(uint8_t ascii);
    static bool mic_e_lng_is_west(uint8_t ascii);
    static uint8_t mic_e_lng_offset(uint8_t ascii);

    uint8_t payload_[kAprsMaxLength]{};
    size_t payload_size_{0};
    bool valid_checksum_{false};
};

/* ===========================================================================
 * AX.25 HDLC bit decoder (port of APRSRxProcessor::parse_bit / parse_packet)
 * ===========================================================================*/

class Ax25Decoder {
   public:
    /* Called for every frame whose FCS checks out. `length` includes the two
     * FCS bytes, matching upstream's packet_buffer. */
    using FrameHandler = std::function<void(const uint8_t* frame, size_t length)>;

    void set_frame_handler(FrameHandler handler) { on_frame_ = std::move(handler); }
    void reset();

    /* One raw slicer bit straight off the AFSK demodulator, before NRZI. */
    void feed_bit(uint8_t raw_bit);
    void feed_bits(const uint8_t* bits, size_t count);
    void feed_bits(const std::vector<uint8_t>& bits);

    size_t frames_valid() const { return frames_valid_; }
    size_t frames_bad_fcs() const { return frames_bad_fcs_; }
    size_t frames_too_short() const { return frames_too_short_; }
    size_t bits_fed() const { return bits_fed_; }

   private:
    enum class State : uint8_t { WaitFlag, WaitFrame, InFrame };

    /* Returns true when the bit just consumed closed a frame. */
    bool parse_bit(uint8_t raw_bit);
    void abort_frame();

    FrameHandler on_frame_{};

    State state_{State::WaitFlag};
    uint8_t last_bit_{0};
    uint8_t ones_count_{0};
    uint8_t current_byte_{0};
    uint8_t bit_index_{0};
    uint8_t buffer_[kAprsMaxLength]{};
    size_t buffer_size_{0};

    size_t frames_valid_{0};
    size_t frames_bad_fcs_{0};
    size_t frames_too_short_{0};
    size_t bits_fed_{0};
};

/* ===========================================================================
 * Channel decoder: complex baseband in, APRS packets out
 *
 * This is the host equivalent of APRSRxProcessor::execute(): the whole chain
 * from captured IQ to a parsed packet, with no UI and no radio, so it can be
 * driven from a test.
 * ===========================================================================*/

class AprsChannelDecoder {
   public:
    /* Nominal rate the AFSK demodulator wants, matching upstream's audio_fs
     * (3072000 / 8 / 8 / 2). The real rate is the input rate divided by an
     * integer, so it lands near this rather than on it. */
    static constexpr double kNominalChannelRate = 24000.0;

    /* One-sided channel half-bandwidth. Upstream uses an 11 kHz channel filter
     * (taps_11k0_channel) for the 5 kHz-deviation NBFM APRS signal. */
    static constexpr double kChannelCutoffHz = 5500.0;

    /* `frequency_offset_hz` is where the wanted signal sits inside the captured
     * band: positive means above the centre of `in`. */
    void configure(double input_rate_hz, double frequency_offset_hz);
    void set_frequency_offset(double frequency_offset_hz);
    void reset();

    void process(const dsp::cfloat* in, size_t count);

    /* Fires for each frame with a good FCS. */
    std::function<void(const AprsPacket&)> on_packet{};

    bool configured() const { return configured_; }
    double input_rate() const { return input_rate_hz_; }
    double channel_rate() const { return channel_rate_hz_; }
    size_t decimation() const { return decimation_; }
    double frequency_offset() const { return offset_hz_; }

    const Ax25Decoder& framer() const { return framer_; }
    Ax25Decoder& framer() { return framer_; }

   private:
    dsp::Nco nco_{};
    dsp::FirDecimateC channel_filter_{};
    dsp::AfskDemod afsk_{};
    Ax25Decoder framer_{};

    std::vector<dsp::cfloat> mixed_{};
    std::vector<dsp::cfloat> channel_{};
    std::vector<uint8_t> bits_{};

    double input_rate_hz_{0.0};
    double channel_rate_hz_{0.0};
    double offset_hz_{0.0};
    size_t decimation_{1};
    bool configured_{false};
};

/* ===========================================================================
 * UI
 * ===========================================================================*/

/* Upstream's region list and its frequencies (ui_aprs_rx.cpp options_region).
 * Index 0 is "MAN", which keeps whatever the operator dialled in. */
struct AprsRegion {
    const char* name;
    uint64_t frequency;
};

extern const AprsRegion kAprsRegions[];
constexpr size_t kAprsRegionCount = 10;

struct AprsRecentEntry {
    using Key = uint64_t;
    static constexpr Key invalid_key = 0xffffffffffffffffULL;

    explicit AprsRecentEntry(Key src) : source{src} {}

    Key key() const { return source; }

    Key source{0};
    std::string source_formatted{};
    std::string time_string{};
    std::string info_string{};
    AprsPosition pos{};
    bool has_position{false};
    uint32_t hits{0};

    void inc_hit() { hits++; }
};

using AprsRecentEntries = ui::RecentEntries<AprsRecentEntry>;

/* --- Stream tab: controls plus the raw packet console --------------------- */

class AprsStreamView : public ui::View {
   public:
    explicit AprsStreamView(ui::Rect parent_rect);

    std::string title() const override { return "Stream"; }

    void focus() override;

    void on_packet(const AprsPacket& packet);
    /* Decoder health, shown so an operator can tell "no traffic" from
     * "nothing is reaching the decoder". */
    void set_stats(size_t valid, size_t bad_fcs, size_t bits);

    /* Fired when the operator picks a region or edits the frequency. */
    std::function<void(uint64_t)> on_frequency_changed{};

    uint64_t frequency() const { return field_frequency_.value(); }
    void set_frequency(uint64_t hz);
    void set_region(size_t index);
    size_t region() const { return region_index_; }

   private:
    void apply_region(size_t index);

    size_t region_index_{1}; /* North America, upstream's default */
    uint8_t console_color_{0};
    bool suppress_region_{false};

    ui::OptionsField options_region_{{0, 0}, 3, {}};
    ui::FrequencyField field_frequency_{{4 * 8, 0}};
    ui::NumberField field_gain_{{15 * 8, 0}, 3, {0, 76}, 1, ' '};
    ui::Text text_gain_label_{{19 * 8, 0, 4 * 8, 16}, "dB"};

    ui::Text text_stats_{{0, 16, 240, 16}, "Pkt 0 FCS 0"};

    ui::Labels labels_note_{
        {{0, 32}, "Wideband snapshot tap only:", ui::Color::yellow()},
        {{0, 48}, "samples drop between frames,", ui::Color::grey()},
        {{0, 64}, "so live decode is unreliable.", ui::Color::grey()},
        {{0, 80}, "Needs a continuous ch. tap.", ui::Color::grey()},
    };

    ui::Console console_{{0, 100, 240, 160}};
};

/* --- Details: one station's last information field, plus a map ------------ */

class AprsDetailsView : public ui::View {
   public:
    AprsDetailsView();

    std::string title() const override { return "Details"; }

    void focus() override;

    void set_entry(const AprsRecentEntry& entry);
    const AprsRecentEntry& entry() const { return entry_; }
    void update();

    std::function<void()> on_close{};

   private:
    AprsRecentEntry entry_{0};
    ui::GeoMapView* geomap_view_{nullptr};

    ui::Text text_source_{{0, 0, 240, 16}, ""};
    ui::Text text_position_{{0, 16, 240, 16}, ""};
    ui::Console console_{{0, 32, 240, 176}};

    ui::Button button_close_{{16, 216, 88, 32}, "Close"};
    ui::Button button_map_{{136, 216, 88, 32}, "Map"};
};

/* --- Stations tab: the RecentEntries table -------------------------------- */

class AprsTableView : public ui::View {
   public:
    explicit AprsTableView(ui::Rect parent_rect);

    std::string title() const override { return "Stations"; }

    void on_show() override;
    void on_hide() override;
    void focus() override;

    void on_packet(const AprsPacket& packet, const std::string& timestamp);

    const AprsRecentEntries& entries() const { return recent_; }

   private:
    void show_list();
    void show_detail(const AprsRecentEntry& entry);

    AprsRecentEntries recent_{};
    ui::RecentEntriesColumns columns_{{{"Source", 0}, {"Loc", 6}, {"Hits", 4}, {"Time", 8}}};
    ui::RecentEntriesView<AprsRecentEntries> entries_view_{columns_, recent_};
    AprsDetailsView details_view_{};
};

/* --- Top level ------------------------------------------------------------ */

class AprsRxView : public ui::View {
   public:
    AprsRxView();
    ~AprsRxView() override;

    AprsRxView(const AprsRxView&) = delete;
    AprsRxView& operator=(const AprsRxView&) = delete;

    std::string title() const override { return "APRS RX"; }

    void focus() override;
    void on_show() override;
    void on_frame_sync() override;

   private:
    void retune(uint64_t hz);
    void update_offset();
    void handle_packet(const AprsPacket& packet);

    radio::ReceiverModel& receiver_;
    AprsChannelDecoder decoder_{};

    std::vector<dsp::cfloat> samples_{};
    double configured_rate_{0.0};
    uint32_t frame_counter_{0};

    ui::Rect content_rect_{0, 24, 240, 280};

    AprsStreamView view_stream_{content_rect_};
    AprsTableView view_table_{content_rect_};

    ui::TabView tab_view_{
        {"Stream", ui::Color::cyan(), &view_stream_},
        {"Stations", ui::Color::yellow(), &view_table_}};
};

}  // namespace app

#endif /*__MB200_UI_APRS_RX_H__*/
