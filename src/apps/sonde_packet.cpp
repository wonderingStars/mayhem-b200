/*
 * mayhem-b200 — Radiosonde protocol and decoder (implementation).
 *
 * See sonde_packet.hpp for the provenance and the list of deliberate
 * departures from upstream.
 *
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 * Copyright (C) 2014 zilog80
 * Copyright (C) 2023 joyel24 (Meteomodem M20)
 * Copyright (C) 2024 Mark Thompson
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "sonde_packet.hpp"

#include "../core/string_format.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>

namespace app {
namespace sonde {

namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

/* --- Rs41Calibration ------------------------------------------------------ */

void Rs41Calibration::reset() {
    std::memset(calibytes, 0, sizeof(calibytes));
    std::memset(calfrchk, 0, sizeof(calfrchk));
}

bool Rs41Calibration::store(const uint8_t slot, const uint8_t* bytes) {
    /* Upstream indexes calibytes[calfr * 16 + i] with calfr taken straight off
     * the air, which overruns the 51-slot table for any calfr > 50. */
    if (slot >= subframe_count || bytes == nullptr) return false;
    std::memcpy(calibytes + (static_cast<size_t>(slot) * subframe_size), bytes, subframe_size);
    calfrchk[slot] = 1;
    return true;
}

/* --- Packet --------------------------------------------------------------- */

Packet::Packet(const dsp::Packet& packet, const Type type, Rs41Calibration* cal)
    : packet_{packet},
      cal_{cal},
      type_{type} {
    /* Upstream constructs a BiphaseMDecoder over the raw chips and reads every
     * Meteomodem field through it; decoding once into a vector is the same
     * thing with the per-access cost paid up front. */
    bi_m_bits_ = dsp::biphase_m_decode(packet_.bits(), &bi_m_errors_);

    if (type_ == Type::Meteomodem_unknown) {
        /* Sync only told us it is a Meteomodem; the first two decoded bytes say
         * which model. */
        const uint32_t id_byte = static_cast<uint32_t>(read_bi_m(0 * 8, 16));

        if (id_byte == 0x649F)
            type_ = Type::Meteomodem_M10;
        else if (id_byte == 0x648F)
            type_ = Type::Meteomodem_M2K2;
        else if (id_byte == 0x4520 || id_byte == 0x4320)
            type_ = Type::Meteomodem_M20;
    }
}

int32_t Packet::read_bi_m(const size_t start_bit, size_t length) const {
    if (length > 32) length = 32;
    uint32_t value = 0;
    for (size_t i = start_bit; i < (start_bit + length); i++) {
        const uint32_t bit = (i < bi_m_bits_.size()) ? (bi_m_bits_[i] & 1u) : 0u;
        value = (value << 1) | bit;
    }
    return static_cast<int32_t>(value);
}

size_t Packet::length() const {
    /* Upstream returns decoder_.symbols_count(), i.e. chips / 2. */
    return packet_.size() / 2;
}

Packet::Timestamp Packet::received_at() const {
    return packet_.timestamp();
}

uint8_t Packet::vaisala_descramble(const uint32_t pos) const {
    /* The RS41 is transmitted least-significant bit first, so byte `pos` is
     * bits [pos*8 .. pos*8+7] read in reverse — upstream's
     * `value = (value << 1) | packet_[(pos * 8) + (7 - i)]`, which is exactly
     * FieldReader<..., BitRemapByteReverse>. */
    const uint8_t value = static_cast<uint8_t>(packet_.read_byte_reversed(
        static_cast<size_t>(pos) * 8, 8));
    /* +4: the packet builder consumed the 4-byte sync word, so payload byte 0
     * is on-air byte 4 and the mask has to be advanced to match. */
    const uint32_t mask_pos = pos + 4;
    return static_cast<uint8_t>(value ^ vaisala_mask[mask_pos % 64]);
}

/* Each RS41 data block is: block ID, data length, <data>, CRC-16 over the data
 * only (CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, no final
 * XOR), stored little-endian. */
bool Packet::crc16rs41(const uint32_t field_start) const {
    uint32_t pos = field_start + 1;
    const uint8_t length = vaisala_descramble(pos);

    if (static_cast<size_t>(pos) + length + 2 > packet_.size() / 8)
        return false;  /* Out of packet! */

    dsp::Crc16Ccitt crc = dsp::make_crc16_ccitt();
    for (unsigned b = 0; b < length; b++) {
        pos++;
        crc.process_byte(vaisala_descramble(pos));
    }

    const uint32_t stored =
        static_cast<uint32_t>(vaisala_descramble(field_start + 2 + length)) |
        (static_cast<uint32_t>(vaisala_descramble(field_start + 2 + length + 1)) << 8);

    return crc.checksum() == stored;
}

