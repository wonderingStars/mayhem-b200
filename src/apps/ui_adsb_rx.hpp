/*
 * mayhem-b200 — ADS-B / Mode S receiver.
 *
 * Ported from, and specified by, upstream PortaPack Mayhem:
 *
 *   firmware/application/apps/ui_adsb_rx.{hpp,cpp}   the app and frame parsing
 *   firmware/baseband/proc_adsbrx.{hpp,cpp}          the PPM demodulator
 *   firmware/common/adsb_frame.hpp                   ADSBFrame + Mode S CRC-24
 *   firmware/common/adsb.cpp                         CPR / callsign / velocity
 *
 * Mode S downlink is 1090 MHz, pulse-position modulated at 1 Mbit/s, so at the
 * 2 Msps the firmware runs it there are exactly two samples per bit and a bit is
 * the *transition* between them: high->low is a 1, low->high is a 0. A frame is
 * an 8 us preamble (pulses at 0, 1.0, 3.5 and 4.5 us) followed by 56 or 112 data
 * bits; the last 24 bits are a CRC over the rest with the generator
 *
 *     G(x) = x^24 + x^23 + ... + x^12 + x^10 + x^3 + 1     (0xFFF409)
 *
 * which upstream writes in reversed-offset form as 0x1205FFF. Both spellings are
 * the same polynomial — verified bit by bit, and the two implementations are
 * cross-checked against each other in tests/test_adsb.cpp.
 *
 * WHAT CHANGED FOR THE HOST
 *
 *  1. There is no M4 baseband core and no message queue. AdsbDemod is the
 *     ported ADSBRXProcessor::execute() state machine, driven from the view's
 *     on_frame_sync() with samples pulled off the receiver instead of from a
 *     buffer_c8_t handed over by an interrupt.
 *
 *  2. Sample tap. ReceiverModel only exposes take_spectrum_samples(), which
 *     hands out the most recent 4096 raw samples (~2 ms at 2 Msps) and is
 *     re-armed once per DSP block. Polled at the 60 Hz UI rate that recovers
 *     roughly 12% of the air time, so frames are decoded correctly but many are
 *     missed. The tap this app actually wants is a gapless raw-IQ subscription
 *     at the tuned rate — see the note on AdsbRxView::pump().
 *
 *  3. `byte` in upstream's execute() is a *local* that resets on every baseband
 *     buffer, so a frame straddling a buffer boundary decodes with corrupted
 *     bytes. Here the bit accumulator is demodulator state, so the result does
 *     not depend on how the caller chunks its samples.
 *
 *  4. Integer approximations (fast_int_magnitude, int_atan2) exist because the
 *     LPC43xx M0 has no FPU; the host uses exact std::hypot / std::atan2. The
 *     approximation is ~4% low on speed (it reports 155 kt where the true value
 *     is 159.2), so this is a fidelity improvement, not a protocol change.
 *
 *  5. cpr_NL() is guarded at |lat| >= 87 and lat == 0. Upstream's unguarded
 *     acos() argument leaves [-1, 1] near the poles and floor(NaN) is undefined.
 *     The guards are dump1090's, which upstream's own comment cites as its
 *     source.
 *
 *  6. The aircraft/airline database views are not ported: they read icao24.db
 *     and airlines.db off the PortaPack's SD card, which the host does not have.
 *     The details view says so on screen rather than showing blank fields.
 *
 * NOTHING HERE HAS SEEN AIR. No USRP was attached during development, so the
 * end-to-end path (1090 MHz -> B200 -> demodulator -> table) is unverified. The
 * decode chain itself is tested against documented Mode S frames and against
 * synthesised PPM waveforms in tests/test_adsb.cpp.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2023 Kyle Reed
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_ADSB_RX_H__
#define __MB200_UI_ADSB_RX_H__

#include "../dsp/protocol.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_geomap.hpp"
#include "ui_recent_entries.hpp"
#include "ui_widget.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace app {

/* =========================================================================
 * The protocol layer. Free of any UI so it can be tested on its own.
 * ========================================================================= */
