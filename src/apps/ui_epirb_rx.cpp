/*
 * mayhem-b200 — 406 MHz COSPAS-SARSAT distress beacon receiver.
 *
 * Copyright (C) 2024 EPIRB Receiver Implementation
 * Copyright (C) 2026 Frederic BORRY - ADRASEC 31 (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "ui_epirb_rx.hpp"

#include "../core/string_format.hpp"
#include "../radio/receiver_model.hpp"
#include "app_context.hpp"
#include "ui_painter.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

namespace app {
namespace epirb {

/* --- Resource strings ----------------------------------------------------- */

/* /EPIRB/RES/BEACON.RES, compiled in. Indices are the zero-based line numbers
 * upstream's ResourceManager::get_beacon_resource() takes, including the "#"
 * comment lines it counts but never asks for. */
namespace {

constexpr const char* kUnknownLabel = "Unk.";

constexpr size_t kAdditionalDataResourceStart = 25;
constexpr size_t kLongAdditionalDataResourceStart = 33;
constexpr size_t kEmergencyResourceStart = 39;
constexpr size_t kEmergencyOtherResourceStart = 49;
constexpr size_t kAuxDeviceResourceStart = 53;

const char* const kBeaconResources[] = {
    "EPIRB - Maritime",
    "EPIRB - Radio Call Sign",
    "ELT - Aviation",
    "Serial User Protocol",
    "Test User Protocol",
    "Orbitography Protocol",
    "National User Protocol",
    "2nd Generation Beacons",
    "EPIRB - MMSI / Location",
    "ELT-24-bit Address / Location",
    "ELT Serial Location",
    "ELT Serial Aircraft Location",
    "EPIRB Serial Location",
    "PLB Serial Location",
    "Ship Security Location",
    "Test Standard Location",
    "ELT National Location",
    "EPIRB National Location",
    "PLB National Location",
    "Test National Location",
    "RLS Location Protocol",
    "ELT(DT) Location Protocol",
    "Spare",
    "Unknown",
    "# L. 25: additional data",
    "ELTs/SN",
    "ELTs/AC op & SN",
    "FF EPIRBs/SN",
    "ELTs+AC 24-bit ad.",
    "NFF EPIRBs/SN",
    "Not Used",
    "PLBs/SN",
    "# L. 33: long additinal data",
    "MMSI=0x%08lX MID=%ld",
    "24 bits AC ad.",
    "C/S TA #=%ld",
    "Op Design.=%c%c%c",
    "Nati. data=%ld",
    "# L. 39: emmergency",
    "Other",
    "Fire",
    "Flooding",
    "Collision",
    "Grounding",
    "Capsizing",
    "Sinking",
    "Disabled",
    "Abandoning",
    "# L. 49: other emmergency",
    "Fire",
    "Med.",
    "Dis.",
    "# L. 53: ",
    "Undefined",
    "No device",
    "Other/no device",
    "Other device",
    "121.5 MHz",
    "SART",
};

const char* beacon_resource(size_t line) {
    constexpr size_t count = sizeof(kBeaconResources) / sizeof(kBeaconResources[0]);
    return (line < count) ? kBeaconResources[line] : kUnknownLabel;
}

/* Baudot code matrix, verbatim from beacon.hpp. */
constexpr char kBaudotCode[64] = {
    ' ', '5', ' ', '9', ' ', ' ', ' ', ' ', ' ', ' ', '4', ' ', '8', '0', ' ', ' ',
    '3', ' ', ' ', ' ', ' ', '6', ' ', '/', '-', '2', ' ', ' ', '7', '1', ' ', ' ',
    ' ', 'T', ' ', 'O', ' ', 'H', 'N', 'M', ' ', 'L', 'R', 'G', 'I', 'P', 'C', 'V',
    'E', 'Z', 'D', 'B', 'S', 'Y', 'F', 'X', 'A', 'W', 'J', ' ', 'U', 'Q', 'K', '\0'};

struct CountryRow {
    int16_t code;
    const char* alpha;
    const char* name;
};

/* /EPIRB/RES/COUNTRY.RES, compiled in. */
const CountryRow kCountries[] = {
    {501, "ADE", "AdelieLand"}, {201, "ALB", "Albania"},
    {202, "AND", "Andorra"}, {203, "AUT", "Austria"},
    {204, "AZC", "Azores"}, {205, "BEL", "Belgium"},
    {206, "BLR", "Belarus"}, {207, "BUL", "Bulgaria"},
    {208, "VAT", "Vatican"}, {209, "CYP", "Cyprus"},
    {210, "CYP", "Cyprus"}, {211, "GER", "Germany"},
    {212, "CYP", "Cyprus"}, {213, "GOG", "Georgia"},
    {214, "MOL", "Moldova"}, {215, "MAL", "MALTA"},
    {216, "ARM", "Armenia"}, {218, "GER", "Germany"},
    {219, "DEN", "Denmark"}, {220, "DEN", "Denmark"},
    {224, "SPA", "Spain"}, {225, "SPA", "Spain"},
    {226, "FRA", "France"}, {227, "FRA", "France"},
    {228, "FRA", "France"}, {229, "MAL", "Malta"},
    {230, "FIN", "Finland"}, {231, "FAR", "Faro Isle"},
    {232, "UKM", "G Britain"}, {233, "UKM", "G Britain"},
    {234, "UKM", "G Britain"}, {235, "UKM", "G Britain"},
    {236, "GIB", "Gibraltar"}, {237, "GRE", "Greece"},
    {238, "CRT", "Croatia"}, {239, "GRE", "Greece"},
    {240, "GRE", "Greece"}, {241, "GRE", "Greece"},
    {242, "MOR", "Morocco"}, {243, "HUN", "Hungary"},
    {244, "NET", "Netherland"}, {245, "NET", "Netherland"},
    {246, "NET", "Netherland"}, {247, "ITA", "Italy"},
    {248, "MAL", "Malta"}, {249, "MAL", "Malta"},
    {250, "IRE", "Ireland"}, {251, "ICE", "Iceland"},
    {252, "LIE", "Liechten"}, {253, "LUX", "Luxembourg"},
    {254, "MON", "Monaco"}, {255, "MAE", "Madeira"},
    {256, "MAL", "Malta"}, {257, "NOR", "Norway"},
    {258, "NOR", "Norway"}, {259, "NOR", "Norway"},
    {261, "POL", "Poland"}, {262, "MNT", "Montenegro"},
    {263, "POR", "Portugal"}, {264, "ROM", "Romania"},
    {265, "SWE", "Sweden"}, {266, "SWE", "Sweden"},
    {267, "SLV", "Slovakia"}, {268, "SAN", "San Marino"},
    {269, "SWT", "Swiss"}, {270, "CZH", "Czech Rep"},
    {271, "TUR", "Turkiye"}, {272, "UKR", "Ukraine"},
    {273, "RUS", "Russia"}, {274, "MKD", "North Mac"},
    {275, "LAT", "Latvia"}, {276, "EST", "Estonia"},
    {277, "LIT", "Lithuania"}, {278, "SVN", "Slovenia"},
    {279, "SER", "Serbia"}, {301, "ANA", "Anguilla"},
    {303, "ALA", "Alaska"}, {304, "ANT", "Antigua"},
    {305, "ANT", "Antigua"}, {306, "NEA", "N Antilles"},
    {307, "ARU", "Aruba"}, {308, "BAA", "Bahamas"},
    {309, "BAA", "Bahamas"}, {310, "BER", "Bermuda"},
    {311, "BAA", "Bahamas"}, {312, "BEZ", "Belize"},
    {314, "BAR", "Barbados"}, {316, "CAN", "Canada"},
    {319, "CAY", "Cayman Is"}, {321, "COS", "Costa Rica"},
    {323, "CUB", "Cuba"}, {325, "DOM", "Dominica"},
    {327, "DOR", "Dominican"}, {329, "GUA", "Guadeloupe"},
    {330, "GRA", "Grenada"}, {331, "GRN", "Greenland"},
    {332, "GUT", "Guatemala"}, {334, "HON", "Honduras"},
    {336, "HAI", "Haiti"}, {338, "USA", "USA"},
    {339, "JAM", "Jamaica"}, {341, "SKN", "St Kitts"},
    {343, "SLU", "St Lucia"}, {345, "MEX", "Mexico"},
    {347, "MTQ", "Martinique"}, {348, "MOT", "Montserrat"},
    {350, "NIC", "Nicaragua"}, {351, "PAN", "Panama"},
    {352, "PAN", "Panama"}, {353, "PAN", "Panama"},
    {354, "PAN", "Panama"}, {355, "PAN", "Panama"},
    {356, "PAN", "Panama"}, {357, "PAN", "Panama"},
    {358, "PUE", "PuertoRico"}, {359, "ELS", "ElSalvador"},
    {361, "SPI", "St Pierre"}, {362, "TAT", "Trinidad"},
    {364, "TUK", "Caicos Is"}, {366, "USA", "USA"},
    {367, "USA", "USA"}, {368, "USA", "USA"},
    {369, "USA", "USA"}, {370, "PAN", "Panama"},
    {371, "PAN", "Panama"}, {372, "PAN", "Panama"},
    {373, "PAN", "Panama"}, {374, "PAN", "Panama"},
    {375, "SVG", "St Vincent"}, {376, "SVG", "St Vincent"},
    {377, "SVG", "St Vincent"}, {378, "BVI", "Virgin GB"},
    {379, "USV", "Virgin US"}, {401, "AFG", "Afghan"},
    {403, "SAU", "Saudi"}, {405, "BAN", "Bangladesh"},
    {408, "BAH", "Bahrain"}, {410, "BHU", "Bhutan"},
    {412, "CHN", "China"}, {413, "CHN", "China"},
    {414, "CHN", "CHINA"}, {416, "TAI", "Taipei"},
    {417, "SRI", "Sri Lanka"}, {419, "IND", "India"},
    {422, "IRN", "Iran"}, {423, "AZR", "Azerbaijan"},
    {425, "IRQ", "Iraq"}, {428, "ISR", "Israel"},
    {431, "JPN", "Japan"}, {432, "JPN", "Japan"},
    {434, "TKM", "Turkmenist"}, {436, "KAZ", "Kazakhstan"},
    {437, "UZB", "Uzbekistan"}, {438, "JOR", "Jordan"},
    {440, "KOR", "Korea Sou"}, {441, "KOR", "Korea Sou"},
    {443, "PAA", "Palestine"}, {445, "KDR", "Korea Nor"},
    {447, "KUW", "Kuwait"}, {450, "LEB", "Lebanon"},
    {451, "KYR", "Kyrgyzia"}, {453, "MAC", "Macao"},
    {455, "MAV", "Maldives"}, {457, "MNG", "Mongolia"},
    {459, "NEP", "Nepal"}, {461, "OMN", "Oman"},
    {463, "PAK", "Pakistan"}, {466, "QAT", "Qatar"},
    {468, "SYR", "Syria"}, {470, "UAE", "UAE"},
    {471, "UAE", "UAE"}, {472, "TJK", "TAJIKISTAN"},
    {473, "YEM", "Yemen"}, {475, "YEM", "Yemen"},
    {477, "HKG", "Hong Kong"}, {478, "BOS", "Bosniaherz"},
    {503, "AUS", "Australia"}, {506, "BUR", "Burma"},
    {508, "BRU", "Brunei"}, {510, "MIC", "Micronesia"},
    {511, "PAL", "Palau"}, {512, "NZL", "NewZealand"},
    {514, "CMB", "Cambodia"}, {515, "CMB", "Cambodia"},
    {516, "CHR", "Christmas"}, {518, "COO", "Cook Isles"},
    {520, "FIJ", "Fiji"}, {523, "COC", "Cocos Isle"},
    {525, "INO", "Indonesia"}, {529, "KIR", "Kiribati"},
    {531, "LAO", "Lao"}, {533, "MLY", "Malaysia"},
    {536, "MAI", "Mariana Is"}, {538, "MAR", "Marshall I"},
    {540, "NCA", "Caledonia"}, {542, "NIU", "Niue Isle"},
    {544, "NAU", "Nauru"}, {546, "PLY", "Polynesia"},
    {548, "PHI", "Philippine"}, {550, "TIM", "TimorLeste"},
    {553, "PAP", "Papua NG"}, {555, "PIT", "Pitcairn I"},
    {557, "SOL", "Solomon Is"}, {559, "ASA", "Samoa USA"},
    {561, "WSA", "West Samoa"}, {563, "SIN", "Singapore"},
    {564, "SIN", "Singapore"}, {565, "SIN", "Singapore"},
    {566, "SIN", "SINGAPORE"}, {567, "THA", "Thailand"},
    {570, "TON", "Tonga"}, {572, "TUV", "Tuvalu Is"},
    {574, "VIE", "Vietnam"}, {576, "VAN", "Vanuatu"},
    {577, "VAN", "Vanuatu"}, {578, "WAL", "Wallis Is"},
    {601, "SAF", "So Africa"}, {603, "ANG", "Angola"},
    {605, "ALG", "Algeria"}, {607, "SPL", "St Paul"},
    {608, "ASC", "Ascension"}, {609, "BUI", "Burundi"},
    {610, "BEN", "Benin"}, {611, "BOT", "Botswana"},
    {612, "CAR", "CenAfr Rep"}, {613, "CAM", "Cameroon"},
    {615, "CON", "Congo"}, {616, "COM", "Comoros"},
    {617, "CAP", "Cape Verde"}, {618, "CRP", "Crozet"},
    {619, "IVO", "IvoryCoast"}, {620, "COM", "Comoros"},
    {621, "DJI", "Djibouti"}, {622, "EGY", "Egypt"},
    {624, "ETH", "Ethiopia"}, {625, "ERT", "Eritrea"},
    {626, "GAB", "Gabon Rep"}, {627, "GHA", "Ghana"},
    {629, "GAM", "Gambia"}, {630, "GUB", "Guinea Bis"},
    {631, "EQG", "Eq Guinea"}, {632, "GUN", "Guinea Rep"},
    {633, "BUF", "Burkina FS"}, {634, "KEN", "Kenya"},
    {635, "KER", "Kerguelen"}, {636, "LIB", "Liberia"},
    {637, "LIB", "Liberia"}, {638, "SSD", "Southsudan"},
    {642, "LBY", "Libya"}, {644, "LES", "Lesotho"},
    {645, "MAU", "Mauritius"}, {647, "MAD", "Madagascar"},
    {649, "MLI", "Mali"}, {650, "MOZ", "Mozambique"},
    {654, "MAA", "Mauritania"}, {655, "MAW", "Malawi"},
    {656, "NIG", "Niger"}, {657, "NIA", "Nigeria"},
    {659, "NAM", "Namibia"}, {660, "REU", "Reunion"},
    {661, "RWA", "Rwanda"}, {662, "SUD", "Sudan"},
    {663, "SEN", "Senegal"}, {664, "SEY", "Seychelle"},
    {665, "SHE", "St Helena"}, {666, "SOM", "Somali"},
    {667, "SIL", "Sierra Leo"}, {668, "SAO", "Sao Tome"},
    {669, "SWZ", "Eswatini"}, {670, "CHA", "Chad"},
    {671, "TOG", "Togo"}, {672, "TUN", "Tunisia"},
    {674, "TAN", "Tanzania"}, {675, "UGA", "Uganda"},
    {676, "ZAI", "Zaire"}, {677, "TAN", "Tanzania"},
    {678, "ZAM", "Zambia"}, {679, "ZIM", "Zimbabwe"},
    {701, "ARG", "Argentina"}, {710, "BRA", "Brazil"},
    {720, "BOL", "Bolivia"}, {725, "CHI", "Chile"},
    {730, "COL", "Colombia"}, {735, "ECU", "Ecuador"},
    {740, "FAL", "Falkland I"}, {745, "GUI", "Guiana"},
    {750, "GUY", "Guyana"}, {755, "PAR", "Paraguay"},
    {760, "PER", "Peru"}, {765, "SUR", "Suriname"},
    {770, "URU", "Uruguay"}, {775, "VEN", "Venezuela"},
};

}  // namespace