GPS_data Packet::get_GPS_data() const {
    GPS_data result;

    if ((type_ == Type::Meteomodem_M10) || (type_ == Type::Meteomodem_M2K2)) {
        result.alt = static_cast<uint32_t>((read_bi_m(22 * 8, 32) / 1000) - 48);
        result.lat = static_cast<float>(read_bi_m(14 * 8, 32) / ((1ULL << 32) / 360.0));
        result.lon = static_cast<float>(read_bi_m(18 * 8, 32) / ((1ULL << 32) / 360.0));
    } else if (type_ == Type::Meteomodem_M20) {
        /* m20mod.c field positions. */
        result.alt = static_cast<uint32_t>(read_bi_m(8 * 8, 24) / 100.0);
        result.lat = static_cast<float>(read_bi_m(28 * 8, 32) / 1000000.0);
        result.lon = static_cast<float>(read_bi_m(32 * 8, 32) / 1000000.0);
    } else if (type_ == Type::Vaisala_RS41_SG && crc16rs41(block_gpspos)) {
        uint8_t XYZ_bytes[4];
        int32_t XYZ = 0;
        double X[3];

        for (int32_t k = 0; k < 3; k++) {    /* X, Y, Z ECEF position from GPS */
            for (int32_t i = 0; i < 4; i++)  /* each is a little-endian int32, in cm */
                XYZ_bytes[i] = vaisala_descramble(
                    pos_GPSecefX + static_cast<uint32_t>((4 * k) + i));
            std::memcpy(&XYZ, XYZ_bytes, 4);
            X[k] = XYZ / 100.0;
        }

        /* WGS84 ECEF -> geodetic, Bowring's closed form (upstream's, verbatim). */
        const double a = 6378137.0;
        const double b = 6356752.31424518;
        const double e = std::sqrt((a * a - b * b) / (a * a));
        const double ee = std::sqrt((a * a - b * b) / (b * b));

        const double lam = std::atan2(X[1], X[0]);
        const double p = std::sqrt(X[0] * X[0] + X[1] * X[1]);
        const double t = std::atan2(X[2] * a, p * b);
        const double phi = std::atan2(X[2] + ee * ee * b * std::sin(t) * std::sin(t) * std::sin(t),
                                      p - e * e * a * std::cos(t) * std::cos(t) * std::cos(t));

        const double R = a / std::sqrt(1 - e * e * std::sin(phi) * std::sin(phi));

        result.alt = static_cast<uint32_t>(p / std::cos(phi) - R);
        result.lat = static_cast<float>(phi * 180 / kPi);
        result.lon = static_cast<float>(lam * 180 / kPi);
    }

    return result;
}

uint32_t Packet::battery_voltage() const {
    if (type_ == Type::Meteomodem_M10) {
        return static_cast<uint32_t>(
            (read_bi_m(69 * 8, 8) + (read_bi_m(70 * 8, 8) << 8)) * 1000 / 150);
    } else if (type_ == Type::Meteomodem_M20) {
        return static_cast<uint32_t>(read_bi_m(0x26 * 8, 8) * (3.3f / 255.0f) * 1000.0f);
    } else if (type_ == Type::Meteomodem_M2K2) {
        return static_cast<uint32_t>(read_bi_m(69 * 8, 8) * 66);  /* actually 65.8 */
    } else if (type_ == Type::Vaisala_RS41_SG) {
        /* Byte holds volts * 10, so * 100 gives millivolts. */
        return static_cast<uint32_t>(vaisala_descramble(pos_Voltage)) * 100u;
    }
    return 0;  /* Unknown */
}

uint32_t Packet::frame() const {
    if (type_ == Type::Vaisala_RS41_SG) {
        return static_cast<uint32_t>(vaisala_descramble(pos_FrameNb)) |
               (static_cast<uint32_t>(vaisala_descramble(pos_FrameNb + 1)) << 8);
    } else if (type_ == Type::Meteomodem_M20) {
        return static_cast<uint32_t>(read_bi_m(0x15 * 8, 8));
    }
    return 0;  /* Unknown */
}

