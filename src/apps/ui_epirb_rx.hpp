/*
 * mayhem-b200 — 406 MHz COSPAS-SARSAT distress beacon receiver (EPIRB/ELT/PLB).
 *
 * Ported from firmware/application/external/epirb_rx/ (beacon.hpp, location.*,
 * country.hpp, resources.hpp, ui_epirb_rx.*) and firmware/baseband/proc_epirb.*
 *
 * The 406 MHz burst is 160 ms of unmodulated carrier followed by a 144-bit
 * (long) or 112-bit (short) frame, biphase-L encoded at 400 bps and phase
 * modulated +/-1.1 rad, so the symbol rate on the wire is 800 baud.
 *
 * Frame layout (bit numbers are 1-based, as C/S T.001 numbers them, and as
 * Beacon::get_bits() takes them):
 *
 *      1 ..  15   bit sync, all ones
 *     16 ..  24   frame sync: 000101111 normal, 011010000 self-test
 *          25     format flag: 1 = long frame (144 bits), 0 = short (112)
 *          26     protocol flag: 1 = user protocol, 0 = location protocol
 *     27 ..  36   country code (maritime identification digits)
 *     37 ..  40   protocol code (37..39 when the protocol flag is set)
 *     41 ..  85   identification data (serial / MMSI / call sign / position)
 *     86 .. 106   BCH-1, the 21-bit check over bits 25..85
 *    107 .. 132   supplementary data (position offsets, locating devices)
 *    133 .. 144   BCH-2, the 12-bit check over bits 107..132
 *
 * BCH-1 generator: x^21+x^18+x^17+x^15+x^14+x^12+x^11+x^8+x^7+x^6+x^5+x+1
 * BCH-2 generator: x^12+x^10+x^8+x^5+x^4+x^3+1
 * Both verbatim from upstream's BCH_21_POLYNOMIAL / BCH_12_POLYNOMIAL.
 *
 * Three documented departures from upstream, all noted again at the point of
 * use in the .cpp:
 *
 *   1. Beacon::getBits() advances its byte pointer *after* consuming the last
 *      requested bit and dereferences it, so a read that ends exactly on the
 *      final frame bit (get_bits(133,144)) reads one byte past the buffer.
 *      Bounds-checked here.
 *   2. Beacon::getProtocolType() omits STD_EPIRB_SERIAL (protocol code 0b0110)
 *      from the STANDARD_LOCATION group, so a serialised EPIRB decodes with no
 *      protocol type and therefore no position — even though the same file's
 *      getType() calls it an EPIRB and parseAdditionalData() handles 0b0110 in
 *      the standard-location group. Added here.
 *   3. The country names and the label strings upstream loads from
 *      /EPIRB/RES/COUNTRY.RES and /EPIRB/RES/BEACON.RES on the SD card are
 *      compiled in, because there is no SD card. The data is the same.
 *
 * SAMPLE TAP — see the note in ui_epirb_rx.cpp. A 406 MHz burst lasts ~680 ms
 * and ReceiverModel's only tap is a block-based wideband one, so live capture
 * will drop bursts. The demodulator and the frame decoder are exercised
 * end-to-end on a synthesised burst in tests/test_epirb_rx.cpp.
 *
 * Copyright (C) 2024 EPIRB Receiver Implementation
 * Copyright (C) 2026 Frederic BORRY - ADRASEC 31 (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_EPIRB_RX_H__
#define __MB200_UI_EPIRB_RX_H__

#include "../dsp/demod.hpp"
#include "../dsp/fir.hpp"
#include "../dsp/protocol.hpp"
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
namespace epirb {

/* --- Frame constants ------------------------------------------------------ */

inline constexpr size_t kBeaconDataSize = 18;  /* 144 bits */
inline constexpr size_t kPreambleBits = 24;
inline constexpr size_t kLongFrameBits = 144;
inline constexpr size_t kShortFrameBits = 112;
inline constexpr uint32_t kRealPreamble = 0b1111'1111'1111'1110'0010'1111u;
inline constexpr uint32_t kTestPreamble = 0b1111'1111'1111'1110'1101'0000u;