namespace adsb {

/* Downlink formats this app understands (frame.get_DF()). */
enum downlink_format : uint8_t {
    DF_SHORT_AIR_AIR = 0,
    DF_SURV_ALT = 4,
    DF_SURV_IDENT = 5,
    DF_ALL_CALL = 11,
    DF_ADSB = 17,
    DF_ADSB_NT = 18,
    DF_COMM_B_ALT = 20,
    DF_EHS_SQUAWK = 21,
};

/* ME type codes (frame.get_msg_type()). */
enum type_code : uint8_t {
    TC_IDENT = 4,
    TC_AIRBORNE_POS = 11,
    TC_AIRBORNE_VELO = 19,
};

enum speed_type : uint8_t {
    SPD_GND = 0, /* ground speed  */
    SPD_IAS = 1, /* indicated airspeed */
    SPD_TAS = 2, /* true airspeed */
};

/* ME type code ranges, upstream's #defines. */
constexpr uint8_t AIRCRAFT_ID_L = 1;
constexpr uint8_t AIRCRAFT_ID_H = 4;
constexpr uint8_t SURFACE_POS_L = 5;
constexpr uint8_t SURFACE_POS_H = 8;
constexpr uint8_t AIRBORNE_POS_BARO_L = 9;
constexpr uint8_t AIRBORNE_POS_BARO_H = 18;
constexpr uint8_t AIRBORNE_VEL = 19;
constexpr uint8_t AIRBORNE_POS_GPS_L = 20;
constexpr uint8_t AIRBORNE_POS_GPS_H = 22;
constexpr uint8_t AIRBORNE_OP_STATUS = 31;

constexpr uint8_t VEL_GND_SUBSONIC = 1;
constexpr uint8_t VEL_GND_SUPERSONIC = 2;
constexpr uint8_t VEL_AIR_SUBSONIC = 3;
constexpr uint8_t VEL_AIR_SUPERSONIC = 4;

/* Seconds an even/odd position pair may be apart and still be combined. */
constexpr uint32_t O_E_FRAME_TIMEOUT = 20;

struct adsb_pos {
    bool pos_valid{false};
    bool alt_valid{false};
    float latitude{0};
    float longitude{0};
    int32_t altitude{0};
};

struct adsb_vel {
    bool valid{false};
    speed_type type{SPD_GND};
    int32_t speed{0};
    uint16_t heading{0};
    int32_t v_rate{0};
};

/* 6-bit character set for the aircraft identification message. Upstream's
 * table, '#' marking the code points that carry no character. */
inline constexpr char icao_id_lut[65] =
    "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";

/* Mode S generator polynomial, truncated (the x^24 term is implicit). */
constexpr uint32_t kModeSPolynomial = 0xFFF409;

/* CRC-24 over `length` bytes, MSB-first, zero init, no final XOR — i.e. the
 * remainder of data * x^24 modulo G(x). Uses the Phase A CRC engine. */
uint32_t mode_s_crc(const uint8_t* data, size_t length);

/* Upstream's ADSBFrame: a 14-byte buffer with a push cursor, plus the CRC and
 * field accessors. Copyable, because a recent-entry keeps its own even/odd
 * position frames. */
class AdsbFrame {
   public:
    static constexpr size_t max_bytes = 14; /* 112 bits */

    uint8_t get_DF() const { return static_cast<uint8_t>(raw_data_[0] >> 3); }
    uint8_t get_msg_type() const { return static_cast<uint8_t>(raw_data_[4] >> 3); }
    uint8_t get_msg_sub() const { return static_cast<uint8_t>(raw_data_[4] & 7); }