uint8_t Packet::getFwVerM20() const {
    /* Ported verbatim, including the bit/byte mix-up: upstream passes `pos_fw`
     * (0x43) straight to read() as a BIT offset while every other M20 field is
     * read at `byte * 8`. Left as upstream has it — the alternative is a guess
     * at intent, and the only consumer is the M20 pressure correction. */
    size_t pos_fw = 0x43;
    const int flen = read_bi_m(0, 8);
    if (flen != 0x45) {
        const int auxLen = flen - 0x45;
        if (auxLen < 0) {
            pos_fw = static_cast<size_t>(flen - 2);
        }
    }
    return static_cast<uint8_t>(read_bi_m(pos_fw, 8));
}

float Packet::get_pressure() const {
    float pressure = 0.0f;

    if (type_ == Type::Meteomodem_M20) {
        float hPa = 0.0f;
        uint32_t val = (static_cast<uint32_t>(read_bi_m(0x25 * 8, 8)) << 8) |
                       static_cast<uint32_t>(read_bi_m(0x24 * 8, 8));  /* cf. DF9DQ */
        uint8_t p0 = 0x00;
        const uint8_t fwVer = getFwVerM20();
        if (fwVer >= 0x07) {  /* SPI1_P[0] */
            p0 = static_cast<uint8_t>(read_bi_m(0x16 * 8, 8));
        }
        val = (val << 8) | p0;

        if (val > 0) {
            hPa = val / static_cast<float>(16 * 256);  /* 4096 = 0x1000 */
        }
        if (hPa > 2560.0f) {  /* val > 0xA00000 */
            hPa = -1.0f;
        }
        pressure = hPa;
    }

    return pressure;
}