void lookup_country(const int code, Country& out) {
    out.code = static_cast<int16_t>(code);
    out.alpha_code[0] = '\0';
    out.short_name[0] = '\0';

    constexpr size_t count = sizeof(kCountries) / sizeof(kCountries[0]);
    for (size_t i = 0; i < count; i++) {
        if (kCountries[i].code != code) continue;
        std::snprintf(out.alpha_code, sizeof(out.alpha_code), "%s", kCountries[i].alpha);
        std::snprintf(out.short_name, sizeof(out.short_name), "%s", kCountries[i].name);
        return;
    }
}

/* --- Location ------------------------------------------------------------- */

void Location::Angle::clear() {
    degrees = 255;
    minutes = 0;
    seconds = 0;
    orientation = false;
    float_value_ = 255.0f;
}

float Location::Angle::float_value() {
    if (float_value_ >= 255.0f) {
        float_value_ = static_cast<float>(degrees);
        float_value_ += static_cast<float>(minutes) / 60.0f;
        float_value_ += static_cast<float>(seconds) / 3600.0f;
        if (orientation) float_value_ = -float_value_;
    }
    return float_value_;
}

void Location::Angle::apply_offset(const bool positive, const long ofmin, const long ofsec) {
    if (positive) {
        minutes += ofmin;
        seconds += ofsec;
    } else {
        minutes -= ofmin;
        if (minutes < 0) {
            minutes += 60;
            degrees -= 1;
        }
        seconds -= ofsec;
        if (seconds < 0) {
            seconds += 60;
            minutes -= 1;
        }
    }
}