inline constexpr uint32_t kBch21Polynomial = 0b1001101101100111100011u;
inline constexpr int kBch21PolyLength = 22;
inline constexpr uint32_t kBch12Polynomial = 0b1010100111001u;
inline constexpr int kBch12PolyLength = 13;

/* --- Country ------------------------------------------------------------- */

struct Country {
    int16_t code{0};
    char alpha_code[4]{};
    char short_name[12]{};
};

/* The maritime identification digits table from COUNTRY.RES. Fills `out` with
 * blank names when the code is not listed, as upstream does. */
void lookup_country(int code, Country& out);

/* --- Location ------------------------------------------------------------- */

class Location {
   public:
    class Angle {
       public:
        long degrees{0};
        long minutes{0};
        long seconds{0};
        bool orientation{false};  /* false = N/E, true = S/W */

        Angle() = default;
        explicit Angle(long d) : degrees{d} {}

        void clear();
        float float_value();
        /* Applies the PDF-2 offset with sign-based borrow into minutes and
         * degrees, exactly as upstream's apply_offset(). */
        void apply_offset(bool positive, long ofmin, long ofsec);

       private:
        float float_value_{255.0f};
    };

    enum class Format { Decimal, Sexagesimal, MaidenheadLocator };

    Angle latitude{127};
    Angle longitude{255};

    void clear();
    bool is_unknown() const;

    static std::string maidenhead(float lat, float lon, int precision = 6);
    std::string to_string(Format format, int precision = 6);
};

/* --- Beacon --------------------------------------------------------------- */

class Beacon {
   public:
    enum class FrameMode : uint8_t { Normal, SelfTest, Unknown };
    enum class MainLocatingDevice : uint8_t { Undefined, InternalNav, ExternalNav };
    enum class AuxLocatingDevice : uint8_t { Undefined, None, NoneOrOther, Other, Mhz121_5, Sart };

    enum class ProtocolType : uint8_t {
        Unknown, User, StandardLocation, NationalLocation, RlsLocation, EltDtLocation, Spare
    };

    enum class Protocol : uint8_t {
        UserEpirbMaritime, UserEpirbRadio, UserElt, UserSerial, UserTest, UserOrb, UserNat, User2G,
        StdEpirb, StdElt24, StdEltSerial, StdEltAircraft, StdEpirbSerial, StdPlbSerial, StdShip,
        StdTest, NatElt, NatEpirb, NatPlb, NatTest, Rls, EltDt, Spare, Unknown
    };

    void set_frame(const uint8_t* frame_buffer);
    /* Convenience for a decoded dsp::Packet of 112 or 144 symbol bits. */
    void set_frame_bits(const std::vector<uint8_t>& bits);

    /* Bits `start_bit`..`end_bit` inclusive, 1-based, MSB first. */
    uint64_t get_bits(int start_bit, int end_bit) const;
    void flip_bit(int bit);

    uint64_t compute_bch(int start_bit, int end_bit, uint32_t poly, int poly_length) const;
    uint64_t compute_bch1() const { return compute_bch(25, 85, kBch21Polynomial, kBch21PolyLength); }
    uint64_t compute_bch2() const { return compute_bch(107, 132, kBch12Polynomial, kBch12PolyLength); }

    bool protocol_is_user() const { return protocol_type == ProtocolType::User; }
    bool protocol_is_standard() const { return protocol_type == ProtocolType::StandardLocation; }
    bool protocol_is_national() const { return protocol_type == ProtocolType::NationalLocation; }
    bool protocol_is_rls() const { return protocol_type == ProtocolType::RlsLocation; }
    bool protocol_is_rls_or_elt() const {
        return protocol_type == ProtocolType::RlsLocation ||
               protocol_type == ProtocolType::EltDtLocation;
    }
    bool is_orbito() const { return protocol == Protocol::UserOrb; }

