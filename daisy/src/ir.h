// The Ducky IR binary, format 1 (docs/FILTERS.md "IR binary (format 1)"). Host-portable (no libDaisy, no allocation),
// covered by tests/ir_test.cpp.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "binary.h"
#include "dsp/blocks.h"

namespace dkir {

constexpr uint16_t kFormatVersion = 1;
constexpr size_t kBinSize = 8292;
constexpr size_t kNameSize = 64;

enum class Status : uint8_t { kOk, kSize, kMagic, kVersion, kCrc, kTaps, kSamples };

inline const char* StatusName(Status s) {
    switch (s) {
        case Status::kOk: return "ok";
        case Status::kSize: return "size";
        case Status::kMagic: return "magic";
        case Status::kVersion: return "version";
        case Status::kCrc: return "crc";
        case Status::kTaps: return "taps";
        case Status::kSamples: return "samples";
    }
    return "?";
}

struct Ir {
    uint32_t tap_count;  // 1..kIrTaps; taps from tap_count on are 0
    uint32_t source_rate;
    uint32_t source_frames;
    uint16_t source_channels;
    float gain;                  // the converter's normalisation (80 Hz-5 kHz at 0 dB), already applied to taps
    char name[kNameSize];        // NUL-terminated printable ASCII
    float taps[dkdsp::kIrTaps];  // at 48 kHz
};

namespace detail {

// Byte offsets of the format-1 fields.
constexpr size_t kOffVersion = 4, kOffSize = 8, kOffTapCount = 12, kOffSourceRate = 16, kOffSourceFrames = 20,
                 kOffSourceChannels = 24, kOffGain = 28, kOffName = 32, kOffTaps = 96, kOffCrc = 8288;
static_assert(kOffName + kNameSize == kOffTaps, "name, then taps");
static_assert(kOffTaps + 4 * dkdsp::kIrTaps == kOffCrc && kOffCrc + 4 == kBinSize, "taps, then CRC, then the end");

}  // namespace detail

// Validates and decodes a format-1 image. *out is written only on kOk.
inline Status Parse(const uint8_t* data, size_t n, Ir* out) {
    using namespace detail;
    using dkbin::Le16;
    using dkbin::Le32;
    using dkbin::LeF32;
    if (n != kBinSize || Le32(data + kOffSize) != kBinSize) return Status::kSize;
    if (std::memcmp(data, "DKIR", 4) != 0) return Status::kMagic;
    if (Le16(data + kOffVersion) != kFormatVersion) return Status::kVersion;
    if (!dkbin::CrcOk(data, kOffCrc)) return Status::kCrc;

    const uint32_t tap_count = Le32(data + kOffTapCount);
    if (tap_count < 1 || tap_count > dkdsp::kIrTaps) return Status::kTaps;
    for (size_t i = kOffTaps + 4 * tap_count; i < kOffCrc; ++i)
        if (data[i] != 0) return Status::kTaps;

    if (!std::isfinite(LeF32(data + kOffGain))) return Status::kSamples;
    for (size_t i = 0; i < dkdsp::kIrTaps; ++i)
        if (!std::isfinite(LeF32(data + kOffTaps + 4 * i))) return Status::kSamples;

    out->tap_count = tap_count;
    out->source_rate = Le32(data + kOffSourceRate);
    out->source_frames = Le32(data + kOffSourceFrames);
    out->source_channels = Le16(data + kOffSourceChannels);
    out->gain = LeF32(data + kOffGain);
    std::memcpy(out->name, data + kOffName, kNameSize);
    out->name[kNameSize - 1] = '\0';
    for (size_t i = 0; i < dkdsp::kIrTaps; ++i) out->taps[i] = LeF32(data + kOffTaps + 4 * i);
    return Status::kOk;
}

}  // namespace dkir