void Location::clear() {
    latitude.clear();
    longitude.clear();
}

bool Location::is_unknown() const {
    return latitude.degrees >= 255 || longitude.degrees >= 255;
}

std::string Location::maidenhead(float lat, float lon, const int precision) {
    std::string out;

    lon += 180.0f;
    lat += 90.0f;

    const int A = static_cast<int>(lon / 20.0f);
    const int B = static_cast<int>(lat / 10.0f);
    lon -= A * 20.0f;
    lat -= B * 10.0f;

    const int C = static_cast<int>(lon / 2.0f);
    const int D = static_cast<int>(lat / 1.0f);
    lon -= C * 2.0f;
    lat -= D * 1.0f;

    const int E = static_cast<int>(lon / (5.0f / 60.0f));
    const int F = static_cast<int>(lat / (2.5f / 60.0f));
    lon -= E * (5.0f / 60.0f);
    lat -= F * (2.5f / 60.0f);

    const int G = static_cast<int>(lon / (5.0f / 600.0f));
    const int H = static_cast<int>(lat / (2.5f / 600.0f));

    out += static_cast<char>('A' + A);
    out += static_cast<char>('A' + B);
    if (precision >= 4) {
        out += static_cast<char>('0' + C);
        out += static_cast<char>('0' + D);
    }
    if (precision >= 6) {
        out += static_cast<char>('a' + E);
        out += static_cast<char>('a' + F);
    }
    if (precision >= 8) {
        out += static_cast<char>('0' + G);
        out += static_cast<char>('0' + H);
    }
    return out;
}

std::string Location::to_string(const Format format, const int precision) {
    if (is_unknown()) return "GPS not synchronized";

    char buf[64];
    switch (format) {
        case Format::Sexagesimal:
            /* 0xB0 is the degree sign in the 8x16 font. */
            std::snprintf(buf, sizeof(buf), "%ld\xB0%02ld'%02ld\"%c, %ld\xB0%02ld'%02ld\"%c",
                          latitude.degrees, latitude.minutes, latitude.seconds,
                          latitude.orientation ? 'S' : 'N',
                          longitude.degrees, longitude.minutes, longitude.seconds,
                          longitude.orientation ? 'W' : 'E');
            return buf;

        case Format::MaidenheadLocator:
            return maidenhead(latitude.float_value(), longitude.float_value(), precision);

        case Format::Decimal:
        default:
            return to_string_decimal(latitude.float_value(), 5) + ", " +
                   to_string_decimal(longitude.float_value(), 5);
    }
}

/* --- Beacon --------------------------------------------------------------- */

uint64_t Beacon::get_bits(int start_bit, const int end_bit) const {
    uint64_t result = 0;
    start_bit--;
    if (start_bit < 0) return 0;
    const int num_bits = end_bit - start_bit;
    if (num_bits <= 0) return 0;

    size_t byte_index = static_cast<size_t>(start_bit) / 8;
    if (byte_index >= frame.size()) return 0;
    uint8_t b = frame[byte_index];
    int bit_offset = 7 - (start_bit % 8);

    for (int i = 0; i < num_bits; ++i) {
        result <<= 1;
        result |= static_cast<uint64_t>((b >> bit_offset) & 0x01);
        if (--bit_offset < 0) {
            /* Upstream advances and dereferences unconditionally, reading one
             * byte past the frame when the read ends on the final bit. */
            ++byte_index;
            b = (byte_index < frame.size()) ? frame[byte_index] : uint8_t{0};
            bit_offset = 7;
        }
    }
    return result;
}

void Beacon::flip_bit(const int bit) {
    const size_t byte_idx = static_cast<size_t>(bit - 1) / 8;
    const int bit_idx = 7 - ((bit - 1) % 8);
    if (byte_idx < frame.size()) frame[byte_idx] ^= static_cast<uint8_t>(1 << bit_idx);
}

uint64_t Beacon::compute_bch(const int start_bit, const int end_bit,
                             const uint32_t poly, const int poly_length) const {
    const int data_length = end_bit - start_bit + 1;
    const int total_length = data_length + poly_length - 1;
    uint64_t result = get_bits(start_bit, start_bit + poly_length - 1);
    for (int i = poly_length; i <= total_length; i++) {
        const bool first_bit = (result >> (poly_length - 1)) != 0;
        if (first_bit) result = result ^ poly;
        if (i < total_length) {
            result = result << 1;
            if (i < data_length) result |= get_bits(start_bit + i, start_bit + i);
        }
    }
    return result;
}

void Beacon::set_serial_number(const uint32_t serial) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%lu (0x%08lX)",
                  static_cast<unsigned long>(serial), static_cast<unsigned long>(serial));
    serial_number = buf;
}