    const char* protocol_name() const;
    const char* type_name() const;
    const char* aux_locating_device_name() const;
    const char* main_locating_device_name() const;

    bool bch1_valid() const { return bch1 == computed_bch1; }
    bool bch2_valid() const { return bch2 == computed_bch2; }
    bool frame_valid() const { return bch1_valid() && ((!has_bch2) || bch2_valid()) && !is_empty; }

    std::string hex_string(bool with_header) const;
    std::string short_id() const;
    std::string status() const { return frame_valid() ? "OK" : "KO"; }
    std::string summary() const;

    /* --- Parsed fields --- */
    std::array<uint8_t, kBeaconDataSize> frame{};
    bool long_frame{true};
    bool protocol_flag{false};
    FrameMode frame_mode{FrameMode::Unknown};
    MainLocatingDevice main_locating_device{MainLocatingDevice::Undefined};
    AuxLocatingDevice aux_locating_device{AuxLocatingDevice::Undefined};
    long protocol_code{0};
    Protocol protocol{Protocol::Unknown};
    ProtocolType protocol_type{ProtocolType::Unknown};
    Country country{};
    Location location{};
    uint64_t identifier{0};
    bool has_additional_data{false};
    std::string additional_data{};
    bool has_serial_number{false};
    std::string serial_number{};
    std::string hex_id{};
    uint32_t bch1{0};
    uint32_t computed_bch1{0};
    bool bch1_corrected{false};
    bool has_bch2{false};
    uint32_t bch2{0};
    uint32_t computed_bch2{0};
    bool bch2_corrected{false};
    bool is_empty{true};
    bool has_emergency{false};
    bool is_automatic_emergency{false};
    bool is_maritime{false};
    std::string emergency_type{};

   private:
    ProtocolType protocol_type_for(Protocol p) const;
    void parse_frame();
    void parse_protocol();
    void parse_additional_data();
    void parse_locating_devices();
    void simple_correction(bool is_bch1);
    void set_serial_number(uint32_t serial);

    bool correcting_{false};
};

/* --- Packet builder (proc_epirb.hpp EPIRBPacketBuilder) ------------------- */

/* Detects both the normal and the self-test frame sync, prepends the 24 sync
 * bits to the packet, then takes the format bit to decide whether 144 or 112
 * bits are expected. */
class PacketBuilder {
   public:
    using Handler = std::function<void(const std::vector<uint8_t>& bits)>;

    void set_handler(Handler h) { handler_ = std::move(h); }
    void execute(uint_fast8_t symbol);
    void flush();
    void reset_state();

    size_t size() const { return packet_.size(); }

   private:
    enum class State : uint8_t { Preamble, Format, Payload };

    dsp::BitHistory bit_history_{};
    dsp::BitPattern real_sync_{kRealPreamble, kPreambleBits};
    dsp::BitPattern test_sync_{kTestPreamble, kPreambleBits};
    std::vector<uint8_t> packet_{};
    size_t size_{0};
    State state_{State::Preamble};
    Handler handler_{};
};

/* --- Demodulator (proc_epirb.cpp) ---------------------------------------- */

/* Carrier-lock, AFC, and biphase-L symbol timing over the complex baseband.
 *
 * The phase discriminator runs per sample; the per-sample deltas are summed
 * over a sliding window a fifth of a symbol long so a single-sample +/-2.2 rad
 * jump is visible as one accumulated step. Bit values come from the spacing
 * between direction-alternating jumps: one symbol means the bit repeats, two
 * symbols means it flips. */
class Demodulator {
   public:
    /* Upstream runs at 3.072 MHz / 8 / 8 = 48 kHz with an 800 baud symbol
     * rate, so 60 samples per symbol. */
    void configure(float sample_rate_hz = 48000.0f);
    void reset();

    void set_handler(PacketBuilder::Handler h) { builder_.set_handler(std::move(h)); }

    void process(const dsp::cfloat* in, size_t count);
    void process_sample(dsp::cfloat sample);