    /* Surveillance integrity level, ME bits 83-84 (operational status msg). */
    uint8_t get_sil_value() const { return static_cast<uint8_t>((raw_data_[10] >> 6) & 0x3); }

    uint32_t get_ICAO_address() const {
        return (static_cast<uint32_t>(raw_data_[1]) << 16) |
               (static_cast<uint32_t>(raw_data_[2]) << 8) |
               static_cast<uint32_t>(raw_data_[3]);
    }

    /* True for the long (112-bit) formats, DF >= 16. */
    bool is_long() const { return (raw_data_[0] & 0x80) != 0; }

    /* Bytes of payload ahead of the 24-bit CRC. */
    size_t data_length() const { return is_long() ? 11u : 4u; }

    void set_rx_timestamp(uint32_t timestamp) { rx_timestamp_ = timestamp; }
    uint32_t get_rx_timestamp() const { return rx_timestamp_; }

    void clear() {
        index_ = 0;
        std::memset(raw_data_, 0, sizeof(raw_data_));
    }

    void push_byte(uint8_t byte) {
        if (index_ >= max_bytes) return;
        raw_data_[index_++] = byte;
    }

    const uint8_t* get_raw_data() const { return raw_data_; }
    uint8_t* get_raw_data() { return raw_data_; }

    size_t size() const { return index_; }
    bool empty() const { return index_ == 0; }

    /* Writes the computed CRC into the frame's parity field. */
    void make_CRC();

    /* Zero when the parity field matches the payload. For the address-parity
     * formats (DF 0/4/5/16/20/21) a correct frame instead yields the ICAO
     * address of the transponder that sent it, which is what upstream keys the
     * entry on when the syndrome is non-zero. */
    uint32_t check_CRC() const;

    uint32_t compute_CRC() const { return mode_s_crc(raw_data_, data_length()); }

   private:
    uint8_t index_{0};
    uint8_t raw_data_[max_bytes]{};
    uint32_t rx_timestamp_{0};
};

/* --- Compact Position Reporting -------------------------------------------- */

constexpr double CPR_MAX_VALUE = 131072.0; /* 2^17 */
constexpr double NZ = 15.0;                /* number of latitude zones / 4 */

/* Positive-result modulo, upstream's cpr_mod. */
double cpr_mod(double a, double b);

/* Number of longitude zones at a latitude. dump1090's special cases apply:
 * 59 at the equator, 2 at exactly +/-87 deg, 1 beyond that. */
int cpr_NL(double lat);

/* NL(lat) minus the frame parity, floored at 1. */
int cpr_N(double lat, int is_odd);

/* Longitude zone width in degrees. */
double cpr_Dlon(double lat, int is_odd);

/* --- Message decoders ------------------------------------------------------ */

/* ME type 1-4: eight 6-bit characters, ME bits 9-56 (frame bytes 5..10). */
std::string decode_frame_id(const AdsbFrame& frame);

/* ME type 9-18 / 20-22: globally unambiguous position from an even/odd pair.
 * The altitude reported is the one from whichever frame is newer. */
adsb_pos decode_frame_pos(const AdsbFrame& frame_even, const AdsbFrame& frame_odd);

/* ME type 19: ground speed (subtypes 1-2) or airspeed (subtypes 3-4). */
adsb_vel decode_frame_velo(const AdsbFrame& frame);

/* The 13-bit identity field, from the two bytes at `s`. Returns the squawk as
 * a decimal number whose digits are the four octal digits, e.g. 07500 -> 7500,
 * matching what upstream displays. */
uint16_t decode_squawk(const uint8_t* s);

/* The 12-bit AC altitude field of DF 0/4/16/20, in feet. Sets `valid` false
 * for the metric (M-bit) and Gillham (Q-bit clear) encodings, which upstream
 * does not decode either. */
int32_t decode_ac13_altitude(const uint8_t* raw_data, bool& valid);

/* The 12-bit altitude of an airborne position / type-0 message (frame bytes
 * 5-6), in feet. Sets `valid` false when the Q bit is clear. */
int32_t decode_me_altitude(const uint8_t* raw_data, bool& valid);

/* --- Demodulator ----------------------------------------------------------- */

constexpr size_t kPreambleLength = 16; /* 8 us at 2 samples/us */

/* Linear interpolation of an envelope (magnitude) stream from one rate to
 * another. Legitimate for on/off keying, and the only way to decode when the
 * device cannot deliver exactly 2 Msps, but it is not free: an output sample
 * whose two neighbours straddle a half-bit edge lands between the two levels,
 * which costs contrast on the bit decision. Split out from the demodulator so
 * the rate arithmetic can be checked on its own. */
class MagnitudeResampler {
   public:
    /* input_hz == output_hz (within a hertz) selects bypass, and samples are
     * then passed through untouched. */
    void set_rate(double input_hz, double output_hz);