void Beacon::set_frame(const uint8_t* frame_buffer) {
    frame_mode = FrameMode::Unknown;
    main_locating_device = MainLocatingDevice::Undefined;
    aux_locating_device = AuxLocatingDevice::Undefined;
    protocol_code = 0;
    protocol = Protocol::Unknown;
    protocol_type = ProtocolType::Unknown;
    country = Country{};
    location = Location{};
    identifier = 0;
    has_additional_data = false;
    additional_data.clear();
    has_serial_number = false;
    serial_number.clear();
    hex_id.clear();
    bch1 = 0;
    computed_bch1 = 0;
    has_bch2 = false;
    bch1_corrected = false;
    bch2 = 0;
    computed_bch2 = 0;
    bch2_corrected = false;
    is_empty = true;
    has_emergency = false;
    is_automatic_emergency = false;
    is_maritime = false;
    emergency_type.clear();
    correcting_ = false;

    std::memcpy(frame.data(), frame_buffer, kBeaconDataSize);
    parse_frame();
}

void Beacon::set_frame_bits(const std::vector<uint8_t>& bits) {
    uint8_t data[kBeaconDataSize]{};
    const size_t max_bytes = std::min(bits.size() / 8, kBeaconDataSize);
    size_t bit_index = 0;
    for (size_t i = 0; i < max_bytes; i++) {
        uint8_t value = 0;
        for (int bit = 0; bit < 8; bit++, bit_index++) {
            if (bits[bit_index]) value = static_cast<uint8_t>(value | (1u << (7 - bit)));
        }
        data[i] = value;
    }
    set_frame(data);
}

Beacon::ProtocolType Beacon::protocol_type_for(const Protocol p) const {
    switch (p) {
        case Protocol::UserEpirbMaritime:
        case Protocol::UserEpirbRadio:
        case Protocol::UserElt:
        case Protocol::UserSerial:
        case Protocol::UserTest:
        case Protocol::UserOrb:
        case Protocol::UserNat:
        case Protocol::User2G:
            return ProtocolType::User;
        case Protocol::StdEpirb:
        case Protocol::StdElt24:
        case Protocol::StdEltSerial:
        case Protocol::StdEltAircraft:
        /* Upstream omits StdEpirbSerial here, which drops the position of every
         * serialised EPIRB. It is a standard-location protocol: its own
         * type_name() reports "EPIRB" and parse_additional_data() handles code
         * 0b0110 alongside the other standard-location codes. */
        case Protocol::StdEpirbSerial:
        case Protocol::StdPlbSerial:
        case Protocol::StdShip:
        case Protocol::StdTest:
            return ProtocolType::StandardLocation;
        case Protocol::NatElt:
        case Protocol::NatEpirb:
        case Protocol::NatPlb:
        case Protocol::NatTest:
            return ProtocolType::NationalLocation;
        case Protocol::Rls:
            return ProtocolType::RlsLocation;
        case Protocol::EltDt:
            return ProtocolType::EltDtLocation;
        case Protocol::Spare:
            return ProtocolType::Spare;
        case Protocol::Unknown:
        default:
            return ProtocolType::Unknown;
    }
}

const char* Beacon::protocol_name() const {
    switch (protocol_type) {
        case ProtocolType::StandardLocation: return "Standard";
        case ProtocolType::NationalLocation: return "National";
        case ProtocolType::RlsLocation: return "RLS";
        case ProtocolType::EltDtLocation: return "ELT(DT)";
        case ProtocolType::User: return long_frame ? "User Location" : "User";
        case ProtocolType::Spare: return "Spare";
        default: return kUnknownLabel;
    }
}

const char* Beacon::type_name() const {
    if (protocol == Protocol::UserEpirbMaritime || protocol == Protocol::UserEpirbRadio ||
        protocol == Protocol::StdEpirb || protocol == Protocol::StdEpirbSerial ||
        protocol == Protocol::NatEpirb)
        return "EPIRB";
    if (protocol == Protocol::UserElt || protocol == Protocol::StdElt24 ||
        protocol == Protocol::StdEltSerial || protocol == Protocol::StdEltAircraft ||
        protocol == Protocol::NatElt || protocol == Protocol::EltDt)
        return "ELT";
    if (protocol == Protocol::StdPlbSerial || protocol == Protocol::NatPlb) return "PLB";
    if (protocol == Protocol::UserTest || protocol == Protocol::StdTest ||
        protocol == Protocol::NatTest)
        return "TEST";
    if (protocol == Protocol::UserSerial) return "SRIAL";
    if (protocol == Protocol::UserOrb) return "ORB";
    if (protocol == Protocol::UserNat) return "NAT";
    if (protocol == Protocol::User2G) return "2G";
    if (protocol == Protocol::StdShip) return "SHIP";
    if (protocol == Protocol::Rls) return "RLS";
    if (protocol == Protocol::Spare) return "SPARE";
    return kUnknownLabel;
}

const char* Beacon::main_locating_device_name() const {
    if (main_locating_device == MainLocatingDevice::ExternalNav) return "Exernal";
    if (main_locating_device == MainLocatingDevice::InternalNav) return "Internal";
    return kUnknownLabel;
}

const char* Beacon::aux_locating_device_name() const {
    return beacon_resource(kAuxDeviceResourceStart + static_cast<size_t>(aux_locating_device));
}

void Beacon::parse_protocol() {
    protocol_flag = get_bits(26, 26) != 0;
    if (protocol_flag)
        protocol_code = static_cast<long>(get_bits(37, 39));
    else
        protocol_code = static_cast<long>(get_bits(37, 40));

    if (!long_frame || protocol_flag) {
        switch (protocol_code) {
            case 0b000: protocol = Protocol::UserOrb; break;
            case 0b001: protocol = Protocol::UserElt; break;
            case 0b010: protocol = Protocol::UserEpirbMaritime; is_maritime = true; break;
            case 0b011: protocol = Protocol::UserSerial; break;
            case 0b100: protocol = Protocol::UserNat; break;
            case 0b101: protocol = Protocol::User2G; break;
            case 0b110: protocol = Protocol::UserEpirbRadio; is_maritime = true; break;
            case 0b111: protocol = Protocol::UserTest; break;
            default: protocol = Protocol::Unknown; break;
        }
    } else {
        switch (protocol_code) {
            case 0b0010: protocol = Protocol::StdEpirb; break;
            case 0b0011: protocol = Protocol::StdElt24; break;
            case 0b0100: protocol = Protocol::StdEltSerial; break;
            case 0b0101: protocol = Protocol::StdEltAircraft; break;
            case 0b0110: protocol = Protocol::StdEpirbSerial; break;
            case 0b0111: protocol = Protocol::StdPlbSerial; break;
            case 0b1000: protocol = Protocol::NatElt; break;
            case 0b1001: protocol = Protocol::EltDt; break;
            case 0b1010: protocol = Protocol::NatEpirb; break;
            case 0b1011: protocol = Protocol::NatPlb; break;
            case 0b1100: protocol = Protocol::StdShip; break;
            case 0b1101: protocol = Protocol::Rls; break;
            case 0b1110: protocol = Protocol::StdTest; break;
            case 0b1111: protocol = Protocol::NatTest; break;
            default: protocol = Protocol::Unknown; break;
        }
    }
}