temp_humid Packet::get_temp_humid() const {
    temp_humid result;
    result.humid = 0;
    result.temp = 0;

    if (type_ == Type::Meteomodem_M10) {
        /* m10mod.c. */
        /* NOTE (upstream, verbatim): the low byte is shifted by 8 as well, so
         * it is discarded. Kept as-is rather than guessed at. */
        const uint16_t ADC_Ti_raw = static_cast<uint16_t>(
            (read_bi_m(0x49 * 8, 8) << 8) | read_bi_m(0x48 * 8, 8) << 8);
        if (ADC_Ti_raw != 0) {
            /* INCH1A (temp. diode), slau144. V_REF+ = 1.5 V, no calibration. */
            const float vti = ADC_Ti_raw / 4095.0f * 1.5f;
            const float ti = (vti - 0.986f) / 0.00355f;
            result.temp = ti;
        }

        /* NTC - thermistor Shibaura PB5-41E,
         * T/K = 1 / (p0 + p1*ln(R) + p2*ln(R)^2 + p3*ln(R)^3). */
        const float p0 = 1.07303516e-03f;
        const float p1 = 2.41296733e-04f;
        const float p2 = 2.26744154e-06f;
        const float p3 = 6.52855181e-08f;

        const float Rs_T[3] = {12.1e3f, 36.5e3f, 475.0e3f};  /* bias/series */
        const float Rp[3] = {1e20f, 330.0e3f, 2000.0e3f};    /* parallel, Rp[0] = inf */

        const float adc_max = 4095.0f;
        float T = 0;  /* T/Kelvin */

        const uint8_t scT = static_cast<uint8_t>(read_bi_m(0x3E * 8, 8));  /* adr_0455h */
        uint16_t ADC_RT = static_cast<uint16_t>(
            (read_bi_m(0x40 * 8, 8) << 8) | read_bi_m(0x3F * 8, 8));
        if (ADC_RT != 0) {
            ADC_RT = static_cast<uint16_t>(ADC_RT - 0xA000);

            const float x = (adc_max - ADC_RT) / static_cast<float>(ADC_RT);
            float R;
            if (scT < 3)
                R = Rs_T[scT] / (x - Rs_T[scT] / Rp[scT]);
            else
                R = -1;

            if (R > 0)
                T = 1 / (p0 + p1 * std::log(R) + p2 * std::log(R) * std::log(R) +
                         p3 * std::log(R) * std::log(R) * std::log(R));
            result.temp = T - 273.15f;
        }

        /* Humidity: counts. */
        const float TBCCR1 = (read_bi_m(0x35 * 8, 8) | (read_bi_m(0x36 * 8, 8) << 8) |
                              (read_bi_m(0x37 * 8, 8) << 16)) / 1000.0f;
        const float TBCREF = (read_bi_m(0x32 * 8, 8) | (read_bi_m(0x33 * 8, 8) << 8) |
                              (read_bi_m(0x34 * 8, 8) << 16)) / 1000.0f;
        if (TBCREF != 0) {
            const float cRHc55 = TBCCR1 / TBCREF;  /* CalRef 55 %RH at T = 20 C */
            float rh = (cRHc55 - 0.8955f) / 0.002f;  /* UPSI linear transfer function */
            const float T0 = 0.0f;
            const float T1 = -30.0f;
            if (result.temp < T0) rh += T0 - result.temp / 5.5f;             /* empirical */
            if (result.temp < T1) rh *= 1.0f + (T1 - result.temp) / 75.0f;   /* empirical */
            if (rh < 0.0f) rh = 0.0f;
            if (rh > 100.0f) rh = 100.0f;
            result.humid = rh;
        }
    }

    /* RS41: only usable once the calibration subframes have been collected, so
     * a null store means "not yet known" and reports zero, which is what
     * upstream shows before the subframes arrive. */
    if (type_ == Type::Vaisala_RS41_SG && cal_ != nullptr && crc16rs41(block_meas)) {
        /* rs41ptu.c. */
        float Rf1, Rf2;    /* reference resistors f1 (750 Ohm), f2 (1100 Ohm) */
        float co1[3], calT1[3];
        float co2[3], calT2[3];
        float calH[2];

        uint32_t meas[12];

        /* Store this frame's calibration subframe. */
        const uint8_t calfr = vaisala_descramble(pos_CalData);
        uint8_t subframe[Rs41Calibration::subframe_size];
        for (size_t i = 0; i < Rs41Calibration::subframe_size; i++)
            subframe[i] = vaisala_descramble(pos_CalData + 1 + static_cast<uint32_t>(i));
        cal_->store(calfr, subframe);

        const uint8_t* calibytes = cal_->calibytes;

        std::memcpy(&Rf1, calibytes + 61, 4);  /* 0x03*0x10+13 */
        std::memcpy(&Rf2, calibytes + 65, 4);  /* 0x04*0x10+ 1 */

        std::memcpy(co1 + 0, calibytes + 77, 4);  /* 0x04*0x10+13 */
        std::memcpy(co1 + 1, calibytes + 81, 4);  /* 0x05*0x10+ 1 */
        std::memcpy(co1 + 2, calibytes + 85, 4);  /* 0x05*0x10+ 5 */

        std::memcpy(calT1 + 0, calibytes + 89, 4);  /* 0x05*0x10+ 9 */
        std::memcpy(calT1 + 1, calibytes + 93, 4);  /* 0x05*0x10+13 */
        std::memcpy(calT1 + 2, calibytes + 97, 4);  /* 0x06*0x10+ 1 */

        std::memcpy(calH + 0, calibytes + 117, 4);  /* 0x07*0x10+ 5 */
        std::memcpy(calH + 1, calibytes + 121, 4);  /* 0x07*0x10+ 9 */

        std::memcpy(co2 + 0, calibytes + 293, 4);  /* 0x12*0x10+ 5 */
        std::memcpy(co2 + 1, calibytes + 297, 4);  /* 0x12*0x10+ 9 */
        std::memcpy(co2 + 2, calibytes + 301, 4);  /* 0x12*0x10+13 */

        std::memcpy(calT2 + 0, calibytes + 305, 4);  /* 0x13*0x10+ 1 */
        std::memcpy(calT2 + 1, calibytes + 309, 4);  /* 0x13*0x10+ 5 */
        std::memcpy(calT2 + 2, calibytes + 313, 4);  /* 0x13*0x10+ 9 */

        (void)co2;
        (void)calT2;

        for (uint32_t i = 0; i < 12; i++)
            meas[i] = static_cast<uint32_t>(vaisala_descramble(pos_temp + (3 * i))) |
                      (static_cast<uint32_t>(vaisala_descramble(pos_temp + (3 * i) + 1)) << 8) |
                      (static_cast<uint32_t>(vaisala_descramble(pos_temp + (3 * i) + 2)) << 16);

        if (cal_->have(0x03) && cal_->have(0x04) && cal_->have(0x05) && cal_->have(0x06)) {
            /* get_Tc */
            const float* p = co1;
            const float* c = calT1;
            const float g = static_cast<float>(meas[2] - meas[1]) / (Rf2 - Rf1);  /* gain */
            const float Rb = (meas[1] * Rf2 - meas[2] * Rf1) /
                             static_cast<float>(meas[2] - meas[1]);  /* offset */
            const float Rc = meas[0] / g - Rb;
            const float R = Rc * c[0];
            const float T = (p[0] + p[1] * R + p[2] * R * R + c[1]) * (1.0f + c[2]);
            result.temp = T;
        }

        if (cal_->have(0x07)) {
            /* get_RH */
            const float a0 = 7.5f;              /* empirical */
            const float a1 = 350.0f / calH[0];  /* empirical */
            const float fh = (meas[3] - meas[4]) / static_cast<float>(meas[5] - meas[4]);
            float rh = 100.0f * (a1 * fh - a0);
            const float T0 = 0.0f;
            const float T1 = -25.0f;
            rh += T0 - result.temp / 5.5f;  /* empirical temperature compensation */
            if (result.temp < T1)
                rh *= 1.0f + (T1 - result.temp) / 90.0f;
            if (rh < 0.0f) rh = 0.0f;
            if (rh > 100.0f) rh = 100.0f;
            if (result.temp < -273.0f) rh = -1.0f;
            result.humid = rh;
        }
    }

    if (type_ == Type::Meteomodem_M20) {
        const float p0 = 1.07303516e-03f;
        const float p1 = 2.41296733e-04f;
        const float p2 = 2.26744154e-06f;
        const float p3 = 6.52855181e-08f;
        const float Rs[3] = {12.1e3f, 36.5e3f, 475.0e3f};  /* bias/series */
        const float Rp[3] = {1e20f, 330.0e3f, 2000.0e3f};  /* parallel, Rp[0] = inf */
        uint8_t scT = 0;
        float T = 0;  /* T/Kelvin */

        const uint32_t b2 = static_cast<uint32_t>(read_bi_m(0x5 * 8, 8));
        const uint32_t b1 = static_cast<uint32_t>(read_bi_m(0x4 * 8, 8));
        uint16_t ADC_RT = static_cast<uint16_t>((b2 << 8) | b1);
        if (ADC_RT > 8191) {
            scT = 2;
            ADC_RT = static_cast<uint16_t>(ADC_RT - 8192);
        } else if (ADC_RT > 4095) {
            scT = 1;
            ADC_RT = static_cast<uint16_t>(ADC_RT - 4096);
        } else {
            scT = 0;
        }
        const float x = (4095.0f - ADC_RT) / static_cast<float>(ADC_RT);
        const float R = Rs[scT] / (x - Rs[scT] / Rp[scT]);
        if (R > 0)
            T = 1.0f / (p0 + p1 * std::log(R) + p2 * std::log(R) * std::log(R) +
                        p3 * std::log(R) * std::log(R) * std::log(R));
        if (T - 273.15f < -120.0f || T - 273.15f > 60.0f) T = 0;  /* out of range */
        result.temp = T - 273.15f;

        /* Humidity. */
        const float Rsq = 22.1e3f;  /* P5.6 = Vcc */
        const float R25 = 2.2e3f;
        const float bq = 3650.0f;   /* B/Kelvin */
        const float T25 = 25.0f + 273.15f;
        T = 0.0f;

        const uint32_t bq2 = static_cast<uint32_t>(read_bi_m(0x7 * 8, 8));
        const uint32_t bq1 = static_cast<uint32_t>(read_bi_m(0x6 * 8, 8));
        const uint16_t ADC_ntc0 = static_cast<uint16_t>((bq2 << 8) | bq1);
        const float xq = (4095.0f - ADC_ntc0) / static_cast<float>(ADC_ntc0);
        const float Rq = Rsq / xq;
        if (Rq > 0) T = 1.0f / (1.0f / T25 + 1.0f / bq * std::log(Rq / R25));

        const float TU = T - 273.15f;
        float RH = -1.0f;

        const uint16_t humval = static_cast<uint16_t>(
            (static_cast<uint32_t>(read_bi_m(0x03 * 8, 8)) << 8) |
            static_cast<uint32_t>(read_bi_m(0x02 * 8, 8)));
        const uint16_t rh_cal = static_cast<uint16_t>(
            (static_cast<uint32_t>(read_bi_m(0x30 * 8, 8)) << 8) |
            static_cast<uint32_t>(read_bi_m(0x2F * 8, 8)));
        const float humidityCalibration = 6.4e8f / (rh_cal + 80000.0f);
        float xqq = (humval + 80000.0f) * humidityCalibration * (1.0f - 5.8e-4f * (TU - 25.0f));
        xqq = 4.16e9f / xqq;
        xqq = 10.087f * xqq * xqq * xqq - 211.62f * xqq * xqq + 1388.2f * xqq - 2797.0f;
        if (humval < 48000) {
            if (xqq > -20.0f && xqq < 120.0f) {
                RH = xqq;
                if (RH < 0.0f) RH = 0.0f;
                if (RH > 100.0f) RH = 100.0f;
            }
        }
        result.humid = RH;
    }

    return result;
}

