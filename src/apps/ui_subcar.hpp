/*
 * mayhem-b200 — SubCar: car key-fob (subcarrier) receiver.
 *
 * Ported from firmware/application/external/subcarrx/ (ui_subcar.*), its
 * baseband processor firmware/baseband/proc_subcar.*, and the eleven protocol
 * decoders in firmware/baseband/fprotos/c-*.hpp plus their shared base
 * (subcarbase.hpp, fprotogeneral.hpp, subcartypes.hpp, subcarprotos.hpp).
 * Those in turn come from ProtoPirate / the Flipper Xtreme sub-GHz stack.
 *
 * THE PIPELINE
 *   RF -> mix + channel filter        host: dsp::Nco + dsp::FirDecimateC
 *                                     (upstream: /4 then /2 fixed decimators)
 *   -> AM: adaptive OOK slicer with   proc_subcar.cpp execute(), modulation 0
 *          a four-state pulse/gap
 *          machine
 *      FM: quadrature discriminator   proc_subcar.cpp execute(), modulation 1
 *          with hysteresis
 *   -> a stream of (level, duration)  PulseExtractor below
 *      pulses in microseconds
 *   -> eleven parallel protocol       CarProtocols below
 *      state machines
 *   -> a recent-entries table, with a detail view that decodes serial /
 *      button / rolling counter per protocol (ui_subcar.cpp parseProtocol)
 *
 * DOCUMENTED DEVIATIONS — two upstream C++ defects that are not protocol
 * behaviour and are not reproduced:
 *
 *   1. MEMBER SHADOWING. FProtoSubCarSuzuki declares its own
 *      `uint8_t data_count_bit`, hiding FProtoSubCarBase's `uint16_t
 *      data_count_bit`. The callback reads the field through a base pointer,
 *      so a decoded Suzuki frame is reported with a bit count of 0 rather
 *      than 64. There is one field here and Suzuki reports 64.
 *
 *   2. INTEGER-TO-POINTER CASTS. c-bmw_v0.hpp does
 *      `uint8_t* raw_bytes = (uint8_t*)decode_data;` and ui_subcar.cpp does
 *      `uint8_t* data_bytes = (uint8_t*)entry_.data;` — casting a 64-bit
 *      *value* to a pointer and dereferencing it. That is undefined behaviour
 *      and on a host it is a segfault. Both sites want the eight bytes of the
 *      64-bit word, most significant first (which is how the Subaru decoder
 *      assembled it), so that is what this port does.
 *
 * Copyright (C) 2026 HTotoo (original app)
 * Copyright (C) 2026 mayhem-b200 contributors (host port)
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __MB200_UI_SUBCAR_H__
#define __MB200_UI_SUBCAR_H__

#include "../core/string_format.hpp"
#include "../dsp/fir.hpp"
#include "../dsp/protocol.hpp"
#include "../radio/receiver_model.hpp"
#include "ui.hpp"
#include "ui_freq_field.hpp"
#include "ui_recent_entries.hpp"
#include "ui_widget.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace app {
namespace subcar {

/* ===========================================================================
 * Protocol identifiers (fprotos/subcartypes.hpp)
 * ===========================================================================*/

enum SensorType : uint8_t {
    FPC_Invalid = 0,
    FPC_SUZUKI = 1,
    FPC_VW = 2,
    FPC_SUBARU = 3,
    FPC_KIAV5 = 4,
    FPC_KIAV3V4 = 5,
    FPC_KIAV2 = 6,
    FPC_KIAV1 = 7,
    FPC_KIAV0 = 8,
    FPC_FORDV0 = 9,
    FPC_FIATV0 = 10,
    FPC_BMWV0 = 11,
    /* FPC_KIAV6 and FPC_PSA are disabled upstream: fully encrypted. */
    FPC_COUNT
};

inline const char* sensor_type_name(uint8_t type) {
    switch (type) {
        case FPC_SUZUKI: return "Suzuki";
        case FPC_VW: return "VW";
        case FPC_SUBARU: return "Subaru";
        case FPC_KIAV5: return "Kia V5";
        case FPC_KIAV3V4: return "Kia V3/V4";
        case FPC_KIAV2: return "Kia V2";
        case FPC_KIAV1: return "Kia V1";
        case FPC_KIAV0: return "Kia V0";
        case FPC_FORDV0: return "Ford V0";
        case FPC_FIATV0: return "Fiat V0";
        case FPC_BMWV0: return "BMW V0";
        case FPC_Invalid:
        default: return "Unknown";
    }
}

/* ===========================================================================
 * Shared helpers (fprotos/fprotogeneral.hpp)
 * ===========================================================================*/

inline uint32_t duration_diff(uint32_t x, uint32_t y) {
    return (x < y) ? (y - x) : (x - y);
}

enum ManchesterState : uint8_t {
    ManchesterStateStart1 = 0,
    ManchesterStateMid1 = 1,
    ManchesterStateMid0 = 2,
    ManchesterStateStart0 = 3,
};

enum ManchesterEvent : uint8_t {
    ManchesterEventShortLow = 0,
    ManchesterEventShortHigh = 2,
    ManchesterEventLongLow = 4,
    ManchesterEventLongHigh = 6,
    ManchesterEventReset = 8,
};

/* The Flipper state machine, transition table and all. */
inline bool manchester_advance(ManchesterState state,
                               ManchesterEvent event,
                               ManchesterState* next_state,
                               bool* data) {
    static constexpr uint8_t transitions[] = {0b00000001, 0b10010001, 0b10011011, 0b11111011};

    bool result = false;
    ManchesterState new_state;

    if (event == ManchesterEventReset) {
        new_state = ManchesterStateMid1;
    } else {
        new_state = static_cast<ManchesterState>((transitions[state] >> event) & 0x3);
        if (new_state == state) {
            new_state = ManchesterStateMid1;
        } else if (new_state == ManchesterStateMid0) {
            if (data) *data = false;
            result = true;
        } else if (new_state == ManchesterStateMid1) {
            if (data) *data = true;
            result = true;
        }
    }

    *next_state = new_state;
    return result;
}

/* Unpacks a 64-bit word into eight bytes, most significant first. This is what
 * upstream's integer-to-pointer casts were reaching for — see the file header. */
inline void unpack_be64(uint64_t value, uint8_t out[8]) {
    for (int i = 0; i < 8; i++) out[i] = static_cast<uint8_t>(value >> (56 - i * 8));
}

/* ===========================================================================
 * Protocol base (fprotos/subcarbase.hpp)
 * ===========================================================================*/

class CarProtocol {
   public:
    using Callback = std::function<void(CarProtocol&)>;

    CarProtocol() = default;
    virtual ~CarProtocol() = default;
    CarProtocol(const CarProtocol&) = delete;
    CarProtocol& operator=(const CarProtocol&) = delete;

    virtual void feed(bool level, uint32_t duration) = 0;

    void set_callback(Callback cb) { callback_ = std::move(cb); }

    uint8_t sensor_type{FPC_Invalid};
    uint16_t data_count_bit{0};
    uint64_t decode_data{0};
    uint64_t decode_data2{0};

   protected:
    void add_bit(uint8_t bit) {
        decode_data = (decode_data << 1) | (bit & 1);
        decode_count_bit++;
    }
    void emit() {
        if (callback_) callback_(*this);
    }

    uint32_t te_short{UINT32_MAX};
    uint32_t te_long{UINT32_MAX};
    uint32_t te_delta{UINT32_MAX};
    uint32_t min_count_bit_for_found{UINT32_MAX};

    Callback callback_{};
    uint8_t parser_step{0};
    uint32_t te_last{0};
    uint32_t decode_count_bit{0};
};

/* ===========================================================================
 * Suzuki (fprotos/c-suzuki.hpp)
 *
 * 250 us / 500 us pulse-width coding, a long ~300-pulse preamble, 64 data
 * bits, terminated by a 2 ms gap.
 * ===========================================================================*/

class ProtoSuzuki : public CarProtocol {
   public:
    static constexpr uint32_t kGapTime = 2000;
    static constexpr uint32_t kGapDelta = 400;

    ProtoSuzuki() {
        sensor_type = FPC_SUZUKI;
        te_short = 250;
        te_long = 500;
        te_delta = 110;
        min_count_bit_for_found = 64;
    }

    void feed(bool level, uint32_t duration) override {
        switch (parser_step) {
            case StepReset:
                if (!level) return;
                if (duration_diff(duration, te_short) > te_delta) return;
                decode_data = 0;
                decode_count_bit = 0;
                parser_step = StepCountPreamble;
                header_count = 0;
                break;

            case StepCountPreamble:
                if (level) {
                    if (header_count >= 300) {
                        if (duration_diff(duration, te_long) <= te_delta) {
                            parser_step = StepDecodeData;
                            add_bit(1);
                        }
                    }
                } else {
                    if (duration_diff(duration, te_short) <= te_delta) {
                        te_last = duration;
                        header_count++;
                    } else {
                        parser_step = StepReset;
                    }
                }
                break;

            case StepDecodeData:
                if (level) {
                    if (duration < te_long) {
                        const uint32_t diff_long = 500 - duration;
                        if (diff_long > 99) {
                            const uint32_t diff_short =
                                (duration < 250) ? (250 - duration) : (duration - 250);
                            if (diff_short <= 99) add_bit(0);
                        } else {
                            add_bit(1);
                        }
                    } else {
                        const uint32_t diff_long = duration - 500;
                        if (diff_long <= 99) add_bit(1);
                    }
                } else {
                    const uint32_t diff_gap = duration_diff(duration, kGapTime);
                    if (diff_gap <= kGapDelta) {
                        if (decode_count_bit == 64) {
                            /* Upstream shadows data_count_bit here and so
                             * reports 0; see the file header. */
                            data_count_bit = 64;
                            emit();
                        }
                        decode_data = 0;
                        decode_count_bit = 0;
                        parser_step = StepReset;
                    }
                }
                break;
            default:
                break;
        }
    }