void Beacon::parse_additional_data() {
    has_additional_data = false;
    has_serial_number = false;
    char buf[64];

    if (protocol_flag) {
        if (protocol_code == 0b011) {
            const uint8_t serial_user_protocol = static_cast<uint8_t>(get_bits(40, 42));
            has_additional_data = true;
            switch (serial_user_protocol) {
                case 0b100:
                case 0b010:
                    is_maritime = true;
                    [[fallthrough]];
                case 0b000:
                case 0b001:
                case 0b011:
                case 0b110:
                    additional_data = beacon_resource(kAdditionalDataResourceStart +
                                                      serial_user_protocol);
                    break;
                default:
                    has_additional_data = false;
                    break;
            }
            has_serial_number = true;
            set_serial_number(static_cast<uint32_t>(
                (serial_user_protocol == 0b011)   ? get_bits(44, 67)
                : (serial_user_protocol == 0b001) ? get_bits(62, 73)
                                                  : get_bits(44, 63)));
        }
        if (!long_frame) {
            has_emergency = get_bits(107, 107) != 0;
            if (has_emergency) {
                is_automatic_emergency = get_bits(108, 108) != 0;
                if (is_maritime) {
                    const uint8_t emergency = static_cast<uint8_t>(get_bits(109, 112));
                    if (emergency >= 0b0001 && emergency <= 0b1000)
                        emergency_type = beacon_resource(kEmergencyResourceStart + emergency);
                    else
                        emergency_type = beacon_resource(kEmergencyResourceStart);
                } else {
                    const bool fire = get_bits(109, 109) != 0;
                    const bool med = get_bits(110, 110) != 0;
                    const bool disabled = get_bits(111, 111) != 0;
                    emergency_type.clear();
                    if (fire) emergency_type += beacon_resource(kEmergencyOtherResourceStart);
                    if (med) {
                        if (fire) emergency_type += '+';
                        emergency_type += beacon_resource(kEmergencyOtherResourceStart + 1);
                    }
                    if (disabled) {
                        if (fire || med) emergency_type += '+';
                        emergency_type += beacon_resource(kEmergencyOtherResourceStart + 2);
                    }
                }
            }
        }
    } else if (long_frame) {
        switch (protocol_code) {
            case 0b0010:
                has_serial_number = true;
                set_serial_number(static_cast<uint32_t>(get_bits(61, 64)));
                break;
            case 0b1100: {
                has_additional_data = true;
                const auto mmsi = static_cast<unsigned long>(get_bits(41, 60));
                std::snprintf(buf, sizeof(buf),
                              beacon_resource(kLongAdditionalDataResourceStart), mmsi, mmsi);
                additional_data = buf;
            } break;
            case 0b0011:
                has_additional_data = true;
                has_serial_number = true;
                set_serial_number(static_cast<uint32_t>(get_bits(41, 64)));
                additional_data = beacon_resource(kLongAdditionalDataResourceStart + 1);
                break;
            case 0b0100:
            case 0b0110:
            case 0b0111: {
                has_additional_data = true;
                has_serial_number = true;
                const auto cs_ta_number = static_cast<unsigned long>(get_bits(41, 50));
                std::snprintf(buf, sizeof(buf),
                              beacon_resource(kLongAdditionalDataResourceStart + 2), cs_ta_number);
                additional_data = buf;
                set_serial_number(static_cast<uint32_t>(get_bits(51, 64)));
            } break;
            case 0b0101: {
                has_additional_data = true;
                has_serial_number = true;
                const char char1 = kBaudotCode[get_bits(41, 45) + 32];
                const char char2 = kBaudotCode[get_bits(46, 50) + 32];
                const char char3 = kBaudotCode[get_bits(51, 55) + 32];
                std::snprintf(buf, sizeof(buf),
                              beacon_resource(kLongAdditionalDataResourceStart + 3),
                              char1, char2, char3);
                additional_data = buf;
                set_serial_number(static_cast<uint32_t>(get_bits(56, 64)));
            } break;
            case 0b1000:
            case 0b1010:
            case 0b1011:
            case 0b1111: {
                has_serial_number = true;
                has_additional_data = true;
                set_serial_number(static_cast<uint32_t>(get_bits(41, 58)));
                const auto nat_num = static_cast<unsigned long>(get_bits(127, 132));
                std::snprintf(buf, sizeof(buf),
                              beacon_resource(kLongAdditionalDataResourceStart + 4), nat_num);
                additional_data = buf;
            } break;
            default:
                break;
        }
    }
}

void Beacon::parse_locating_devices() {
    if (protocol_is_standard() || protocol_is_national()) {
        main_locating_device = get_bits(111, 111) ? MainLocatingDevice::InternalNav
                                                  : MainLocatingDevice::ExternalNav;
    } else if (protocol_is_user() || protocol_is_rls()) {
        main_locating_device = get_bits(107, 107) ? MainLocatingDevice::InternalNav
                                                  : MainLocatingDevice::ExternalNav;
    }

    if (protocol_is_standard() || protocol_is_national()) {
        aux_locating_device = get_bits(112, 112) ? AuxLocatingDevice::Mhz121_5
                                                 : AuxLocatingDevice::NoneOrOther;
    } else if (protocol_is_rls()) {
        aux_locating_device = get_bits(108, 108) ? AuxLocatingDevice::Mhz121_5
                                                 : AuxLocatingDevice::NoneOrOther;
    } else if (protocol_is_user() && protocol_code != 0b100) {
        const uint8_t aux = static_cast<uint8_t>(get_bits(84, 85));
        if (aux == 0b00)
            aux_locating_device = AuxLocatingDevice::None;
        else if (aux == 0b01)
            aux_locating_device = AuxLocatingDevice::Mhz121_5;
        else if (aux == 0b10)
            aux_locating_device = AuxLocatingDevice::Sart;
        else
            aux_locating_device = AuxLocatingDevice::Other;
    }
}