    void reset();

    /* Appends the converted stream to `out`. */
    void process(const float* in, size_t count, std::vector<float>& out);

    double step() const { return step_; }
    bool bypass() const { return bypass_; }

   private:
    double step_{1.0};
    double frac_{0.0};
    float prev_{0.0f};
    bool primed_{false};
    bool bypass_{true};
};

/* The ported ADSBRXProcessor. Feed it magnitude samples at 2 Msps (or complex
 * baseband at any rate, and it resamples); it calls back with every frame whose
 * preamble it locked onto. CRC checking is the caller's job, exactly as it is
 * upstream — the baseband forwards everything and the app filters. */
class AdsbDemod {
   public:
    /* 1 Mbit/s, 2 samples per bit. */
    static constexpr double kBitRate = 1'000'000.0;
    static constexpr double kNativeSampleRate = 2'000'000.0;

    using FrameHandler = std::function<void(const AdsbFrame& frame, float amp)>;

    AdsbDemod() { reset(); }

    void reset();

    /* Rate of the samples handed to process(). Anything other than 2 Msps is
     * linearly resampled to 2 Msps on the magnitude stream, which is legitimate
     * for an envelope-detected on/off keyed signal but costs sensitivity; the
     * app asks the radio for 2 Msps and only relies on this when the device
     * cannot deliver it. */
    void set_input_rate(double hz);
    double input_rate() const { return input_rate_; }

    /* Complex baseband. Magnitudes are computed in the same units the firmware
     * works in (samples scaled to int8 full scale) so that the `amp` reported
     * with each frame is on upstream's scale and displays identically. */
    void process(const dsp::cfloat* samples, size_t count, const FrameHandler& on_frame);

    /* Magnitude (|s|^2) samples already at 2 Msps. */
    void process_magnitudes(const float* mags, size_t count, const FrameHandler& on_frame);

    uint32_t preambles_detected() const { return preambles_; }
    uint32_t frames_emitted() const { return frames_; }

   private:
    void process_one(float mag, const FrameHandler& on_frame);

    double input_rate_{kNativeSampleRate};
    MagnitudeResampler resampler_{};
    std::vector<float> mag_raw_{};
    std::vector<float> mag_scratch_{};

    AdsbFrame frame_{};
    bool decoding_{false};
    size_t msg_len_{112};
    float prev_mag_{0.0f};
    float amp_{0.0f};
    size_t bit_count_{0};
    size_t sample_count_{0};
    uint8_t byte_{0};

    float shifter_[kPreambleLength + 1]{};

    uint32_t preambles_{0};
    uint32_t frames_{0};
};

}  // namespace adsb

/* =========================================================================
 * Aircraft tracking — the recent-entries model, still UI-free.
 * ========================================================================= */

/* Thresholds (seconds) separating the age states. Upstream's ADSBAgeLimit. */
struct ADSBAgeLimit {
    static constexpr uint32_t Current = 10;
    static constexpr uint32_t Recent = 30;
    static constexpr uint32_t Expired = 300;
};