   private:
    enum : uint8_t { StepReset = 0, StepCountPreamble = 1, StepDecodeData = 2 };
    uint16_t header_count{0};
};

/* ===========================================================================
 * Volkswagen (fprotos/c-vw.hpp)
 *
 * 500 us / 1000 us Manchester with its own transition table, 80 bits split
 * across two words to keep the type byte and check digit separate.
 * ===========================================================================*/

class ProtoVW : public CarProtocol {
   public:
    ProtoVW() {
        sensor_type = FPC_VW;
        te_short = 500;
        te_long = 1000;
        te_delta = 130;
        min_count_bit_for_found = 80;
    }

    static uint8_t bit_index(uint8_t bit) {
        uint8_t index = 0;
        if (bit < 72 && bit >= 8) {
            index = static_cast<uint8_t>(bit - 8);
        } else {
            if (bit >= 72) index = static_cast<uint8_t>(bit - 64);  /* byte 0 = type   */
            if (bit < 8) index = bit;                               /* byte 9 = check  */
            index = static_cast<uint8_t>(index | 0x80);             /* mark for data_2 */
        }
        return index;
    }

    void feed(bool level, uint32_t duration) override {
        const uint32_t te_med = (te_long + te_short) / 2;
        const uint32_t te_end = te_long * 5;
        ManchesterEvent event = ManchesterEventReset;

        switch (parser_step) {
            case StepReset:
                if (duration_diff(duration, te_short) < te_delta) parser_step = StepFoundSync;
                break;

            case StepFoundSync:
                if (duration_diff(duration, te_short) < te_delta) break;
                if (level && duration_diff(duration, te_long) < te_delta) {
                    parser_step = StepFoundStart1;
                    break;
                }
                parser_step = StepReset;
                break;

            case StepFoundStart1:
                if (!level && duration_diff(duration, te_short) < te_delta) {
                    parser_step = StepFoundStart2;
                    break;
                }
                parser_step = StepReset;
                break;

            case StepFoundStart2:
                if (level && duration_diff(duration, te_med) < te_delta) {
                    parser_step = StepFoundStart3;
                    break;
                }
                parser_step = StepReset;
                break;

            case StepFoundStart3:
                if (duration_diff(duration, te_med) < te_delta) break;
                if (level && duration_diff(duration, te_short) < te_delta) {
                    vw_manchester_advance(manchester_state_, ManchesterEventReset,
                                          &manchester_state_, nullptr);
                    vw_manchester_advance(manchester_state_, ManchesterEventShortHigh,
                                          &manchester_state_, nullptr);
                    data_count_bit = 0;
                    decode_data = 0;
                    decode_data2 = 0;
                    parser_step = StepFoundData;
                    break;
                }
                parser_step = StepReset;
                break;

            case StepFoundData:
                if (duration_diff(duration, te_short) < te_delta) {
                    event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
                }
                if (duration_diff(duration, te_long) < te_delta) {
                    event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
                }
                /* The last bit may be arbitrarily long. */
                if (data_count_bit == min_count_bit_for_found - 1 && !level && duration > te_end) {
                    event = ManchesterEventShortLow;
                }

                if (event == ManchesterEventReset) {
                    reset_decoder();
                } else {
                    bool new_level = false;
                    if (vw_manchester_advance(manchester_state_, event, &manchester_state_,
                                              &new_level)) {
                        add_vw_bit(new_level);
                    }
                }
                break;
            default:
                break;
        }
    }

   private:
    enum : uint8_t {
        StepReset = 0,
        StepFoundSync,
        StepFoundStart1,
        StepFoundStart2,
        StepFoundStart3,
        StepFoundData,
    };

    void reset_decoder() {
        parser_step = StepReset;
        data_count_bit = 0;
        decode_data = 0;
        decode_data2 = 0;
        manchester_state_ = ManchesterStateMid1;
    }

    void add_vw_bit(bool level) {
        if (data_count_bit >= min_count_bit_for_found) return;
        const uint8_t full = static_cast<uint8_t>(min_count_bit_for_found - 1 - data_count_bit);
        const uint8_t masked = bit_index(full);
        const uint8_t index = static_cast<uint8_t>(masked & 0x7F);

        if (masked & 0x80) {
            if (level)
                decode_data2 |= (1ULL << index);
            else
                decode_data2 &= ~(1ULL << index);
        } else {
            if (level)
                decode_data |= (1ULL << index);
            else
                decode_data &= ~(1ULL << index);
        }

        data_count_bit++;
        if (data_count_bit >= min_count_bit_for_found) emit();
    }

    /* VW's own Manchester variant, not the shared table. */
    static bool vw_manchester_advance(ManchesterState state,
                                      ManchesterEvent event,
                                      ManchesterState* next_state,
                                      bool* data) {
        bool result = false;
        ManchesterState new_state = ManchesterStateMid1;

        if (event == ManchesterEventReset) {
            new_state = ManchesterStateMid1;
        } else if (state == ManchesterStateMid0 || state == ManchesterStateMid1) {
            if (event == ManchesterEventShortHigh)
                new_state = ManchesterStateStart1;
            else if (event == ManchesterEventShortLow)
                new_state = ManchesterStateStart0;
            else
                new_state = ManchesterStateMid1;
        } else if (state == ManchesterStateStart1) {
            if (event == ManchesterEventShortLow) {
                new_state = ManchesterStateMid1;
                result = true;
                if (data) *data = true;
            } else if (event == ManchesterEventLongLow) {
                new_state = ManchesterStateStart0;
                result = true;
                if (data) *data = true;
            } else {
                new_state = ManchesterStateMid1;
            }
        } else if (state == ManchesterStateStart0) {
            if (event == ManchesterEventShortHigh) {
                new_state = ManchesterStateMid0;
                result = true;
                if (data) *data = false;
            } else if (event == ManchesterEventLongHigh) {
                new_state = ManchesterStateStart1;
                result = true;
                if (data) *data = false;
            } else {
                new_state = ManchesterStateMid1;
            }
        }

        *next_state = new_state;
        return result;
    }

    ManchesterState manchester_state_{ManchesterStateMid1};
};

/* ===========================================================================
 * Subaru (fprotos/c-subaru.hpp)
 *
 * 800 us / 1600 us pulse-width coding — short HIGH is a 1, long HIGH a 0 —
 * behind a >20-pulse preamble, a 2..3.5 ms gap and a sync pulse.
 * ===========================================================================*/

class ProtoSubaru : public CarProtocol {
   public:
    ProtoSubaru() {
        sensor_type = FPC_SUBARU;
        te_short = 800;
        te_long = 1600;
        te_delta = 260;
        min_count_bit_for_found = 64;
    }

    void feed(bool level, uint32_t duration) override {
        switch (parser_step) {
            case StepReset:
                if (level && duration_diff(duration, te_long) < te_delta) {
                    parser_step = StepCheckPreamble;
                    te_last = duration;
                    header_count_ = 1;
                }
                break;

            case StepCheckPreamble:
                if (!level) {
                    if (duration_diff(duration, te_long) < te_delta) {
                        header_count_++;
                    } else if (duration > 2000 && duration < 3500) {
                        parser_step = (header_count_ > 20) ? StepFoundGap : StepReset;
                    } else {
                        parser_step = StepReset;
                    }
                } else {
                    if (duration_diff(duration, te_long) < te_delta) {
                        te_last = duration;
                        header_count_++;
                    } else {
                        parser_step = StepReset;
                    }
                }
                break;

            case StepFoundGap:
                parser_step = (level && duration > 2000 && duration < 3500) ? StepFoundSync
                                                                           : StepReset;
                break;

            case StepFoundSync:
                if (!level && duration_diff(duration, te_long) < te_delta) {
                    parser_step = StepSaveDuration;
                    bit_count_ = 0;
                    std::memset(data_, 0, sizeof(data_));
                } else {
                    parser_step = StepReset;
                }
                break;

            case StepSaveDuration:
                if (level) {
                    if (duration_diff(duration, te_short) < te_delta) {
                        add_subaru_bit(true);  /* short HIGH = 1 */
                        te_last = duration;
                        parser_step = StepCheckDuration;
                    } else if (duration_diff(duration, te_long) < te_delta) {
                        add_subaru_bit(false);  /* long HIGH = 0 */
                        te_last = duration;
                        parser_step = StepCheckDuration;
                    } else if (duration > 3000) {
                        if (bit_count_ >= 64) process_data();
                        parser_step = StepReset;
                    } else {
                        parser_step = StepReset;
                    }
                } else {
                    parser_step = StepReset;
                }
                break;

            case StepCheckDuration:
                if (!level) {
                    if (duration_diff(duration, te_short) < te_delta ||
                        duration_diff(duration, te_long) < te_delta) {
                        parser_step = StepSaveDuration;
                    } else if (duration > 3000) {
                        if (bit_count_ >= 64) process_data();
                        parser_step = StepReset;
                    } else {
                        parser_step = StepReset;
                    }
                } else {
                    parser_step = StepReset;
                }
                break;
            default:
                break;
        }
    }

   private:
    enum : uint8_t {
        StepReset = 0,
        StepCheckPreamble,
        StepFoundGap,
        StepFoundSync,
        StepSaveDuration,
        StepCheckDuration,
    };