void Beacon::parse_frame() {
    long latofmin, latofsec, lonofmin, lonofsec;
    bool latoffset, lonoffset;

    long_frame = get_bits(25, 25) != 0;
    parse_protocol();
    protocol_type = protocol_type_for(protocol);
    lookup_country(static_cast<int>(get_bits(27, 36)), country);

    if (frame[2] == 0xD0)
        frame_mode = FrameMode::SelfTest;
    else if (frame[2] == 0x2F)
        frame_mode = FrameMode::Normal;
    else
        frame_mode = FrameMode::Unknown;

    if (long_frame) {
        if (protocol_is_user() && !is_orbito()) {
            location.latitude.orientation = ((frame[13] & 0x10) >> 4) != 0;
            location.latitude.degrees = ((frame[13] & 0x0F) << 3 | (frame[14] & 0xE0) >> 5);
            location.latitude.minutes = ((frame[14] & 0x1E) >> 1) * 4;
            location.longitude.orientation = (frame[14] & 0x01) != 0;
            location.longitude.degrees = frame[15];
            location.longitude.minutes = ((frame[16] & 0xF0) >> 4) * 4;
        } else if (protocol_is_national()) {
            latoffset = ((frame[14] & 0x80) >> 7) != 0;
            location.latitude.orientation = ((frame[7] & 0x20) >> 5) != 0;
            location.latitude.degrees = ((frame[7] & 0x1F) << 2 | (frame[8] & 0xC0) >> 6);
            location.latitude.minutes = ((frame[8] & 0x3E) >> 1) * 2;
            latofmin = (frame[14] & 0x60) >> 5;
            latofsec = ((frame[14] & 0x1E) >> 1) * 4;
            location.latitude.apply_offset(latoffset, latofmin, latofsec);
            lonoffset = (frame[14] & 0x01) != 0;
            location.longitude.orientation = (frame[8] & 0x01) != 0;
            location.longitude.degrees = frame[9];
            location.longitude.minutes = ((frame[10] & 0xF8) >> 3) * 2;
            lonofmin = (frame[15] & 0xC0) >> 6;
            lonofsec = ((frame[15] & 0x3C) >> 2) * 4;
            location.longitude.apply_offset(lonoffset, lonofmin, lonofsec);
        } else if (protocol_is_standard()) {
            latoffset = ((frame[14] & 0x80) >> 7) != 0;
            location.latitude.orientation = ((frame[8] & 0x80) >> 7) != 0;
            location.latitude.degrees = (frame[8] & 0x7F);
            location.latitude.minutes = ((frame[9] & 0xC0) >> 6) * 15;
            latofmin = (frame[14] & 0x7C) >> 2;
            latofsec = ((frame[14] & 0x03) << 2 | (frame[15] & 0xC0) >> 6) * 4;
            location.latitude.apply_offset(latoffset, latofmin, latofsec);
            lonoffset = ((frame[15] & 0x20) >> 5) != 0;
            location.longitude.orientation = ((frame[9] & 0x20) >> 5) != 0;
            location.longitude.degrees = ((frame[9] & 0x1F) << 3 | (frame[10] & 0xE0) >> 5);
            location.longitude.minutes = ((frame[10] & 0x18) >> 3) * 15;
            lonofmin = (frame[15] & 0x1F);
            lonofsec = ((frame[16] & 0xF0) >> 4) * 4;
            location.longitude.apply_offset(lonoffset, lonofmin, lonofsec);
        } else if (protocol_is_rls_or_elt()) {
            latoffset = ((frame[14] & 0x20) >> 5) != 0;
            location.latitude.orientation = ((frame[8] & 0x20) >> 5) != 0;
            location.latitude.degrees = ((frame[8] & 0x1F) << 2) | ((frame[9] & 0xC0) >> 6);
            location.latitude.minutes = ((frame[9] & 0x20) >> 5) * 30;
            latofmin = (frame[14] & 0x1E) >> 1;
            latofsec = ((frame[14] & 0x01) << 3 | (frame[15] & 0xE0) >> 5) * 4;
            location.latitude.apply_offset(latoffset, latofmin, latofsec);
            lonoffset = ((frame[15] & 0x10) >> 4) != 0;
            location.longitude.orientation = ((frame[9] & 0x10) >> 4) != 0;
            location.longitude.degrees = ((frame[9] & 0x0F) << 4 | (frame[10] & 0xF0) >> 4);
            location.longitude.minutes = ((frame[10] & 0x08) >> 3) * 30;
            lonofmin = (frame[15] & 0x0F);
            lonofsec = ((frame[16] & 0xF0) >> 4) * 4;
            location.longitude.apply_offset(lonoffset, lonofmin, lonofsec);
        }
    }

    parse_additional_data();
    parse_locating_devices();

    identifier = get_bits(26, 85);

    uint32_t hex_id_high = static_cast<uint32_t>(identifier >> 32);
    uint32_t hex_id_low = static_cast<uint32_t>(identifier);

    if (protocol_is_standard()) {
        /* Default value of bits 65..74 = 0 111111111, of bits 75..85 = 0 1111111111. */
        hex_id_low &= 0b11111111'11100000'00000000'00000000u;
        hex_id_low |= 0b00000000'00001111'11111011'11111111u;
    } else if (protocol_is_national()) {
        hex_id_low &= 0b11111000'00000000'00000000'00000000u;
        hex_id_low |= 0b00000011'11111000'00011111'11100000u;
    } else if (protocol_is_rls_or_elt()) {
        hex_id_low &= 0b11111111'11111000'00000000'00000000u;
        hex_id_low |= 0b00000000'00000011'11111101'11111111u;
    }

    char id_buf[24];
    std::snprintf(id_buf, sizeof(id_buf), "%07lX%08lX",
                  static_cast<unsigned long>(hex_id_high),
                  static_cast<unsigned long>(hex_id_low));
    hex_id = id_buf;

    if (is_orbito() && !long_frame) {
        is_empty = true;
        for (size_t i = 3; i < kBeaconDataSize; i++) {
            if (frame[i] != 0) {
                is_empty = false;
                break;
            }
        }
    } else {
        is_empty = false;
    }

    bch1 = static_cast<uint32_t>(get_bits(86, 106));
    computed_bch1 = static_cast<uint32_t>(compute_bch1());
    if (computed_bch1 != bch1) simple_correction(true);

    has_bch2 = long_frame && !is_orbito();
    if (has_bch2) {
        bch2 = static_cast<uint32_t>(get_bits(133, 144));
        computed_bch2 = static_cast<uint32_t>(compute_bch2());
        if (computed_bch2 != bch2) simple_correction(false);
    }

    /* A corrected bit changes the payload, so everything above is re-derived
     * once. The guard stops that from recursing. */
    if (!correcting_ && (bch1_corrected || bch2_corrected)) {
        correcting_ = true;
        parse_frame();
        correcting_ = false;
    }
}

void Beacon::simple_correction(const bool is_bch1) {
    const int first = is_bch1 ? 25 : 107;
    const int last = is_bch1 ? 85 : 132;
    for (int i = first; i <= last; i++) {
        flip_bit(i);
        const bool fixed = is_bch1 ? (compute_bch1() == bch1) : (compute_bch2() == bch2);
        if (fixed) {
            if (is_bch1) {
                computed_bch1 = bch1;
                bch1_corrected = true;
            } else {
                computed_bch2 = bch2;
                bch2_corrected = true;
            }
            return;
        }
        flip_bit(i);  /* restore */
    }
}

std::string Beacon::hex_string(const bool with_header) const {
    std::string out;
    const size_t start = with_header ? 0u : 3u;
    const size_t end = long_frame ? kBeaconDataSize : 14u;
    for (size_t i = start; i < end; i++) out += to_string_hex(frame[i], 2);
    return out;
}

std::string Beacon::short_id() const {
    return (hex_id.size() >= 4) ? hex_id.substr(0, 4) : hex_id;
}

std::string Beacon::summary() const {
    std::string out = short_id();
    out += "-";
    out += type_name();
    out += "-";
    Location loc = location;
    out += loc.is_unknown() ? std::string{"      "}
                            : loc.to_string(Location::Format::MaidenheadLocator);
    out += "[";
    out += frame_valid() ? ((bch1_corrected || bch2_corrected) ? STR_COLOR_YELLOW
                                                               : STR_COLOR_GREEN)
                         : STR_COLOR_RED;
    out += status();
    out += STR_COLOR_WHITE "]";
    return out;
}

/* --- PacketBuilder -------------------------------------------------------- */

void PacketBuilder::reset_state() {
    packet_.clear();
    bit_history_.reset();
    state_ = State::Preamble;
    size_ = 0;
}

void PacketBuilder::flush() {
    if (handler_) handler_(packet_);
    reset_state();
}

void PacketBuilder::execute(const uint_fast8_t symbol) {
    bit_history_.add(symbol);

    switch (state_) {
        case State::Preamble: {
            const bool is_real = real_sync_(bit_history_, packet_.size());
            const bool is_test = test_sync_(bit_history_, packet_.size());
            if (is_real || is_test) {
                const uint32_t preamble = is_real ? kRealPreamble : kTestPreamble;
                for (int8_t i = static_cast<int8_t>(kPreambleBits - 1); i >= 0; i--) {
                    packet_.push_back(static_cast<uint8_t>((preamble >> i) & 0x1));
                }
                state_ = State::Format;
            }
        } break;

        case State::Format:
            packet_.push_back(static_cast<uint8_t>(symbol & 1));
            size_ = symbol ? kLongFrameBits : kShortFrameBits;
            state_ = State::Payload;
            break;

        case State::Payload:
            packet_.push_back(static_cast<uint8_t>(symbol & 1));
            if (packet_.size() >= size_) flush();
            break;
    }
}