std::string Packet::type_string() const {
    switch (type_) {
        case Type::Unknown:
            return "Unknown";
        case Type::Meteomodem_unknown:
            return "Meteomodem ???";
        case Type::Meteomodem_M10:
            return "Meteomodem M10";
        case Type::Meteomodem_M20:
            return "Meteomodem M20";
        case Type::Meteomodem_M2K2:
            return "Meteomodem M2K2";
        case Type::Vaisala_RS41_SG:
            return "Vaisala RS41-SG";
        default:
            return "? 0x" + symbols_formatted().data.substr(0, 6);
    }
}

std::string Packet::serial_number() const {
    if (type_ == Type::Meteomodem_M10) {
        /* m10x.c, starting at byte 93:
         *   00000000 11111111 22222222 33333333 44444444
         *       CCCC          AAAABBBB
         *                              DDDEEEEE EEEEEEEE
         *
         * NOTE: upstream's Meteomodem capture is 88 * 2 * 8 chips = 88 decoded
         * bytes, so byte 93 is past the end and every read here returns zero.
         * That is upstream's behaviour and it is reproduced rather than guessed
         * at — the correct M10 frame length is not stated anywhere in the
         * upstream tree. */
        return to_string_hex(static_cast<uint64_t>(read_bi_m(93 * 8 + 16, 4)), 1) +
               to_string_dec_uint(static_cast<uint64_t>(read_bi_m(93 * 8 + 20, 4)), 2, '0') + " " +
               to_string_hex(static_cast<uint64_t>(read_bi_m(93 * 8 + 4, 4)), 1) + " " +
               to_string_dec_uint(static_cast<uint64_t>(read_bi_m(93 * 8 + 24, 3)), 1) +
               to_string_dec_uint(static_cast<uint64_t>(read_bi_m(93 * 8 + 27, 13)), 4, '0');
    } else if (type_ == Type::Vaisala_RS41_SG) {
        std::string serial_id;
        for (uint32_t i = 0; i < 8; i++) {  /* 8 bytes, each a printable char */
            const uint8_t achar = vaisala_descramble(pos_SondeID + i);
            if (achar < 32 || achar > 126) {
                serial_id += "?";
            } else {
                serial_id += static_cast<char>(achar);
            }
        }
        return serial_id;
    } else if (type_ == Type::Meteomodem_M20) {
        /* m20mod.c: 3 bytes at byte 0x12, little-endian. */
        const uint32_t sn = static_cast<uint32_t>(read_bi_m(0x12 * 8, 8)) |
                            (static_cast<uint32_t>(read_bi_m(0x13 * 8, 8)) << 8) |
                            (static_cast<uint32_t>(read_bi_m(0x14 * 8, 8)) << 16);
        return to_string_dec_uint(sn);
    }
    return "?";
}