    void add_subaru_bit(bool bit) {
        if (bit_count_ < 64) {
            const uint8_t byte_idx = static_cast<uint8_t>(bit_count_ / 8);
            const uint8_t bit_idx = static_cast<uint8_t>(7 - (bit_count_ % 8));
            if (bit)
                data_[byte_idx] = static_cast<uint8_t>(data_[byte_idx] | (1 << bit_idx));
            else
                data_[byte_idx] = static_cast<uint8_t>(data_[byte_idx] & ~(1 << bit_idx));
            bit_count_++;
        }
    }

    bool process_data() {
        if (bit_count_ < 64) return false;
        const uint8_t* b = data_;
        decode_data = (static_cast<uint64_t>(b[0]) << 56) | (static_cast<uint64_t>(b[1]) << 48) |
                      (static_cast<uint64_t>(b[2]) << 40) | (static_cast<uint64_t>(b[3]) << 32) |
                      (static_cast<uint64_t>(b[4]) << 24) | (static_cast<uint64_t>(b[5]) << 16) |
                      (static_cast<uint64_t>(b[6]) << 8) | static_cast<uint64_t>(b[7]);
        data_count_bit = bit_count_;
        emit();
        return true;
    }

    uint16_t header_count_{0};
    uint8_t data_[8]{};
    uint8_t bit_count_{0};
};

/* ===========================================================================
 * Kia V5 (fprotos/c-kia_v5.hpp)
 *
 * 400/800 us Manchester, >40-pulse preamble, up to 67 bits carried in two
 * words: the first 64 are moved to decode_data2 and the rest stay in
 * decode_data.
 * ===========================================================================*/

class ProtoKiaV5 : public CarProtocol {
   public:
    ProtoKiaV5() {
        sensor_type = FPC_KIAV5;
        te_short = 400;
        te_long = 800;
        te_delta = 150;
        min_count_bit_for_found = 64;
    }

    void feed(bool level, uint32_t duration) override {
        switch (parser_step) {
            case StepReset:
                if (level && duration_diff(duration, te_short) < te_delta) {
                    parser_step = StepCheckPreamble;
                    te_last = duration;
                    header_count_ = 1;
                    decode_count_bit = 0;
                    decode_data = 0;
                    manchester_advance(manchester_state_, ManchesterEventReset, &manchester_state_,
                                       nullptr);
                }
                break;

            case StepCheckPreamble:
                if (level) {
                    if (duration_diff(duration, te_long) < te_delta) {
                        if (header_count_ > 40) {
                            parser_step = StepData;
                            decode_count_bit = 0;
                            decode_data = 0;
                            decode_data2 = 0;
                            header_count_ = 0;
                        } else {
                            te_last = duration;
                        }
                    } else if (duration_diff(duration, te_short) < te_delta) {
                        te_last = duration;
                    } else {
                        parser_step = StepReset;
                    }
                } else {
                    if ((duration_diff(duration, te_short) < te_delta) &&
                        (duration_diff(te_last, te_short) < te_delta)) {
                        header_count_++;
                    } else if ((duration_diff(duration, te_long) < te_delta) &&
                               (duration_diff(te_last, te_short) < te_delta)) {
                        header_count_++;
                    } else if (duration_diff(te_last, te_long) < te_delta) {
                        header_count_++;
                    } else {
                        parser_step = StepReset;
                    }
                    te_last = duration;
                }
                break;

            case StepData: {
                ManchesterEvent event;
                if (duration_diff(duration, te_short) < te_delta) {
                    event = level ? ManchesterEventShortHigh : ManchesterEventShortLow;
                } else if (duration_diff(duration, te_long) < te_delta) {
                    event = level ? ManchesterEventLongHigh : ManchesterEventLongLow;
                } else {
                    if (decode_count_bit >= min_count_bit_for_found) {
                        data_count_bit =
                            static_cast<uint16_t>((decode_count_bit > 67) ? 67 : decode_count_bit);
                        emit();
                    }
                    parser_step = StepReset;
                    break;
                }

                bool data_bit = false;
                if (decode_count_bit <= 66 &&
                    manchester_advance(manchester_state_, event, &manchester_state_, &data_bit)) {
                    add_bit(data_bit ? 1 : 0);
                    if (decode_count_bit == 64) {
                        decode_data2 = decode_data;
                        decode_data = 0;
                    }
                }
                te_last = duration;
                break;
            }
            default:
                break;
        }
    }

   private:
    enum : uint8_t { StepReset = 0, StepCheckPreamble, StepData };
    uint16_t header_count_{0};
    ManchesterState manchester_state_{ManchesterStateMid1};
};

/* ===========================================================================
 * Kia V3/V4 (fprotos/c-kia_v3v4.hpp)
 *
 * 400/800 us pulse width, sync is a 1..1.5 ms pulse — LOW for V3 (whose data
 * is inverted), HIGH for V4. 68 raw bits; the KeeLoq half is left encrypted.
 * ===========================================================================*/

class ProtoKiaV3V4 : public CarProtocol {
   public:
    ProtoKiaV3V4() {
        sensor_type = FPC_KIAV3V4;
        te_short = 400;
        te_long = 800;
        te_delta = 150;
        min_count_bit_for_found = 68;
    }

    static uint8_t reverse8(uint8_t byte) {
        byte = static_cast<uint8_t>((byte & 0xF0) >> 4 | (byte & 0x0F) << 4);
        byte = static_cast<uint8_t>((byte & 0xCC) >> 2 | (byte & 0x33) << 2);
        byte = static_cast<uint8_t>((byte & 0xAA) >> 1 | (byte & 0x55) << 1);
        return byte;
    }

    void feed(bool level, uint32_t duration) override {
        switch (parser_step) {
            case StepReset:
                if (level && duration_diff(duration, te_short) < te_delta) {
                    parser_step = StepCheckPreamble;
                    te_last = duration;
                    header_count_ = 1;
                }
                break;

            case StepCheckPreamble:
                if (level) {
                    if (duration_diff(duration, te_short) < te_delta) {
                        te_last = duration;
                    } else if (duration > 1000 && duration < 1500) {
                        if (header_count_ >= 8) {
                            parser_step = StepCollectRawBits;
                            raw_bit_count_ = 0;
                            is_v3_sync_ = false;
                            std::memset(raw_bits_, 0, sizeof(raw_bits_));
                        } else {
                            parser_step = StepReset;
                        }
                    } else {
                        parser_step = StepReset;
                    }
                } else {
                    if (duration > 1000 && duration < 1500) {
                        if (header_count_ >= 8) {
                            parser_step = StepCollectRawBits;
                            raw_bit_count_ = 0;
                            is_v3_sync_ = true;
                            std::memset(raw_bits_, 0, sizeof(raw_bits_));
                        } else {
                            parser_step = StepReset;
                        }
                    } else if (duration_diff(duration, te_short) < te_delta &&
                               duration_diff(te_last, te_short) < te_delta) {
                        header_count_++;
                    } else if (duration > 1500) {
                        parser_step = StepReset;
                    }
                }
                break;

            case StepCollectRawBits:
                if (level) {
                    if (duration > 1000 && duration < 1500) {
                        process_buffer();
                        parser_step = StepReset;
                    } else if (duration_diff(duration, te_short) < te_delta) {
                        add_raw_bit(false);
                    } else if (duration_diff(duration, te_long) < te_delta) {
                        add_raw_bit(true);
                    } else {
                        parser_step = StepReset;
                    }
                } else {
                    if (duration > 1000 && duration < 1500) {
                        process_buffer();
                        parser_step = StepReset;
                    } else if (duration > 1500) {
                        process_buffer();
                        parser_step = StepReset;
                    }
                }
                break;
            default:
                break;
        }
    }

   private:
    enum : uint8_t { StepReset = 0, StepCheckPreamble, StepCollectRawBits };

    void add_raw_bit(bool bit) {
        if (raw_bit_count_ < 256) {
            const uint16_t byte_idx = static_cast<uint16_t>(raw_bit_count_ / 8);
            const uint8_t bit_idx = static_cast<uint8_t>(7 - (raw_bit_count_ % 8));
            if (bit)
                raw_bits_[byte_idx] = static_cast<uint8_t>(raw_bits_[byte_idx] | (1 << bit_idx));
            else
                raw_bits_[byte_idx] = static_cast<uint8_t>(raw_bits_[byte_idx] & ~(1 << bit_idx));
            raw_bit_count_++;
        }
    }

    bool process_buffer() {
        if (raw_bit_count_ < 68) return false;
        uint8_t* b = raw_bits_;

        /* V3's long-LOW sync means the data is inverted. */
        if (is_v3_sync_) {
            const uint16_t num_bytes = static_cast<uint16_t>((raw_bit_count_ + 7) / 8);
            for (uint16_t i = 0; i < num_bytes; i++) b[i] = static_cast<uint8_t>(~b[i]);
        }

        const uint32_t serial = (static_cast<uint32_t>(reverse8(b[7] & 0xF0)) << 24) |
                                (static_cast<uint32_t>(reverse8(b[6])) << 16) |
                                (static_cast<uint32_t>(reverse8(b[5])) << 8) |
                                static_cast<uint32_t>(reverse8(b[4]));
        const uint8_t btn = static_cast<uint8_t>((reverse8(b[7]) & 0xF0) >> 4);

        decode_data = serial;
        decode_count_bit = 68;
        decode_data2 = btn;
        data_count_bit = static_cast<uint16_t>(decode_count_bit);
        emit();
        return true;
    }

    bool is_v3_sync_{false};
    uint8_t raw_bits_[32]{};
    uint16_t raw_bit_count_{0};
    uint16_t header_count_{0};
};

/* ===========================================================================
 * Kia V2 (fprotos/c-kia_v2.hpp) — 500/1000 us Manchester, 53 bits.
 * ===========================================================================*/