enum class ADSBAgeState : uint8_t {
    Invalid,
    Current,
    Recent,
    Old,
    Expired,
};

struct AircraftRecentEntry {
    using Key = uint32_t;
    static constexpr Key invalid_key = 0xffffffffu;

    uint32_t ICAO_address{};
    uint16_t hits{0};

    ADSBAgeState state{ADSBAgeState::Invalid};
    uint32_t age{0}; /* seconds since the last accepted frame */
    uint32_t amp{0};
    adsb::adsb_pos pos{};
    adsb::adsb_vel velo{false, adsb::SPD_GND, 0, 999, 0};
    adsb::AdsbFrame frame_pos_even{};
    adsb::AdsbFrame frame_pos_odd{};

    std::string icao_str{};
    std::string callsign{};
    std::string info_string{};

    uint8_t sil{0};
    uint16_t sqwk{0};

    explicit AircraftRecentEntry(uint32_t address);

    Key key() const { return ICAO_address; }

    void set_callsign(std::string new_callsign) { callsign = std::move(new_callsign); }
    void inc_hit() { hits++; }
    void reset_age() { age = 0; }
    void set_info_string(std::string s) { info_string = std::move(s); }

    /* Stores the frame in the even or odd slot and, when both slots hold
     * frames received close enough together, decodes the position. */
    void set_frame_pos(const adsb::AdsbFrame& frame, uint32_t parity);
    void set_frame_velo(const adsb::AdsbFrame& frame);

    /* Ground speed in knots, correcting IAS for altitude the way upstream
     * does (roughly +2% per 1000 ft). */
    int32_t get_ground_speed() const;

    void inc_age(uint32_t delta);
};

using AircraftRecentEntries = ui::RecentEntries<AircraftRecentEntry>;

/* One line of the ADSB.TXT log. */
struct AdsbLogEntry {
    std::string raw_data{};
    std::string icao{};
    std::string callsign{};
    adsb::adsb_pos pos{};
    adsb::adsb_vel vel{};
    uint8_t vel_type{0};
    uint8_t sil{0};
    uint16_t sqwk{0};
};

/* Upstream's ADSBLogger::log() line format. Pure, so it is tested directly. */
std::string format_adsb_log_line(const AdsbLogEntry& entry);

/* The frame-to-entry pipeline of ADSBRxView::on_frame(), lifted out of the view
 * so it can be exercised without a display or a radio. */
class AircraftTracker {
   public:
    /* Consumes one demodulated frame. Returns true if it was accepted (CRC
     * clean, or a syndrome matching a known aircraft's address parity) and an
     * entry was updated. `rx_timestamp` is in seconds and only ever used as a
     * difference, so any monotonic seconds counter works.
     *
     * When `log_out` is non-null it receives the log record for the frame; it
     * is left untouched for frames that are rejected or deliberately not
     * logged (DF11, which arrives far too often to be worth a line). */
    bool handle_frame(adsb::AdsbFrame& frame,
                      uint32_t amp,
                      uint32_t rx_timestamp,
                      AdsbLogEntry* log_out = nullptr,
                      bool* logged_out = nullptr);

    /* Ages every entry, re-sorts by state, truncates and drops the expired. */
    void age_entries(uint32_t delta, size_t max_entries = 64);

    AircraftRecentEntries& entries() { return recent_; }
    const AircraftRecentEntries& entries() const { return recent_; }

    AircraftRecentEntry* find(uint32_t icao);
    const AircraftRecentEntry* find(uint32_t icao) const;

    uint32_t frames_seen() const { return frames_seen_; }
    uint32_t frames_accepted() const { return frames_accepted_; }

    void clear();

   private:
    AircraftRecentEntry& find_or_create_entry(uint32_t ICAO_address);
    void sort_entries_by_state();
    void remove_expired_entries();