FormattedSymbols Packet::symbols_formatted() const {
    if (type_ == Type::Vaisala_RS41_SG) {
        const size_t bytes = packet_.size() / 8;
        std::string hex_data;
        hex_data.reserve(bytes * 2);
        for (size_t i = 0; i < bytes; i++)
            hex_data += to_string_hex(vaisala_descramble(static_cast<uint32_t>(i)), 2);
        return {hex_data, std::string{}};
    }

    /* common/manchester.cpp format_symbols(), over the bi-phase-M symbols. */
    const size_t payload_length_decoded = packet_.size() / 2;
    const size_t payload_length_hex_characters = (payload_length_decoded + 3) / 4;
    const size_t payload_length_symbols_rounded = payload_length_hex_characters * 4;

    std::string hex_data;
    std::string hex_error;
    hex_data.reserve(payload_length_hex_characters);
    hex_error.reserve(payload_length_hex_characters);

    uint_fast8_t data = 0;
    uint_fast8_t error = 0;
    for (size_t i = 0; i < payload_length_symbols_rounded; i++) {
        const uint_fast8_t value = (i < bi_m_bits_.size()) ? bi_m_bits_[i] : uint_fast8_t{0};
        const uint_fast8_t err = (i < bi_m_errors_.size()) ? bi_m_errors_[i] : uint_fast8_t{1};

        data = static_cast<uint_fast8_t>((data << 1) | (value & 1));
        error = static_cast<uint_fast8_t>((error << 1) | (err & 1));

        if ((i & 3) == 3) {
            hex_data += to_string_hex(data & 0xf, 1);
            hex_error += to_string_hex(error & 0xf, 1);
        }
    }

    return {hex_data, hex_error};
}

bool Packet::crc_ok() const {
    switch (type_) {
        case Type::Meteomodem_M10:
            return crc_ok_M10();
        case Type::Vaisala_RS41_SG:
            return crc_ok_RS41();
        case Type::Meteomodem_M20:
            return check_ok_M20();
        default:
            /* No check routine, so no way to reject. Upstream's choice. */
            return true;
    }
}