class ProtoKiaV2 : public CarProtocol {
   public:
    ProtoKiaV2() {
        sensor_type = FPC_KIAV2;
        te_short = 500;
        te_long = 1000;
        te_delta = 160;
        min_count_bit_for_found = 53;
    }

    void feed(bool level, uint32_t duration) override {
        switch (parser_step) {
            case StepReset:
                if (level && duration_diff(duration, te_long) < te_delta) {
                    parser_step = StepCheckPreamble;
                    te_last = duration;
                    header_count_ = 0;
                    manchester_advance(manchester_state_, ManchesterEventReset, &manchester_state_,
                                       nullptr);
                }
                break;

            case StepCheckPreamble:
                if (level) {
                    if (duration_diff(duration, te_long) < te_delta) {
                        te_last = duration;
                        header_count_++;
                    } else if (duration_diff(duration, te_short) < te_delta) {
                        if (header_count_ >= 100) {
                            header_count_ = 0;
                            decode_data = 0;
                            decode_count_bit = 1;
                            parser_step = StepCollectRawBits;
                            add_bit(1);
                        } else {
                            te_last = duration;
                        }
                    } else {
                        parser_step = StepReset;
                    }
                } else {
                    if (duration_diff(duration, te_long) < te_delta) {
                        header_count_++;
                        te_last = duration;
                    } else if (duration_diff(duration, te_short) < te_delta) {
                        te_last = duration;
                    } else {
                        parser_step = StepReset;
                    }
                }
                break;

            case StepCollectRawBits: {
                ManchesterEvent event;
                if (duration_diff(duration, te_short) < te_delta) {
                    event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
                } else if (duration_diff(duration, te_long) < te_delta) {
                    event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
                } else {
                    parser_step = StepReset;
                    break;
                }

                bool data_bit = false;
                if (manchester_advance(manchester_state_, event, &manchester_state_, &data_bit)) {
                    add_bit(data_bit ? 1 : 0);
                    if (decode_count_bit == 53) {
                        data_count_bit = static_cast<uint16_t>(decode_count_bit);
                        emit();
                        decode_data = 0;
                        decode_count_bit = 0;
                        header_count_ = 0;
                        parser_step = StepReset;
                    }
                }
                break;
            }
            default:
                break;
        }
    }

   private:
    enum : uint8_t { StepReset = 0, StepCheckPreamble, StepCollectRawBits };
    uint16_t header_count_{0};
    ManchesterState manchester_state_{ManchesterStateMid1};
};

/* ===========================================================================
 * Kia V1 (fprotos/c-kia_v1.hpp) — 800/1600 us Manchester, 57 bits, a >70
 * pulse preamble ended by a short LOW.
 * ===========================================================================*/

class ProtoKiaV1 : public CarProtocol {
   public:
    ProtoKiaV1() {
        sensor_type = FPC_KIAV1;
        te_short = 800;
        te_long = 1600;
        te_delta = 200;
        min_count_bit_for_found = 57;
    }

    void feed(bool level, uint32_t duration) override {
        ManchesterEvent event = ManchesterEventReset;

        switch (parser_step) {
            case StepReset:
                if (level && duration_diff(duration, te_long) < te_delta) {
                    parser_step = StepCheckPreamble;
                    te_last = duration;
                    header_count_ = 0;
                    decode_data = 0;
                    decode_count_bit = 0;
                    manchester_advance(manchester_saved_state_, ManchesterEventReset,
                                       &manchester_saved_state_, nullptr);
                }
                break;

            case StepCheckPreamble:
                if (!level) {
                    if ((duration_diff(duration, te_long) < te_delta) &&
                        (duration_diff(te_last, te_long) < te_delta)) {
                        header_count_++;
                        te_last = duration;
                    } else {
                        parser_step = StepReset;
                    }
                }
                if (header_count_ > 70) {
                    if (!level && (duration_diff(duration, te_short) < te_delta) &&
                        (duration_diff(te_last, te_long) < te_delta)) {
                        decode_count_bit = 1;
                        add_bit(1);
                        header_count_ = 0;
                        parser_step = StepDecodeData;
                    }
                }
                break;

            case StepDecodeData:
                if (duration_diff(duration, te_short) < te_delta) {
                    event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
                } else if (duration_diff(duration, te_long) < te_delta) {
                    event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
                }

                if (event != ManchesterEventReset) {
                    bool data = false;
                    if (manchester_advance(manchester_saved_state_, event, &manchester_saved_state_,
                                           &data)) {
                        add_bit(data ? 1 : 0);
                    }
                }

                if (decode_count_bit == min_count_bit_for_found) {
                    data_count_bit = static_cast<uint16_t>(decode_count_bit);
                    emit();
                    decode_data = 0;
                    decode_count_bit = 0;
                    parser_step = StepReset;
                }
                break;
            default:
                break;
        }
    }

   private:
    enum : uint8_t { StepReset = 0, StepCheckPreamble, StepDecodeData };
    uint8_t header_count_{0};
    ManchesterState manchester_saved_state_{ManchesterStateStart1};
};

/* ===========================================================================
 * Kia V0 (fprotos/c-kia_v0.hpp) and BMW V0 (fprotos/c-bmw_v0.hpp)
 *
 * The same shape: a >15 short-pulse preamble ended by a long HIGH/long LOW
 * pair, then symmetric pulse pairs — short/short is a 0, long/long a 1 —
 * terminated by an over-long HIGH. BMW additionally checks a CRC8 (poly 0x31)
 * or CRC16-CCITT over the received bytes.
 * ===========================================================================*/

class ProtoKiaV0 : public CarProtocol {
   public:
    ProtoKiaV0() {
        sensor_type = FPC_KIAV0;
        te_short = 250;
        te_long = 500;
        te_delta = 100;
        min_count_bit_for_found = 61;
    }

    void feed(bool level, uint32_t duration) override {
        switch (parser_step) {
            case StepReset:
                if (level && (duration_diff(duration, te_short) < te_delta)) {
                    parser_step = StepCheckPreambula;
                    te_last = duration;
                    header_count_ = 0;
                }
                break;

            case StepCheckPreambula:
                if (level) {
                    if ((duration_diff(duration, te_short) < te_delta) ||
                        (duration_diff(duration, te_long) < te_delta)) {
                        te_last = duration;
                    } else {
                        parser_step = StepReset;
                    }
                } else if ((duration_diff(duration, te_short) < te_delta) &&
                           (duration_diff(te_last, te_short) < te_delta)) {
                    header_count_++;
                    break;
                } else if ((duration_diff(duration, te_long) < te_delta) &&
                           (duration_diff(te_last, te_long) < te_delta)) {
                    if (header_count_ > 15) {
                        parser_step = StepSaveDuration;
                        decode_data = 0;
                        decode_count_bit = 1;
                        add_bit(1);
                    } else {
                        parser_step = StepReset;
                    }
                } else {
                    parser_step = StepReset;
                }
                break;

            case StepSaveDuration:
                if (level) {
                    if (duration >= (te_long + te_delta * 2UL)) {
                        parser_step = StepReset;
                        if (decode_count_bit == min_count_bit_for_found) {
                            data_count_bit = static_cast<uint16_t>(decode_count_bit);
                            emit();
                        }
                        decode_data = 0;
                        decode_count_bit = 0;
                        break;
                    }
                    te_last = duration;
                    parser_step = StepCheckDuration;
                } else {
                    parser_step = StepReset;
                }
                break;

            case StepCheckDuration:
                if (!level) {
                    if ((duration_diff(te_last, te_short) < te_delta) &&
                        (duration_diff(duration, te_short) < te_delta)) {
                        add_bit(0);
                        parser_step = StepSaveDuration;
                    } else if ((duration_diff(te_last, te_long) < te_delta) &&
                               (duration_diff(duration, te_long) < te_delta)) {
                        add_bit(1);
                        parser_step = StepSaveDuration;
                    } else {
                        parser_step = StepReset;
                    }
                } else {
                    parser_step = StepReset;
                }
                break;
            default:
                break;
        }
    }

   protected:
    enum : uint8_t { StepReset = 0, StepCheckPreambula, StepSaveDuration, StepCheckDuration };
    uint16_t header_count_{0};
};

class ProtoBmwV0 : public CarProtocol {
   public:
    ProtoBmwV0() {
        sensor_type = FPC_BMWV0;
        te_short = 350;
        te_long = 700;
        te_delta = 120;
        min_count_bit_for_found = 61;
    }

    static uint8_t crc8(const uint8_t* data, size_t len) {
        uint8_t crc = 0x00;
        for (size_t i = 0; i < len; i++) {
            crc = static_cast<uint8_t>(crc ^ data[i]);
            for (uint8_t j = 0; j < 8; j++) {
                if (crc & 0x80)
                    crc = static_cast<uint8_t>((crc << 1) ^ 0x31);
                else
                    crc = static_cast<uint8_t>(crc << 1);
            }
        }
        return crc;
    }

    static uint16_t crc16(const uint8_t* data, size_t len) {
        uint16_t crc = 0xFFFF;
        for (size_t i = 0; i < len; i++) {
            crc = static_cast<uint16_t>(crc ^ (static_cast<uint16_t>(data[i]) << 8));
            for (uint8_t j = 0; j < 8; j++) {
                if (crc & 0x8000)
                    crc = static_cast<uint16_t>((crc << 1) ^ 0x1021);
                else
                    crc = static_cast<uint16_t>(crc << 1);
            }
        }
        return crc;
    }

    uint8_t crc_type() const { return crc_type_; }