    AircraftRecentEntries recent_{};
    uint32_t frames_seen_{0};
    uint32_t frames_accepted_{0};
};

/* =========================================================================
 * Views
 * ========================================================================= */

class AdsbRxView;
class AdsbRxDetailsView;

/* GeoMapView with a tick hook. The host only ticks the top view, so a map
 * pushed above the details view has to pump the decoder itself or everything
 * below it freezes while the map is open. */
class AdsbMapView : public ui::GeoMapView {
   public:
    using ui::GeoMapView::GeoMapView;

    std::function<void()> on_tick{};

    void on_frame_sync() override {
        ui::GeoMapView::on_frame_sync();
        if (on_tick) on_tick();
    }
};

/* Per-aircraft detail, and the door to the map. */
class AdsbRxDetailsView : public ui::View {
   public:
    AdsbRxDetailsView(AdsbRxView& parent, const AircraftRecentEntry& entry);
    ~AdsbRxDetailsView() override;

    AdsbRxDetailsView(const AdsbRxDetailsView&) = delete;
    AdsbRxDetailsView& operator=(const AdsbRxDetailsView&) = delete;

    std::string title() const override { return "Details"; }

    void focus() override;
    void on_frame_sync() override;

    /* Altitude-to-colour ramp for map markers, upstream's stops. */
    static ui::Color altitude_color(int32_t alt_ft);

   private:
    void refresh_ui();
    void refresh_map();
    void open_map();

    AdsbRxView& parent_;
    AircraftRecentEntry entry_;
    AdsbMapView* map_view_{nullptr};
    uint8_t map_filter_{0}; /* 0: every aircraft, 1: only this one */

    struct PosHistory {
        float lat;
        float lon;
        uint16_t heading;
        int32_t altitude;
    };
    std::vector<PosHistory> pos_history_{};
    void add_map_trail(const AircraftRecentEntry& entry);
    void refresh_trail_markers();
    bool add_map_marker(const AircraftRecentEntry& entry);

    uint32_t tick_{0};

    ui::Labels labels_{
        {{0, 0}, "ICAO:", ui::Color::light_grey()},
        {{13 * 8, 0}, "Call:", ui::Color::light_grey()},
        {{0, 16}, "Last seen:", ui::Color::light_grey()},
        {{0, 32}, "Squawk:", ui::Color::light_grey()},
        {{13 * 8, 32}, "SIL:", ui::Color::light_grey()},
        {{0, 160}, "Even position frame:", ui::Color::light_grey()},
        {{0, 192}, "Odd position frame:", ui::Color::light_grey()},
    };

    ui::Text text_icao_{{5 * 8, 0, 7 * 8, 16}, "-"};
    ui::Text text_callsign_{{18 * 8, 0, 9 * 8, 16}, "-"};
    ui::Text text_last_seen_{{11 * 8, 16, 19 * 8, 16}, "-"};
    ui::Text text_squawk_{{7 * 8, 32, 6 * 8, 16}, "-"};
    ui::Text text_sil_{{17 * 8, 32, 4 * 8, 16}, "-"};
    ui::Text text_infos_{{0, 48, 240, 16}, "-"};
    ui::Text text_info2_{{0, 64, 240, 16}, "-"};
    ui::Text text_frame_pos_even_{{0, 176, 240, 16}, "-"};
    ui::Text text_frame_pos_odd_{{0, 208, 240, 16}, "-"};

    ui::OptionsField opt_map_list_{
        {0, 88},
        12,
        {{"Map: all   ", 0},
         {"Map: only me", 1}}};

    ui::Button button_see_map_{{0, 108, 14 * 8, 32}, "See on map"};

    ui::Labels db_note_{
        {{0, 232}, "No aircraft/airline lookup:", ui::Color::yellow()},
        {{0, 248}, " icao24.db and airlines.db", ui::Color::grey()},
        {{0, 264}, " live on the PortaPack SD", ui::Color::grey()},
        {{0, 280}, " card; no host equivalent.", ui::Color::grey()},
    };
};