/* --- Demodulator ---------------------------------------------------------- */

namespace {

/* Loop constants, verbatim from proc_epirb.hpp. */
constexpr float kAfcTrackAlpha = 0.005f;
constexpr float kAfcUpdatePhaseMax = 0.8f;
constexpr float kAfcConvergenceThreshold = 0.001f;
constexpr float kAfcClampRadPerSample = 0.654f;  /* +/-5 kHz at 48 kHz */
constexpr uint32_t kSymbolRate = 800;            /* 400 bps, biphase-L */

}  // namespace

void Demodulator::configure(const float sample_rate_hz) {
    sample_rate_ = (sample_rate_hz > 0.0f) ? sample_rate_hz : 48000.0f;

    samples_per_symbol_ = static_cast<size_t>(std::lround(sample_rate_ / kSymbolRate));
    if (samples_per_symbol_ < 5) samples_per_symbol_ = 5;
    samples_per_bit_ = samples_per_symbol_ * 2;
    samples_margin_ = samples_per_symbol_ / 3;
    rise_filter_samples_ = samples_per_symbol_ / 20;
    if (rise_filter_samples_ < 1) rise_filter_samples_ = 1;

    carrier_samples_threshold_ = static_cast<size_t>(0.080f * sample_rate_);
    carrier_max_samples_ = static_cast<size_t>(0.900f * sample_rate_);
    frame_max_samples_ = static_cast<size_t>(samples_per_bit_ * (144 * 1.1f));

    size_t acc = samples_per_symbol_ / 5;
    if (acc < 1) acc = 1;
    phase_delta_buffer_.assign(acc, 0.0f);

    reset();
}

void Demodulator::reset() {
    last_sample_ = dsp::cfloat{0.0f, 0.0f};
    std::fill(phase_delta_buffer_.begin(), phase_delta_buffer_.end(), 0.0f);
    phase_delta_index_ = 0;
    phase_delta_acc_ = 0.0f;
    freq_offset_est_ = 0.0f;
    afc_mean_ = 0.0f;
    afc_m2_ = 0.0f;
    afc_convergence_n_ = 0;
    carrier_phase_sum_ = 0.0f;
    carrier_phase_n_ = 0;
    state_ = State::Idle;
    stability_counter_ = 0;
    sample_count_ = 0;
    frame_sample_count_ = 0;
    rise_detection_count_ = 0;
    last_phase_positive_ = false;
    last_bit_ = false;
    builder_.reset_state();
}

float Demodulator::phase_diff(const dsp::cfloat a, const dsp::cfloat b) {
    const float di = b.real() * a.real() + b.imag() * a.imag();
    const float dq = b.imag() * a.real() - b.real() * a.imag();
    return std::atan2(dq, di);
}

bool Demodulator::filtered_rise_detect(const bool condition) {
    if (!condition) {
        rise_detection_count_ = 0;
        return false;
    }
    rise_detection_count_++;
    if (rise_detection_count_ >= rise_filter_samples_) {
        rise_detection_count_ = 0;
        return true;
    }
    return false;
}

void Demodulator::frame_end() {
    sample_count_ = 0;
    frame_sample_count_ = 0;
    stability_counter_ = 0;
    last_phase_positive_ = false;
    last_bit_ = false;
    state_ = State::Idle;
    freq_offset_est_ = 0.0f;
    carrier_phase_sum_ = 0.0f;
    carrier_phase_n_ = 0;
    afc_mean_ = 0.0f;
    afc_m2_ = 0.0f;
    afc_convergence_n_ = 0;
    builder_.reset_state();
}

void Demodulator::process(const dsp::cfloat* in, const size_t count) {
    for (size_t i = 0; i < count; i++) process_sample(in[i]);
}

void Demodulator::process_sample(const dsp::cfloat sample) {
    if (phase_delta_buffer_.empty()) configure(sample_rate_);

    sample_count_++;
    frame_sample_count_++;

    float phase_delta = phase_diff(last_sample_, sample);
    last_sample_ = sample;

    /* AFC: strip the estimated carrier offset before anything else looks at
     * the delta, so the accumulator below tracks a centred signal. */
    phase_delta -= freq_offset_est_;
    const float sample_phase_delta = phase_delta;

    phase_delta_acc_ -= phase_delta_buffer_[phase_delta_index_];
    phase_delta_buffer_[phase_delta_index_] = phase_delta;
    phase_delta_acc_ += phase_delta;
    phase_delta_index_ = (phase_delta_index_ + 1) % phase_delta_buffer_.size();
    phase_delta = phase_delta_acc_;

    switch (state_) {
        case State::Idle: {
            if (std::fabs(sample_phase_delta) <= kAfcUpdatePhaseMax) {
                freq_offset_est_ += kAfcTrackAlpha * sample_phase_delta;
                freq_offset_est_ = std::clamp(freq_offset_est_,
                                              -kAfcClampRadPerSample, kAfcClampRadPerSample);
                /* Welford, so convergence is judged on variance not on time. */
                afc_convergence_n_++;
                const float delta = freq_offset_est_ - afc_mean_;
                afc_mean_ += delta / static_cast<float>(afc_convergence_n_);
                afc_m2_ += delta * (freq_offset_est_ - afc_mean_);
            }

            if (filtered_rise_detect(std::fabs(phase_delta) >= 0.6f)) {
                stability_counter_ = 0;
                afc_mean_ = 0.0f;
                afc_m2_ = 0.0f;
                afc_convergence_n_ = 0;
            } else {
                stability_counter_++;
                if (stability_counter_ > carrier_samples_threshold_) {
                    const float afc_variance =
                        (afc_convergence_n_ > 1)
                            ? afc_m2_ / static_cast<float>(afc_convergence_n_ - 1)
                            : 0.0f;
                    if (afc_variance < kAfcConvergenceThreshold) {
                        state_ = State::CarrierLocked;
                        carrier_phase_sum_ = 0.0f;
                        carrier_phase_n_ = 0;
                        frame_sample_count_ = 0;
                    }
                }
            }
        } break;

        case State::CarrierLocked:
            carrier_phase_sum_ += sample_phase_delta;
            carrier_phase_n_++;
            /* The frame opens with a +1.1 rad step out of the flat carrier. */
            if (filtered_rise_detect(phase_delta >= 0.7f)) {
                if (carrier_phase_n_ > 0) {
                    freq_offset_est_ += carrier_phase_sum_ / static_cast<float>(carrier_phase_n_);
                    freq_offset_est_ = std::clamp(freq_offset_est_,
                                                  -kAfcClampRadPerSample, kAfcClampRadPerSample);
                }
                frame_sample_count_ = 0;
                state_ = State::DataSync;
                last_phase_positive_ = true;
                last_bit_ = true;
            } else if (frame_sample_count_ > carrier_max_samples_) {
                frame_end();
            }
            break;

        case State::DataSync:
            data_sync(phase_delta);
            break;

        case State::PostFrame:
            if (frame_sample_count_ > carrier_max_samples_) frame_end();
            break;
    }
}