    void feed(bool level, uint32_t duration) override {
        switch (parser_step) {
            case StepReset:
                if (level && (duration_diff(duration, te_short) < te_delta)) {
                    parser_step = StepCheckPreambula;
                    te_last = duration;
                    header_count_ = 0;
                    decode_data = 0;
                    decode_count_bit = 0;
                }
                break;

            case StepCheckPreambula:
                if (level) {
                    if ((duration_diff(duration, te_short) < te_delta) ||
                        (duration_diff(duration, te_long) < te_delta)) {
                        te_last = duration;
                    } else {
                        parser_step = StepReset;
                    }
                } else if ((duration_diff(duration, te_short) < te_delta) &&
                           (duration_diff(te_last, te_short) < te_delta)) {
                    header_count_++;
                } else if ((duration_diff(duration, te_long) < te_delta) &&
                           (duration_diff(te_last, te_long) < te_delta)) {
                    if (header_count_ > 15) {
                        parser_step = StepSaveDuration;
                        decode_data = 0;
                        decode_count_bit = 0;
                    } else {
                        parser_step = StepReset;
                    }
                } else {
                    parser_step = StepReset;
                }
                break;

            case StepSaveDuration:
                if (level) {
                    if (duration >= (te_long + te_delta * 2UL)) {
                        if (decode_count_bit >= min_count_bit_for_found) {
                            data_count_bit = static_cast<uint16_t>(decode_count_bit);

                            /* Upstream dereferences decode_data as a pointer
                             * here (undefined behaviour); the bytes it wanted
                             * are the 64-bit word, most significant first. */
                            uint8_t raw_bytes[8];
                            unpack_be64(decode_data, raw_bytes);
                            const size_t raw_len = (decode_count_bit + 7) / 8;

                            const uint8_t c8 = crc8(raw_bytes, raw_len - 1);
                            if (c8 == raw_bytes[raw_len - 1]) {
                                crc_type_ = 8;
                            } else {
                                const uint16_t c16 = crc16(raw_bytes, raw_len - 2);
                                const uint16_t rx_crc16 =
                                    static_cast<uint16_t>((raw_bytes[raw_len - 2] << 8) |
                                                          raw_bytes[raw_len - 1]);
                                crc_type_ = (c16 == rx_crc16) ? 16 : 0;
                            }

                            if (crc_type_ != 0) emit();
                        }
                        reset_internal();
                    } else {
                        te_last = duration;
                        parser_step = StepCheckDuration;
                    }
                } else {
                    parser_step = StepReset;
                }
                break;

            case StepCheckDuration:
                if (!level) {
                    if ((duration_diff(te_last, te_short) < te_delta) &&
                        (duration_diff(duration, te_short) < te_delta)) {
                        add_bit(0);
                        parser_step = StepSaveDuration;
                    } else if ((duration_diff(te_last, te_long) < te_delta) &&
                               (duration_diff(duration, te_long) < te_delta)) {
                        add_bit(1);
                        parser_step = StepSaveDuration;
                    } else {
                        parser_step = StepReset;
                    }
                } else {
                    parser_step = StepReset;
                }
                break;
            default:
                break;
        }
    }

   private:
    enum : uint8_t { StepReset = 0, StepCheckPreambula, StepSaveDuration, StepCheckDuration };

    void reset_internal() {
        decode_data = 0;
        decode_count_bit = 0;
        decode_data2 = 0;
        parser_step = StepReset;
        header_count_ = 0;
        crc_type_ = 0;
    }

    uint16_t header_count_{0};
    uint8_t crc_type_{0};
};

/* ===========================================================================
 * Ford V0 (fprotos/c-ford_v0.hpp)
 *
 * 250/500 us Manchester behind a preamble and a 3.5 ms gap. 64 bits form the
 * complemented key1, then 16 more the complemented key2.
 * ===========================================================================*/

class ProtoFordV0 : public CarProtocol {
   public:
    ProtoFordV0() {
        sensor_type = FPC_FORDV0;
        te_short = 250;
        te_long = 500;
        te_delta = 100;
        min_count_bit_for_found = 64;
    }

    void feed(bool level, uint32_t duration) override {
        constexpr uint32_t gap_threshold = 3500;

        switch (parser_step) {
            case StepReset:
                if (level && (duration_diff(duration, te_short) < te_delta)) {
                    data_low_ = 0;
                    data_high_ = 0;
                    parser_step = StepPreamble;
                    te_last = duration;
                    header_count_ = 0;
                    bit_count_ = 0;
                    manchester_advance(manchester_state_, ManchesterEventReset, &manchester_state_,
                                       nullptr);
                }
                break;

            case StepPreamble:
                if (!level) {
                    if (duration_diff(duration, te_long) < te_delta) {
                        te_last = duration;
                        parser_step = StepPreambleCheck;
                    } else {
                        parser_step = StepReset;
                    }
                }
                break;

            case StepPreambleCheck:
                if (level) {
                    if (duration_diff(duration, te_long) < te_delta) {
                        header_count_++;
                        te_last = duration;
                        parser_step = StepPreamble;
                    } else if (duration_diff(duration, te_short) < te_delta) {
                        parser_step = StepGap;
                    } else {
                        parser_step = StepReset;
                    }
                }
                break;

            case StepGap:
                if (!level && (duration_diff(duration, gap_threshold) < 250)) {
                    data_low_ = 1;
                    data_high_ = 0;
                    bit_count_ = 1;
                    parser_step = StepData;
                } else if (!level && duration > gap_threshold + 250) {
                    parser_step = StepReset;
                }
                break;

            case StepData: {
                ManchesterEvent event;
                if (duration_diff(duration, te_short) < te_delta) {
                    event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
                } else if (duration_diff(duration, te_long) < te_delta) {
                    event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
                } else {
                    parser_step = StepReset;
                    break;
                }

                bool data_bit = false;
                if (manchester_advance(manchester_state_, event, &manchester_state_, &data_bit)) {
                    add_ford_bit(data_bit);
                    if (process_data()) {
                        data_count_bit = 64;
                        emit();
                        data_low_ = 0;
                        data_high_ = 0;
                        bit_count_ = 0;
                        parser_step = StepReset;
                    }
                }
                te_last = duration;
                break;
            }
            default:
                break;
        }
    }

   private:
    enum : uint8_t { StepReset = 0, StepPreamble, StepPreambleCheck, StepGap, StepData };

    void add_ford_bit(bool bit) {
        const uint32_t low = static_cast<uint32_t>(data_low_);
        data_low_ = (data_low_ << 1) | (bit ? 1u : 0u);
        data_high_ = (data_high_ << 1) | ((low >> 31) & 1u);
        bit_count_++;
    }

    bool process_data() {
        if (bit_count_ == 64) {
            const uint64_t combined =
                (static_cast<uint64_t>(static_cast<uint32_t>(data_high_)) << 32) |
                static_cast<uint32_t>(data_low_);
            key1_ = ~combined;
            data_low_ = 0;
            data_high_ = 0;
            return false;
        }
        if (bit_count_ == 80) {
            const uint16_t key2_raw = static_cast<uint16_t>(data_low_ & 0xFFFF);
            decode_data = key1_;
            decode_data2 = static_cast<uint16_t>(~key2_raw);
            return true;
        }
        return false;
    }

    ManchesterState manchester_state_{ManchesterStateMid1};
    uint64_t data_low_{0};
    uint64_t data_high_{0};
    uint8_t bit_count_{0};
    uint16_t header_count_{0};
    uint64_t key1_{0};
};

/* ===========================================================================
 * Fiat V0 (fprotos/c-fiat_v0.hpp)
 *
 * 200/400 us Manchester behind a 150-pulse (0x96) preamble and an 800 us gap;
 * 64 bits of fix/hop plus a trailing byte.
 * ===========================================================================*/

class ProtoFiatV0 : public CarProtocol {
   public:
    ProtoFiatV0() {
        sensor_type = FPC_FIATV0;
        te_short = 200;
        te_long = 400;
        te_delta = 100;
        min_count_bit_for_found = 64;
    }