    enum class State : uint8_t { Idle, CarrierLocked, DataSync, PostFrame };
    State state() const { return state_; }
    float frequency_offset_estimate() const { return freq_offset_est_; }
    size_t samples_per_symbol() const { return samples_per_symbol_; }

   private:
    static float phase_diff(dsp::cfloat a, dsp::cfloat b);
    bool filtered_rise_detect(bool condition);
    void frame_end();
    void data_sync(float phase_delta);

    float sample_rate_{48000.0f};
    size_t samples_per_symbol_{60};
    size_t samples_per_bit_{120};
    size_t samples_margin_{20};
    size_t rise_filter_samples_{3};
    size_t carrier_samples_threshold_{3840};
    size_t carrier_max_samples_{43200};
    size_t frame_max_samples_{19008};

    dsp::cfloat last_sample_{0.0f, 0.0f};
    std::vector<float> phase_delta_buffer_{};
    size_t phase_delta_index_{0};
    float phase_delta_acc_{0.0f};

    float freq_offset_est_{0.0f};
    float afc_mean_{0.0f};
    float afc_m2_{0.0f};
    uint32_t afc_convergence_n_{0};
    float carrier_phase_sum_{0.0f};
    uint32_t carrier_phase_n_{0};

    State state_{State::Idle};
    uint32_t stability_counter_{0};
    uint16_t sample_count_{0};
    uint32_t frame_sample_count_{0};
    uint16_t rise_detection_count_{0};
    bool last_phase_positive_{false};
    bool last_bit_{false};

    PacketBuilder builder_{};
};

}  // namespace epirb

/* --- View ----------------------------------------------------------------- */

struct EpirbRecentEntry {
    using Key = std::string;
    static const Key invalid_key;

    EpirbRecentEntry() = default;
    explicit EpirbRecentEntry(Key k) : hex_id{std::move(k)} {}

    Key hex_id{};
    uint32_t count{0};
    std::string line{};

    const Key& key() const { return hex_id; }
};

using EpirbRecentEntries = ui::RecentEntries<EpirbRecentEntry>;

class EpirbRxView : public ui::View {
   public:
    EpirbRxView();
    ~EpirbRxView() override;

    EpirbRxView(const EpirbRxView&) = delete;
    EpirbRxView& operator=(const EpirbRxView&) = delete;

    std::string title() const override { return "EPIRB RX"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void rebuild_front_end();
    void on_frame_bits(const std::vector<uint8_t>& bits);

    radio::ReceiverModel& receiver_;

    bool running_{false};
    uint32_t frames_valid_{0};
    uint32_t frames_corrected_{0};
    uint32_t frames_error_{0};

    double configured_input_rate_{0.0};
    dsp::FirDecimateC channel_filter_{};
    std::vector<dsp::cfloat> samples_{};
    std::vector<dsp::cfloat> channel_{};

    epirb::Demodulator demod_{};
    epirb::Beacon beacon_{};
    EpirbRecentEntries entries_{};

    ui::Labels labels_{
        {{0, 2}, "Freq", ui::Color::light_grey()},
    };

    ui::FrequencyField field_frequency_{{40, 2}};
    ui::FrequencyStepView step_view_{{132, 2}, field_frequency_};

    ui::Button button_startstop_{{192, 20, 44, 20}, "Start"};

    ui::Text text_status_{{0, 24, 184, 16}, ""};
    ui::Text text_detail1_{{0, 44, 240, 16}, ""};
    ui::Text text_detail2_{{0, 60, 240, 16}, ""};

    ui::RecentEntriesColumns columns_{{{"Beacon", 0}}};
    ui::RecentEntriesTable<EpirbRecentEntries> table_{entries_, columns_};

    ui::Labels notes_{
        {{0, 252}, "Tap: wideband spectrum block", ui::Color::grey()},
        {{0, 268}, "(no channel tap; see .cpp).", ui::Color::grey()},
        {{0, 288}, "Untested on air - no radio.", ui::Color::yellow()},
    };
};

}  // namespace app

#endif /*__MB200_UI_EPIRB_RX_H__*/