void Demodulator::data_sync(const float phase_delta) {
    const float abs_phase_delta = std::fabs(phase_delta);
    const size_t sps = samples_per_symbol_;
    const size_t margin = samples_margin_;

    if (abs_phase_delta >= 1.6f) {
        /* +1.1 rad to -1.1 rad, or back: a 2.2 rad step. */
        const bool phase_positive = (phase_delta >= 0.0f);

        if (phase_positive != last_phase_positive_) {
            last_phase_positive_ = phase_positive;
            bool cur_bit;

            if ((frame_sample_count_ >= (sps - margin)) &&
                (frame_sample_count_ <= (sps + margin))) {
                /* First symbol boundary after the carrier: detection is on the
                 * falling edge, so a rising one is ignored. */
                if (!phase_positive)
                    cur_bit = true;
                else
                    return;
            } else if (sample_count_ > (sps * 2 + margin)) {
                cur_bit = last_bit_;
            } else if (sample_count_ >= (sps * 2 - margin)) {
                /* Two symbols since the last change: the bit flips. */
                cur_bit = !last_bit_;
            } else if ((sample_count_ >= (sps - margin)) && (sample_count_ <= (sps + margin))) {
                /* One symbol: the transition that starts a repeated bit is
                 * ignored, the other keeps the value. */
                if ((phase_positive && last_bit_) || (!phase_positive && !last_bit_)) {
                    sample_count_ = 0;
                    return;
                }
                cur_bit = last_bit_;
            } else {
                return;
            }

            sample_count_ = 0;
            builder_.execute(cur_bit ? 1u : 0u);
            last_bit_ = cur_bit;
        }
    }

    if (frame_sample_count_ > frame_max_samples_) {
        state_ = State::PostFrame;
        builder_.flush();
    }
}

}  // namespace epirb

/* --- View ----------------------------------------------------------------- */

const EpirbRecentEntry::Key EpirbRecentEntry::invalid_key{};

EpirbRxView::EpirbRxView()
    : receiver_{*globals().receiver} {
    add_children({&labels_,
                  &field_frequency_,
                  &step_view_,
                  &button_startstop_,
                  &text_status_,
                  &text_detail1_,
                  &text_detail2_,
                  &table_,
                  &notes_});

    table_.set_parent_rect({0, 80, 240, 164});
    table_.on_draw = [](const EpirbRecentEntry& entry, const ui::Rect& target_rect,
                        ui::Painter& painter, const ui::Style& style,
                        ui::RecentEntriesColumns&) {
        std::string line = entry.line;
        const size_t width = static_cast<size_t>(target_rect.width() / 8);
        line.resize(width, ' ');
        painter.draw_string(target_rect.location(), style, line);
    };

    button_startstop_.on_select = [this](ui::Button&) {
        if (running_) {
            running_ = false;
            button_startstop_.set_text("Start");
        } else {
            receiver_.set_mode(radio::ReceiverModel::Mode::SpectrumAnalysis);
            if (!receiver_.running()) receiver_.start();
            rebuild_front_end();
            running_ = true;
            button_startstop_.set_text("Stop");
        }
    };

    /* The C/S 406 MHz channels are 3 kHz apart from 406.022 MHz; 406.037 MHz
     * carries most of the current beacon population. */
    field_frequency_.set_step_index(2);
    field_frequency_.set_value(406'037'000ULL, false);
    field_frequency_.on_change = [this](uint64_t hz) { receiver_.set_target_frequency(hz); };

    demod_.configure(48000.0f);
    demod_.set_handler([this](const std::vector<uint8_t>& bits) { this->on_frame_bits(bits); });
}

EpirbRxView::~EpirbRxView() = default;

void EpirbRxView::on_show() {
    ui::View::on_show();
    button_startstop_.focus();
}

void EpirbRxView::on_hide() {
    running_ = false;
    button_startstop_.set_text("Start");
    ui::View::on_hide();
}

void EpirbRxView::rebuild_front_end() {
    const double input_rate = receiver_.sampling_rate();
    if (input_rate <= 0.0) return;

    /* Upstream decimates 3.072 MHz by 8 then 8 to 48 kHz. Match that rate as
     * closely as an integer factor allows and configure the loop for whatever
     * it actually is. */
    size_t decim = static_cast<size_t>(std::lround(input_rate / 48000.0));
    if (decim < 1) decim = 1;
    const double rate = input_rate / static_cast<double>(decim);

    channel_filter_.configure(dsp::design_lowpass(5500.0, 3400.0, input_rate, 60.0), decim);
    demod_.configure(static_cast<float>(rate));
    demod_.set_handler([this](const std::vector<uint8_t>& bits) { this->on_frame_bits(bits); });

    text_status_.set(to_string_dec_uint(static_cast<uint64_t>(rate)) + " Hz, " +
                     to_string_dec_uint(demod_.samples_per_symbol()) + " samp/sym");
    configured_input_rate_ = input_rate;
}

void EpirbRxView::on_frame_bits(const std::vector<uint8_t>& bits) {
    if (bits.size() < epirb::kShortFrameBits) return;

    beacon_.set_frame_bits(bits);

    if (beacon_.frame_valid()) {
        if (beacon_.bch1_corrected || beacon_.bch2_corrected)
            frames_corrected_++;
        else
            frames_valid_++;
    } else {
        frames_error_++;
    }

    auto& entry = ui::on_packet(entries_, beacon_.hex_id, 32);
    entry.count++;
    entry.line = beacon_.summary();
    table_.set_dirty();

    text_detail1_.set(std::string{beacon_.type_name()} + " " + beacon_.protocol_name() + " " +
                      beacon_.country.short_name);
    text_detail2_.set(beacon_.location.is_unknown()
                          ? std::string{"no position"}
                          : beacon_.location.to_string(epirb::Location::Format::Sexagesimal));
}

void EpirbRxView::on_frame_sync() {
    ui::View::on_frame_sync();

    if (!running_) return;
    if (receiver_.sampling_rate() != configured_input_rate_) rebuild_front_end();

    /* SAMPLE TAP — the honest note.
     *
     * A 406 MHz burst is 160 ms of carrier plus a 360/440 ms frame, and the
     * demodulator needs all of it contiguously: the carrier-stability counter
     * alone wants 80 ms unbroken. take_spectrum_samples() returns only the
     * most recent 4096-sample wideband block and clears the ready flag, so
     * ~1.7 ms arrives per UI frame and the rest is dropped. Live decoding is
     * therefore not expected to work through this tap; the demodulator and
     * frame decoder are proven end-to-end on a synthesised burst in
     * tests/test_epirb_rx.cpp.
     *
     * The tap that would make this exact: a lossless channel-rate ring buffer
     * on ReceiverModel, drained by the decoder — the host equivalent of the M4
     * handing every sample to the baseband processor. */
    if (!receiver_.take_spectrum_samples(samples_, 4096)) return;
    if (samples_.empty()) return;

    channel_filter_.process(samples_.data(), samples_.size(), channel_);
    if (channel_.empty()) return;

    demod_.process(channel_.data(), channel_.size());
}

}  // namespace app

#include "app_registry.hpp"
#include "bitmaps.hpp"

namespace {
/* Upstream: menu_location RX, icon_color green. */
const app::Registrar reg_epirb_rx{{"epirb_rx", "EPIRB RX", app::Category::Receive,
                                   ui::Color::green(), &ui::bitmap_icon_sonde,
                                   [] { return std::make_unique<app::EpirbRxView>(); }}};
}  // namespace