    void feed(bool level, uint32_t duration) override {
        constexpr uint32_t gap_threshold = 800;
        uint32_t diff;

        switch (decoder_state_) {
            case StepReset:
                if (!level) return;
                diff = (duration < te_short) ? (te_short - duration) : (duration - te_short);
                if (diff < te_delta) {
                    data_low_ = 0;
                    data_high_ = 0;
                    decoder_state_ = StepPreamble;
                    te_last = duration;
                    preamble_count_ = 0;
                    bit_count_ = 0;
                    manchester_advance(manchester_state_, ManchesterEventReset, &manchester_state_,
                                       nullptr);
                }
                break;

            case StepPreamble:
                if (level) return;
                if (duration < te_short) {
                    diff = te_short - duration;
                    if (diff < te_delta) {
                        preamble_count_++;
                        te_last = duration;
                        if (preamble_count_ >= 0x96) {
                            diff = (duration < gap_threshold) ? (gap_threshold - duration)
                                                              : (duration - gap_threshold);
                            if (diff < te_delta) {
                                start_data();
                                return;
                            }
                        }
                    } else {
                        decoder_state_ = StepReset;
                        if (preamble_count_ >= 0x96) {
                            diff = (duration < gap_threshold) ? (gap_threshold - duration)
                                                              : (duration - gap_threshold);
                            if (diff < te_delta) {
                                start_data();
                                return;
                            }
                        }
                    }
                } else {
                    diff = duration - te_short;
                    if (diff < te_delta) {
                        preamble_count_++;
                        te_last = duration;
                    } else {
                        decoder_state_ = StepReset;
                    }
                    if (preamble_count_ >= 0x96) {
                        diff = (duration >= 799) ? (duration - gap_threshold)
                                                 : (gap_threshold - duration);
                        if (diff < te_delta) {
                            start_data();
                            return;
                        }
                    }
                }
                break;

            case StepData: {
                ManchesterEvent event = ManchesterEventReset;
                if (duration < te_short) {
                    diff = te_short - duration;
                    if (diff < te_delta) {
                        event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
                    }
                } else {
                    diff = duration - te_short;
                    if (diff < te_delta) {
                        event = level ? ManchesterEventShortLow : ManchesterEventShortHigh;
                    } else {
                        diff = (duration < te_long) ? (te_long - duration) : (duration - te_long);
                        if (diff < te_delta) {
                            event = level ? ManchesterEventLongLow : ManchesterEventLongHigh;
                        }
                    }
                }

                if (event != ManchesterEventReset) {
                    bool data_bit = false;
                    if (manchester_advance(manchester_state_, event, &manchester_state_,
                                           &data_bit)) {
                        const uint32_t new_bit = data_bit ? 1u : 0u;
                        const uint32_t carry = (data_low_ >> 31) & 1u;
                        data_low_ = (data_low_ << 1) | new_bit;
                        data_high_ = (data_high_ << 1) | carry;
                        bit_count_++;

                        if (bit_count_ == 0x40) {
                            fix_ = data_low_;
                            hop_ = data_high_;
                            data_low_ = 0;
                            data_high_ = 0;
                        }

                        if (bit_count_ > 0x46) {
                            endbyte_ = static_cast<uint8_t>(data_low_);
                            decode_data = (static_cast<uint64_t>(hop_) << 32) | fix_;
                            decode_data2 = endbyte_;
                            data_count_bit = 64;
                            emit();

                            data_low_ = 0;
                            data_high_ = 0;
                            bit_count_ = 0;
                            decoder_state_ = StepReset;
                        }
                    }
                }
                te_last = duration;
                break;
            }
            default:
                break;
        }
    }

   private:
    enum : uint8_t { StepReset = 0, StepPreamble = 1, StepData = 2 };

    void start_data() {
        decoder_state_ = StepData;
        preamble_count_ = 0;
        data_low_ = 0;
        data_high_ = 0;
        bit_count_ = 0;
    }

    ManchesterState manchester_state_{ManchesterStateMid1};
    uint8_t decoder_state_{StepReset};
    uint8_t bit_count_{0};
    uint8_t endbyte_{0};
    uint16_t preamble_count_{0};
    uint32_t data_low_{0};
    uint32_t data_high_{0};
    uint32_t hop_{0};
    uint32_t fix_{0};
};

/* ===========================================================================
 * The full protocol set (fprotos/subcarprotos.hpp)
 * ===========================================================================*/

class CarProtocols {
   public:
    using Callback = std::function<void(CarProtocol&)>;

    CarProtocols() { reset(); }

    void set_callback(Callback cb) {
        callback_ = std::move(cb);
        for (auto& p : protos_) {
            if (p) p->set_callback([this](CarProtocol& proto) {
                if (callback_) callback_(proto);
            });
        }
    }

    /* Every protocol sees every pulse, exactly as upstream's list does. */
    void feed(bool level, uint32_t duration) {
        for (auto& p : protos_) {
            if (p) p->feed(level, duration);
        }
    }

    void reset() {
        protos_[FPC_SUZUKI] = std::make_unique<ProtoSuzuki>();
        protos_[FPC_VW] = std::make_unique<ProtoVW>();
        protos_[FPC_SUBARU] = std::make_unique<ProtoSubaru>();
        protos_[FPC_KIAV5] = std::make_unique<ProtoKiaV5>();
        protos_[FPC_KIAV3V4] = std::make_unique<ProtoKiaV3V4>();
        protos_[FPC_KIAV2] = std::make_unique<ProtoKiaV2>();
        protos_[FPC_KIAV1] = std::make_unique<ProtoKiaV1>();
        protos_[FPC_KIAV0] = std::make_unique<ProtoKiaV0>();
        protos_[FPC_FORDV0] = std::make_unique<ProtoFordV0>();
        protos_[FPC_FIATV0] = std::make_unique<ProtoFiatV0>();
        protos_[FPC_BMWV0] = std::make_unique<ProtoBmwV0>();
        set_callback(callback_);
    }

    CarProtocol* by_type(uint8_t type) const {
        return (type < FPC_COUNT) ? protos_[type].get() : nullptr;
    }

   private:
    std::array<std::unique_ptr<CarProtocol>, FPC_COUNT> protos_{};
    Callback callback_{};
};

/* ===========================================================================
 * Pulse extraction (proc_subcar.cpp execute())
 * ===========================================================================*/

/* Upstream's OOK level estimator constants. */
constexpr uint32_t kOokEstHighRatio = 3;
constexpr uint32_t kOokEstLowRatio = 5;
constexpr uint32_t kOokMaxHighLevel = 450000;

/* Upstream's magnitude is (re*re + im*im) >> 10 on complex16 samples, so a
 * full-scale sample maps to 2^30 / 2^10. This scales normalised float power
 * into the same units, which is what makes every threshold above meaningful. */
constexpr float kSubCarMagScale = 1048576.0f;  /* 32768^2 / 1024 */

class PulseExtractor {
   public:
    using PulseHandler = std::function<void(bool level, uint32_t duration_us)>;

    /* modulation: 0 = AM/OOK, 1 = FM/2-FSK, matching upstream's options. */
    void configure(float sample_rate_hz, uint8_t modulation) {
        modulation_ = modulation;
        ns_per_sample_ = (sample_rate_hz > 0.0f)
                             ? static_cast<uint32_t>(1'000'000'000.0 /
                                                     static_cast<double>(sample_rate_hz))
                             : 0;
        configured_ = ns_per_sample_ != 0;
        reset();
    }

    void reset() {
        sig_state_ = StateIdle;
        low_estimate_ = 100;
        high_estimate_ = 12000;
        numg_ = 0;
        current_duration_ = 0;
        threshold_ = 0x0630;
        current_hi_low_ = false;
        fm_level_ = false;
        fm_count_ = 0;
        fm_last_re_ = 0;
        fm_last_im_ = 0;
        fm_smoothed_ = 0;
    }

    void set_handler(PulseHandler h) { handler_ = std::move(h); }

    void process(const dsp::cfloat* in, size_t count) {
        if (!configured_ || in == nullptr) return;

        for (size_t i = 0; i < count; i++) {
            const float re_f = in[i].real();
            const float im_f = in[i].imag();
            const float mag2 = re_f * re_f + im_f * im_f;
            const uint32_t mag =
                static_cast<uint32_t>(std::min(mag2 * kSubCarMagScale, 4.0e9f));

            if (modulation_ == 0) {
                process_ook(mag);
            } else {
                process_fsk(static_cast<int32_t>(re_f * 32768.0f),
                            static_cast<int32_t>(im_f * 32768.0f), mag);
            }
        }
    }

    uint8_t modulation() const { return modulation_; }
    uint32_t threshold() const { return threshold_; }

   private:
    void process_ook(uint32_t mag) {
        threshold_ = (low_estimate_ + high_estimate_) / 2;
        const int32_t hysteresis = static_cast<int32_t>(threshold_ / 8);  /* +-12% */
        const int32_t ook_low_delta = static_cast<int32_t>(mag) - static_cast<int32_t>(low_estimate_);
        bool meashl = current_hi_low_;

        if (sig_state_ == StateIdle) {
            if (mag > (threshold_ + static_cast<uint32_t>(hysteresis))) {
                meashl = true;
                sig_state_ = StatePulse;
                numg_ = 0;
            } else {
                meashl = false;
                low_estimate_ = static_cast<uint32_t>(static_cast<int32_t>(low_estimate_) +
                                                      ook_low_delta / static_cast<int32_t>(kOokEstLowRatio));
                /* Upstream's compensation for the lack of fixed-point scaling. */
                low_estimate_ = static_cast<uint32_t>(static_cast<int32_t>(low_estimate_) +
                                                      ((ook_low_delta > 0) ? 1 : -1));
                high_estimate_ = static_cast<uint32_t>(1.35 * static_cast<double>(low_estimate_));
                high_estimate_ = std::max(high_estimate_, min_high_level_);
                high_estimate_ = std::min(high_estimate_, kOokMaxHighLevel);
            }
        } else if (sig_state_ == StatePulse) {
            if (numg_ < 100) ++numg_;
            if (mag + static_cast<uint32_t>(hysteresis) < threshold_) {
                if (numg_ < 3) {
                    sig_state_ = StateGap;  /* suspiciously short: treat as a glitch */
                } else {
                    numg_ = 0;
                    sig_state_ = StateGapStart;
                }
                meashl = false;
            } else {
                high_estimate_ += mag / kOokEstHighRatio - high_estimate_ / kOokEstHighRatio;
                high_estimate_ = std::max(high_estimate_, min_high_level_);
                high_estimate_ = std::min(high_estimate_, kOokMaxHighLevel);
                meashl = true;
            }
        } else if (sig_state_ == StateGapStart) {
            ++numg_;
            if (mag > (threshold_ + static_cast<uint32_t>(hysteresis))) {
                sig_state_ = StatePulse;
                meashl = true;
            } else if (numg_ >= 3) {
                sig_state_ = StateGap;
                meashl = false;
            }
        } else if (sig_state_ == StateGap) {
            ++numg_;
            if (mag > (threshold_ + static_cast<uint32_t>(hysteresis))) {
                numg_ = 0;
                sig_state_ = StatePulse;
                meashl = true;
            } else {
                meashl = false;
            }
        }

        if (meashl == current_hi_low_ && current_duration_ < 30'000'000) {
            current_duration_ += ns_per_sample_;
        } else {
            /* An over-long run means the transmission ended. */
            if (current_duration_ >= 30'000'000) sig_state_ = StateIdle;
            if (handler_) handler_(current_hi_low_, current_duration_ / 1000);
            current_duration_ = ns_per_sample_;
            current_hi_low_ = meashl;
        }
    }