bool Packet::check_ok_M20() const {
    const uint8_t b1 = static_cast<uint8_t>(read_bi_m(0, 8));
    const uint8_t b2 = static_cast<uint8_t>(read_bi_m(8, 8));
    if ((b1 != 0x45 && b1 != 0x43) || b2 != 0x20)
        return false;
    if (packet_.size() / 8 < b1)
        return false;
    return true;
}

bool Packet::crc_ok_RS41() const {
    if (!crc16rs41(block_status)) return false;
    if (!crc16rs41(block_gpspos)) return false;
    if (!crc16rs41(block_meas)) return false;
    return true;
}

bool Packet::crc_ok_M10() const {
    /* Ported verbatim from upstream, INCLUDING two defects that are visible on
     * inspection and are left in place because correcting them would mean
     * inventing a specification upstream does not state:
     *
     *   - the update function is byte-oriented (it rotates `b` within 8 bits
     *     and folds cs >> 7), yet the loop feeds it one raw CHIP at a time;
     *   - the stored checksum is read as packet_[0x63] / packet_[0x63 + 1],
     *     which are single bits, and 0x63 is in any case past the end of the
     *     88-byte Meteomodem capture.
     *
     * The practical effect is that ticking the CRC box rejects every M10 frame,
     * upstream and here. M20 frames use check_ok_M20(), which is sound. */
    uint16_t cs{0};
    uint32_t c0, c1, t, t6, t7, s, b;

    for (size_t i = 0; i < packet_.size(); i++) {
        b = packet_[i];
        c1 = cs & 0xFF;

        /* B */
        b = (b >> 1) | ((b & 1) << 7);
        b ^= (b >> 2) & 0xFF;

        /* A1 */
        t6 = (cs & 1) ^ ((cs >> 2) & 1) ^ ((cs >> 4) & 1);
        t7 = ((cs >> 1) & 1) ^ ((cs >> 3) & 1) ^ ((cs >> 5) & 1);
        t = (cs & 0x3F) | (t6 << 6) | (t7 << 7);

        /* A2 */
        s = (cs >> 7) & 0xFF;
        s ^= (s >> 2) & 0xFF;

        c0 = b ^ t ^ s;

        cs = static_cast<uint16_t>(((c1 << 8) | c0) & 0xFFFF);
    }

    const uint32_t stored = (static_cast<uint32_t>(packet_[0x63]) << 8) |
                            static_cast<uint32_t>(packet_[0x63 + 1]);
    return (static_cast<uint32_t>(cs) & 0xFFFFu) == stored;
}

/* --- Decoder -------------------------------------------------------------- */

Decoder::Decoder() {
    build_packet_builders();
    configure_channel(channel_rate_target);
}

void Decoder::build_packet_builders() {
    for (size_t sense = 0; sense < 2; sense++) {
        rs41_[sense] = std::make_unique<dsp::FixedLengthPacketBuilder>(
            dsp::BitPattern{rs41_sync, rs41_sync_bits, sync_tolerance},
            dsp::NeverMatch{},
            dsp::FixedLength{rs41_frame_bits},
            [this](const dsp::Packet& p) {
                this->emit(p, Packet::Type::Vaisala_RS41_SG, true);
            });

        meteomodem_[sense] = std::make_unique<dsp::FixedLengthPacketBuilder>(
            dsp::BitPattern{meteomodem_sync, meteomodem_sync_bits, sync_tolerance},
            dsp::NeverMatch{},
            dsp::FixedLength{meteomodem_frame_chips},
            [this](const dsp::Packet& p) {
                this->emit(p, Packet::Type::Meteomodem_unknown, false);
            });
    }
}

void Decoder::emit(const dsp::Packet& packet, const Packet::Type type, const bool is_rs41) {
    if (is_rs41)
        packets_rs41_++;
    else
        packets_meteomodem_++;

    if (handler_) {
        const Packet p{packet, type, cal_};
        handler_(p);
    }
}

void Decoder::configure_demods() {
    const float fs = static_cast<float>(channel_rate_);
    demod_rs41_.configure(fs, rs41_baud, rs41_deviation);
    demod_meteomodem_.configure(fs, meteomodem_chip_rate, meteomodem_deviation);
}