/* The app. */
class AdsbRxView : public ui::View {
   public:
    AdsbRxView();
    ~AdsbRxView() override;

    AdsbRxView(const AdsbRxView&) = delete;
    AdsbRxView& operator=(const AdsbRxView&) = delete;

    std::string title() const override { return "ADS-B RX"; }

    void focus() override;
    void on_show() override;
    void on_frame_sync() override;

    /* Pulls whatever samples the receiver has, runs them through the
     * demodulator and folds the resulting frames into the tracker. Public
     * because the views pushed on top of this one have to call it — nothing
     * below the top of the navigation stack is ticked, so without this the
     * decoder would stop the moment the operator opened the detail page.
     *
     * IDEAL TAP: a gapless subscription to the raw RX stream, e.g.
     * ReceiverModel::add_raw_sink(std::function<void(const cfloat*, size_t)>)
     * called from the DSP thread, or a second consumer cursor on
     * UsrpRadio::rx_ring(). take_spectrum_samples() is a 4096-sample snapshot
     * that the DSP thread overwrites continuously, so at 2 Msps and a 60 Hz
     * poll this sees about 12% of the air time. Every frame it does see is
     * decoded correctly; the rest are simply never presented. */
    void pump();

    /* Advances ages and prunes; called once a second by whichever view is on
     * top. */
    void tick_second();

    AircraftTracker& tracker() { return tracker_; }
    const AircraftTracker& tracker() const { return tracker_; }

    uint32_t frames_seen() const { return tracker_.frames_seen(); }
    uint32_t frames_accepted() const { return tracker_.frames_accepted(); }

    /* ADS-B is a fixed-frequency service. */
    static constexpr uint64_t kFrequency = 1'090'000'000;

   private:
    void refresh_status();
    void set_logging(bool enabled);
    void write_log(const AdsbLogEntry& entry);

    radio::ReceiverModel* receiver_{nullptr};
    adsb::AdsbDemod demod_{};
    AircraftTracker tracker_{};

    std::vector<dsp::cfloat> samples_{};
    uint32_t frame_counter_{0};
    uint32_t seconds_{0};

    bool logging_{false};
    std::unique_ptr<std::ofstream> log_file_{};
    std::string log_path_{};
    std::string log_error_{};

    ui::Labels labels_{
        {{0, 0}, "1090.000M", ui::Color::white()},
        {{10 * 8, 0}, "Gain", ui::Color::light_grey()},
        {{18 * 8, 0}, "Lvl", ui::Color::light_grey()},
    };

    ui::NumberField field_gain_{{15 * 8, 0}, 3, {0, 76}, 1, ' '};
    ui::VuMeter level_meter_{{21 * 8, 4, 6 * 8, 8}, 6, false};

    ui::ActivityDot dot_frame_{{28 * 8, 5, 2, 2}, ui::Color::grey()};
    ui::ActivityDot dot_good_frame_{{28 * 8, 9, 2, 2}, ui::Color::green()};

    ui::Checkbox check_log_{{0, 18}, 3, "Log", true};
    ui::Text text_status_{{7 * 8, 22, 23 * 8, 16}, ""};

    ui::RecentEntriesColumns columns_{
        {{"ICAO/Call", 0},
         {"Lvl", 3},
         {"Spd", 3},
         {"Amp", 3},
         {"Hit", 3},
         {"Age", 4}}};
    AircraftRecentEntries& recent_;
    ui::RecentEntriesView<AircraftRecentEntries> recent_view_;

    ui::Labels tap_note_{
        {{0, 272}, "Wideband tap only: ~12% of", ui::Color::grey()},
        {{0, 288}, "air time is seen. No radio.", ui::Color::grey()},
    };
};

}  // namespace app

#endif /*__MB200_UI_ADSB_RX_H__*/