    void process_fsk(int32_t re, int32_t im, uint32_t mag) {
        const int32_t discrim = im * fm_last_re_ - re * fm_last_im_;
        fm_last_re_ = re;
        fm_last_im_ = im;
        fm_smoothed_ += (discrim - fm_smoothed_) >> 4;

        if (mag > (threshold_ / 2)) {
            constexpr int32_t fm_hysteresis = 2000;
            bool new_level = fm_level_;
            if (fm_smoothed_ > fm_hysteresis)
                new_level = true;
            else if (fm_smoothed_ < -fm_hysteresis)
                new_level = false;

            if (new_level == fm_level_) {
                fm_count_++;
            } else {
                const uint32_t duration_us = (fm_count_ * ns_per_sample_) / 1000;
                if (duration_us > 15 && handler_) handler_(fm_level_, duration_us);
                fm_level_ = new_level;
                fm_count_ = 1;
            }
        } else {
            fm_count_ = 0;
        }
    }

    enum : uint8_t { StateIdle = 0, StatePulse = 1, StateGapStart = 2, StateGap = 3 };

    PulseHandler handler_{};
    uint8_t modulation_{0};
    uint8_t sig_state_{StateIdle};
    uint32_t low_estimate_{100};
    uint32_t high_estimate_{12000};
    uint32_t min_high_level_{10};
    uint8_t numg_{0};
    uint32_t ns_per_sample_{0};
    uint32_t current_duration_{0};
    uint32_t threshold_{0x0630};
    bool current_hi_low_{false};
    bool configured_{false};

    /* FM path */
    bool fm_level_{false};
    uint32_t fm_count_{0};
    int32_t fm_last_re_{0};
    int32_t fm_last_im_{0};
    int32_t fm_smoothed_{0};
};

/* ===========================================================================
 * Decoded-frame interpretation (ui_subcar.cpp parseProtocol)
 * ===========================================================================*/

constexpr uint32_t kNoSerial = 0xFFFFFFFF;
constexpr uint32_t kNoCount = 0xFF;

struct DecodedFrame {
    uint32_t serial{0};
    std::string button{};
    uint32_t count{kNoCount};
};

/* Subaru's rolling counter, recovered from the eight key bytes
 * (ui_subcar.cpp subaru_decode_count). */
inline void subaru_decode_count(const uint8_t* kb, uint16_t* count) {
    uint8_t lo = 0;
    if ((kb[4] & 0x40) == 0) lo |= 0x01;
    if ((kb[4] & 0x80) == 0) lo |= 0x02;
    if ((kb[5] & 0x01) == 0) lo |= 0x04;
    if ((kb[5] & 0x02) == 0) lo |= 0x08;
    if ((kb[6] & 0x01) == 0) lo |= 0x10;
    if ((kb[6] & 0x02) == 0) lo |= 0x20;
    if ((kb[5] & 0x40) == 0) lo |= 0x40;
    if ((kb[5] & 0x80) == 0) lo |= 0x80;

    uint8_t reg_sh1 = static_cast<uint8_t>((kb[7] << 4) & 0xF0);
    if (kb[5] & 0x04) reg_sh1 |= 0x04;
    if (kb[5] & 0x08) reg_sh1 |= 0x08;
    if (kb[6] & 0x80) reg_sh1 |= 0x02;
    if (kb[6] & 0x40) reg_sh1 |= 0x01;

    const uint8_t reg_sh2 = static_cast<uint8_t>(((kb[6] << 2) & 0xF0) | ((kb[7] >> 4) & 0x0F));

    uint8_t ser0 = kb[3];
    uint8_t ser1 = kb[1];
    uint8_t ser2 = kb[2];

    const uint8_t total_rot = static_cast<uint8_t>(4 + lo);
    for (uint8_t i = 0; i < total_rot; ++i) {
        const uint8_t t_bit = static_cast<uint8_t>((ser0 >> 7) & 1);
        ser0 = static_cast<uint8_t>(((ser0 << 1) & 0xFE) | ((ser1 >> 7) & 1));
        ser1 = static_cast<uint8_t>(((ser1 << 1) & 0xFE) | ((ser2 >> 7) & 1));
        ser2 = static_cast<uint8_t>(((ser2 << 1) & 0xFE) | t_bit);
    }

    const uint8_t t1 = static_cast<uint8_t>(ser1 ^ reg_sh1);
    const uint8_t t2 = static_cast<uint8_t>(ser2 ^ reg_sh2);

    uint8_t hi = 0;
    if ((t1 & 0x10) == 0) hi |= 0x04;
    if ((t1 & 0x20) == 0) hi |= 0x08;
    if ((t2 & 0x80) == 0) hi |= 0x02;
    if ((t2 & 0x40) == 0) hi |= 0x01;
    if ((t1 & 0x01) == 0) hi |= 0x40;
    if ((t1 & 0x02) == 0) hi |= 0x80;
    if ((t2 & 0x08) == 0) hi |= 0x20;
    if ((t2 & 0x04) == 0) hi |= 0x10;

    *count = static_cast<uint16_t>(((hi << 8) | lo) & 0xFFFF);
}

/* Kia V5's counter, from its 18-round keystream (ui_subcar.cpp). */
inline uint16_t kia_v5_decode_count(uint32_t encrypted) {
    const uint8_t keystore_bytes[] = {0x53, 0x54, 0x46, 0x52, 0x4B, 0x45, 0x30, 0x30};
    uint8_t s0 = static_cast<uint8_t>(encrypted & 0xFF);
    uint8_t s1 = static_cast<uint8_t>((encrypted >> 8) & 0xFF);
    uint8_t s2 = static_cast<uint8_t>((encrypted >> 16) & 0xFF);
    uint8_t s3 = static_cast<uint8_t>((encrypted >> 24) & 0xFF);
    int round_index = 1;

    for (size_t i = 0; i < 18; i++) {
        uint8_t r = static_cast<uint8_t>(keystore_bytes[round_index] & 0xFF);
        int steps = 8;
        while (steps > 0) {
            uint8_t base;
            if ((s3 & 0x40) == 0)
                base = ((s3 & 0x02) == 0) ? 0x74 : 0x2E;
            else
                base = ((s3 & 0x02) == 0) ? 0x3A : 0x5C;

            if (s2 & 0x08) base = static_cast<uint8_t>((((base >> 4) & 0x0F) | ((base & 0x0F) << 4)) & 0xFF);
            if (s1 & 0x01) base = static_cast<uint8_t>(((base & 0x3F) << 2) & 0xFF);
            if (s0 & 0x01) base = static_cast<uint8_t>((base << 1) & 0xFF);

            const uint8_t temp = static_cast<uint8_t>((s3 ^ s1) & 0xFF);
            s3 = static_cast<uint8_t>(((s3 & 0x7F) << 1) & 0xFF);
            if (s2 & 0x80) s3 |= 0x01;
            s2 = static_cast<uint8_t>(((s2 & 0x7F) << 1) & 0xFF);
            if (s1 & 0x80) s2 |= 0x01;
            s1 = static_cast<uint8_t>(((s1 & 0x7F) << 1) & 0xFF);
            if (s0 & 0x80) s1 |= 0x01;
            s0 = static_cast<uint8_t>(((s0 & 0x7F) << 1) & 0xFF);

            const uint8_t chk = static_cast<uint8_t>((base ^ (r ^ temp)) & 0xFF);
            if (chk & 0x80) s0 |= 0x01;
            r = static_cast<uint8_t>(((r & 0x7F) << 1) & 0xFF);
            steps--;
        }
        round_index = (round_index - 1) & 0x7;
    }

    return static_cast<uint16_t>((s0 + (s1 << 8)) & 0xFFFF);
}