void Decoder::configure(const double input_rate_hz, const double offset_hz) {
    input_rate_ = input_rate_hz;
    offset_ = offset_hz;
    front_end_ = true;

    long long d = std::llround(input_rate_hz / channel_rate_target);
    if (d < 1) d = 1;
    decimation_ = static_cast<size_t>(d);
    channel_rate_ = input_rate_hz / static_cast<double>(decimation_);

    /* Mixing by -offset brings the wanted channel from +offset down to DC,
     * which is where the discriminators expect it. */
    nco_.set_frequency(-offset_hz, input_rate_hz);

    if (decimation_ > 1) {
        /* proc_sonde's channel is 38.4 kHz wide. 15 kHz of passband passes the
         * M10's 9600 chip/s spectrum whole and still puts the stopband edge
         * inside the decimated Nyquist. */
        channel_filter_.configure(
            dsp::design_lowpass(15000.0, 4000.0, input_rate_hz, 50.0, 2047), decimation_);
    } else {
        channel_filter_.configure(std::vector<float>{1.0f}, 1);
    }

    configure_demods();
    reset();
}

void Decoder::configure_channel(const double channel_rate_hz) {
    front_end_ = false;
    decimation_ = 1;
    input_rate_ = channel_rate_hz;
    channel_rate_ = channel_rate_hz;
    offset_ = 0.0;
    nco_.set_frequency(0.0, channel_rate_hz);
    channel_filter_.configure(std::vector<float>{1.0f}, 1);
    configure_demods();
    reset();
}

void Decoder::reset() {
    nco_.reset();
    channel_filter_.reset();
    demod_rs41_.reset();
    demod_meteomodem_.reset();
    for (size_t sense = 0; sense < 2; sense++) {
        if (rs41_[sense]) rs41_[sense]->reset();
        if (meteomodem_[sense]) meteomodem_[sense]->reset();
    }
}

void Decoder::feed(const dsp::cfloat* in, const size_t count) {
    if (in == nullptr || count == 0) return;

    if (!front_end_) {
        feed_channel(in, count);
        return;
    }

    mix_scratch_.resize(count);
    nco_.mix(in, mix_scratch_.data(), count);

    channel_scratch_.clear();
    channel_filter_.process(mix_scratch_.data(), count, channel_scratch_);
    if (!channel_scratch_.empty())
        feed_channel(channel_scratch_.data(), channel_scratch_.size());
}

void Decoder::feed_channel(const dsp::cfloat* in, const size_t count) {
    if (in == nullptr || count == 0) return;
    samples_fed_ += count;

    /* proc_sonde runs two clock recoveries off one discriminator; here each
     * protocol gets its own FskDemod because the host has the cycles and the
     * two symbol rates want different loop settings. */
    bits_scratch_.clear();
    demod_rs41_.process(in, count, bits_scratch_);
    if (!bits_scratch_.empty())
        feed_bits_rs41(bits_scratch_.data(), bits_scratch_.size());

    bits_scratch_.clear();
    demod_meteomodem_.process(in, count, bits_scratch_);
    if (!bits_scratch_.empty())
        feed_bits_meteomodem(bits_scratch_.data(), bits_scratch_.size());
}

void Decoder::feed_bits_rs41(const uint8_t* bits, const size_t count) {
    if (bits == nullptr) return;
    bits_rs41_ += count;
    for (size_t i = 0; i < count; i++) {
        const uint_fast8_t bit = static_cast<uint_fast8_t>(bits[i] & 1);
        rs41_[0]->execute(bit);
        rs41_[1]->execute(static_cast<uint_fast8_t>(bit ^ 1));
    }
}

void Decoder::feed_bits_meteomodem(const uint8_t* chips, const size_t count) {
    if (chips == nullptr) return;
    bits_meteomodem_ += count;
    for (size_t i = 0; i < count; i++) {
        const uint_fast8_t chip = static_cast<uint_fast8_t>(chips[i] & 1);
        meteomodem_[0]->execute(chip);
        meteomodem_[1]->execute(static_cast<uint_fast8_t>(chip ^ 1));
    }
}

/* --- Small helpers -------------------------------------------------------- */

std::string format_timestamp(const Packet::Timestamp t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm_value{};
#if defined(_WIN32)
    localtime_s(&tm_value, &tt);
#else
    localtime_r(&tt, &tm_value);
#endif
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm_value.tm_year + 1900, tm_value.tm_mon + 1, tm_value.tm_mday,
                  tm_value.tm_hour, tm_value.tm_min, tm_value.tm_sec);
    return std::string{buffer};
}

std::string geo_uri(const float lat, const float lon) {
    /* 5 decimal digits, ~1 m — upstream's QR payload. */
    return "geo:" + to_string_decimal(lat, 5) + "," + to_string_decimal(lon, 5);
}

}  // namespace sonde
}  // namespace app