/* Port of SubCarRecentEntryDetailView::parseProtocol(). */
inline DecodedFrame parse_frame(uint8_t sensor_type, uint64_t data, uint64_t data2, uint16_t bits) {
    DecodedFrame out{};
    (void)bits;

    if (sensor_type == FPC_Invalid) return out;

    if (sensor_type == FPC_SUZUKI) {
        const uint32_t data_high = static_cast<uint32_t>(data >> 32);
        const uint32_t data_low = static_cast<uint32_t>(data);
        out.serial = ((data_high & 0xFFF) << 16) | (data_low >> 16);
        const uint8_t button = static_cast<uint8_t>((data_low >> 12) & 0xF);
        out.count = (data_high << 4) >> 16;
        out.button = to_string_dec_uint(button);
        return out;
    }

    if (sensor_type == FPC_VW) {
        out.serial = static_cast<uint32_t>(data & 0xFFFFFFFF);  /* trimmed to 32 bits for VW */
        const uint8_t check = static_cast<uint8_t>(data2 & 0xFF);
        switch ((check >> 4) & 0xF) {
            case 0x1: out.button = "UNLOCK"; break;
            case 0x2: out.button = "LOCK"; break;
            case 0x3: out.button = "Un+Lk"; break;
            case 0x4: out.button = "TRUNK"; break;
            case 0x5: out.button = "Un+Tr"; break;
            case 0x6: out.button = "Lk+Tr"; break;
            case 0x7: out.button = "Un+Lk+Tr"; break;
            case 0x8: out.button = "PANIC"; break;
            default: out.button = "Unknown"; break;
        }
        return out;
    }

    if (sensor_type == FPC_SUBARU) {
        /* Upstream casts the 64-bit value to a pointer here; the bytes it
         * wants are the word itself, most significant first. */
        uint8_t b[8];
        unpack_be64(data, b);
        out.serial = (static_cast<uint32_t>(b[1]) << 16) | (static_cast<uint32_t>(b[2]) << 8) | b[3];
        out.button = to_string_dec_uint(static_cast<uint8_t>(b[0] & 0x0F));
        uint16_t counter = 0;
        subaru_decode_count(b, &counter);
        out.count = counter;
        return out;
    }

    if (sensor_type == FPC_KIAV5) {
        uint64_t yek = 0;
        for (int i = 0; i < 8; i++) {
            const uint8_t byte = static_cast<uint8_t>((data2 >> (i * 8)) & 0xFF);
            uint8_t reversed = 0;
            for (int b = 0; b < 8; b++) {
                if (byte & (1 << b)) reversed = static_cast<uint8_t>(reversed | (1 << (7 - b)));
            }
            yek |= (static_cast<uint64_t>(reversed) << ((7 - i) * 8));
        }
        out.serial = static_cast<uint32_t>((yek >> 32) & 0x0FFFFFFF);
        out.button = to_string_dec_uint(static_cast<uint8_t>((yek >> 60) & 0x0F));
        out.count = kia_v5_decode_count(static_cast<uint32_t>(yek & 0xFFFFFFFF));
        return out;
    }

    if (sensor_type == FPC_KIAV3V4) {
        out.serial = static_cast<uint32_t>(data);  /* KeeLoq half is not decrypted */
        out.button = "?";
        return out;
    }

    if (sensor_type == FPC_KIAV2) {
        out.serial = static_cast<uint32_t>((data >> 20) & 0xFFFFFFFF);
        out.button = to_string_dec_uint(static_cast<uint8_t>((data >> 16) & 0x0F));
        const uint16_t raw_count = static_cast<uint16_t>((data >> 4) & 0xFFF);
        out.count = static_cast<uint32_t>(((raw_count >> 4) | (raw_count << 8)) & 0xFFF);
        return out;
    }

    if (sensor_type == FPC_KIAV1) {
        out.serial = static_cast<uint32_t>((data >> 24) & 0xFFFFFFFF);
        out.button = to_string_dec_uint(static_cast<uint8_t>((data >> 16) & 0xFF));
        out.count = static_cast<uint32_t>((static_cast<uint8_t>((data >> 4) & 0xF) << 8) |
                                          ((data >> 8) & 0xFF));
        return out;
    }

    if (sensor_type == FPC_KIAV0 || sensor_type == FPC_BMWV0) {
        /* Upstream decodes both with the same field layout. */
        out.serial = static_cast<uint32_t>((data >> 12) & 0x0FFFFFFF);
        out.button = to_string_dec_uint(static_cast<uint8_t>((data >> 8) & 0x0F));
        out.count = static_cast<uint32_t>((data >> 40) & 0xFFFF);
        return out;
    }

    if (sensor_type == FPC_FORDV0) {
        uint8_t buf[13] = {0};
        for (int i = 0; i < 8; ++i) buf[i] = static_cast<uint8_t>(data >> (56 - i * 8));
        buf[8] = static_cast<uint8_t>(data2 >> 8);
        buf[9] = static_cast<uint8_t>(data2 & 0xFF);

        uint8_t tmp = buf[8];
        uint8_t parity = 0;
        const uint8_t parity_any = (tmp != 0) ? 1 : 0;
        while (tmp) {
            parity = static_cast<uint8_t>(parity ^ (tmp & 1));
            tmp = static_cast<uint8_t>(tmp >> 1);
        }
        buf[11] = parity_any ? parity : 0;

        uint8_t xor_byte;
        uint8_t limit;
        if (buf[11]) {
            xor_byte = buf[7];
            limit = 7;
        } else {
            xor_byte = buf[6];
            limit = 6;
        }
        for (int idx = 1; idx < limit; ++idx) buf[idx] = static_cast<uint8_t>(buf[idx] ^ xor_byte);
        if (buf[11] == 0) buf[7] = static_cast<uint8_t>(buf[7] ^ xor_byte);

        const uint8_t orig_b7 = buf[7];
        buf[7] = static_cast<uint8_t>((orig_b7 & 0xAA) | (buf[6] & 0x55));
        const uint8_t mixed = static_cast<uint8_t>((buf[6] & 0xAA) | (orig_b7 & 0x55));
        buf[12] = mixed;
        buf[6] = mixed;

        const uint32_t serial_le = static_cast<uint32_t>(buf[1]) |
                                   (static_cast<uint32_t>(buf[2]) << 8) |
                                   (static_cast<uint32_t>(buf[3]) << 16) |
                                   (static_cast<uint32_t>(buf[4]) << 24);
        out.serial = ((serial_le & 0xFF) << 24) | (((serial_le >> 8) & 0xFF) << 16) |
                     (((serial_le >> 16) & 0xFF) << 8) | ((serial_le >> 24) & 0xFF);
        out.button = to_string_dec_uint(static_cast<uint8_t>((buf[5] >> 4) & 0x0F));
        out.count = static_cast<uint32_t>(((buf[5] & 0x0F) << 16) | (buf[6] << 8) | buf[7]);
        return out;
    }

    if (sensor_type == FPC_FIATV0) {
        out.serial = static_cast<uint32_t>(data & 0xFFFFFFFF);
        out.count = static_cast<uint32_t>((data >> 32) & 0xFFFFFFFF);
        out.button = to_string_dec_uint(static_cast<uint8_t>(data2 & 0xFF));
        return out;
    }

    return out;
}

/* ===========================================================================
 * Recent entries
 * ===========================================================================*/

struct RecentEntry {
    using Key = uint64_t;
    static constexpr Key invalid_key = 0x0FFFFFFF;

    uint8_t sensor_type{FPC_Invalid};
    uint16_t bits{0};
    uint16_t age{0};
    uint64_t data{0};
    uint64_t data2{0};

    RecentEntry() = default;
    explicit RecentEntry(Key k) : data{k} {}
    RecentEntry(uint8_t type, uint64_t d, uint64_t d2, uint16_t n)
        : sensor_type{type}, bits{n}, data{d}, data2{d2} {}

    Key key() const {
        return data ^ ((static_cast<uint64_t>(sensor_type) & 0xFF) << 0);
    }
    void inc_age(int delta) {
        if (UINT16_MAX - delta > age) age = static_cast<uint16_t>(age + delta);
    }
    void reset_age() { age = 0; }

    std::string to_csv() const {
        std::string csv = ";";
        csv += sensor_type_name(sensor_type);
        csv += ";" + to_string_dec_uint(bits) + ";";
        csv += to_string_hex(data, 64 / 4) + ";" + to_string_hex(data2, 64 / 4);
        return csv;
    }
};

using RecentEntries = ui::RecentEntries<RecentEntry>;
using RecentEntriesView = ui::RecentEntriesView<RecentEntries>;

}  // namespace subcar

/* ===========================================================================
 * Views
 * ===========================================================================*/

class SubCarDetailView : public ui::View {
   public:
    explicit SubCarDetailView(const subcar::RecentEntry& entry);

    std::string title() const override { return "SubCar"; }
    void on_show() override;

   private:
    subcar::RecentEntry entry_{};

    ui::Labels labels_{
        {{0, 0}, "Type:", ui::Color::light_grey()},
        {{0, 32}, "Serial:", ui::Color::light_grey()},
        {{0, 48}, "Data:", ui::Color::light_grey()},
    };

    ui::Text text_type_{{0, 16, 240, 16}, "?"};
    ui::Text text_id_{{64, 32, 176, 16}, "?"};
    ui::Console console_{{0, 64, 240, 190}};
    ui::Button button_done_{{140, 262, 96, 32}, "Done"};
};

class SubCarView : public ui::View {
   public:
    SubCarView();
    ~SubCarView() override;

    SubCarView(const SubCarView&) = delete;
    SubCarView& operator=(const SubCarView&) = delete;

    std::string title() const override { return "SubCar"; }

    void on_show() override;
    void on_hide() override;
    void on_frame_sync() override;

   private:
    void rebuild_channel_filter();
    void on_decoded(subcar::CarProtocol& proto);

    radio::ReceiverModel& receiver_;

    subcar::CarProtocols protocols_{};
    subcar::PulseExtractor extractor_{};
    subcar::RecentEntries recent_{};

    std::vector<dsp::cfloat> samples_{};
    std::vector<dsp::cfloat> channel_{};
    dsp::FirDecimateC channel_filter_{};
    double filter_input_rate_{0.0};
    double channel_rate_{500'000.0};

    uint8_t modulation_{0};
    uint32_t frame_counter_{0};

    ui::FrequencyField field_frequency_{{0, 0}};
    ui::FrequencyStepView step_view_{{84, 0}, field_frequency_};
    ui::NumberField field_gain_{{184, 0}, 3, {0, 76}, 1, ' '};

    ui::Button button_clear_{{0, 20, 56, 24}, "Clear"};
    ui::Labels labels_{
        {{112, 26}, "Mode:", ui::Color::light_grey()},
    };
    ui::OptionsField options_mode_{{168, 26}, 3, {{"AM ", 0}, {"FM ", 1}}};

    ui::Text text_status_{{0, 48, 240, 16}, ""};

    ui::RecentEntriesColumns columns_{{{"Type", 0}, {"Bits", 4}, {"Age", 3}}};
    subcar::RecentEntriesView recent_view_{columns_, recent_};
};

}  // namespace app

#endif /*__MB200_UI_SUBCAR_H__*/
