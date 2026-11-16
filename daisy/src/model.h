// The Ducky model binary, format 2 (docs/NAM_FORMAT.md "Ducky model binary (format 2)"). Host-portable (no libDaisy,
// no allocation), covered by tests/model_test.cpp.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "dsp/a2lite.h"

namespace dkmodel {

constexpr uint16_t kFormatVersion = 2;
constexpr size_t kBinSize = 7908;
constexpr uint16_t kFlagHasLoudness = 0x0001;
constexpr uint16_t kFlagHasInputLevel = 0x0002;
constexpr size_t kNameSize = 64;
constexpr size_t kTextSize = 64;  // gear_type, gear_make, gear_model, modeled_by: NUL-padded printable ASCII

enum class Status : uint8_t { kOk, kSize, kMagic, kVersion, kCrc, kShape, kWeights };

inline const char* StatusName(Status s) {
    switch (s) {
        case Status::kOk: return "ok";
        case Status::kSize: return "size";
        case Status::kMagic: return "magic";
        case Status::kVersion: return "version";
        case Status::kCrc: return "crc";
        case Status::kShape: return "shape";
        case Status::kWeights: return "weights";
    }
    return "?";
}

struct Model {
    uint16_t flags;
    float weights[dkdsp::a2::kWeightCount];  // stream order, without head_scale
    float head_scale;
    float loudness;         // dB; 0 without kFlagHasLoudness
    float input_level_dbu;  // 0 without kFlagHasInputLevel
    char name[kNameSize];   // NUL-terminated printable ASCII
};

// IEEE 802.3 / zlib CRC-32, bitwise (no table): reflected poly 0xEDB88320, init and final XOR 0xFFFFFFFF.
inline uint32_t Crc32(const uint8_t* data, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

namespace detail {

// Byte offsets of the format-2 fields.
constexpr size_t kOffVersion = 4, kOffFlags = 6, kOffSize = 8, kOffDesc = 12, kOffKernels = 16, kOffKernelPad = 39,
                 kOffDilations = 40, kOffDilationPad = 86, kOffWeightCount = 88, kOffWeights = 92,
                 kOffHeadScale = 7572, kOffLoudness = 7576, kOffInputLevel = 7580, kOffName = 7584,
                 kOffGearType = 7648, kOffGearMake = 7712, kOffGearModel = 7776, kOffModeledBy = 7840, kOffCrc = 7904;
static_assert(kOffWeights + 4 * dkdsp::a2::kWeightCount == kOffHeadScale, "weights run up to head_scale");
static_assert(kOffName + kNameSize == kOffGearType && kOffGearType + kTextSize == kOffGearMake &&
                  kOffGearMake + kTextSize == kOffGearModel && kOffGearModel + kTextSize == kOffModeledBy &&
                  kOffModeledBy + kTextSize == kOffCrc && kOffCrc + 4 == kBinSize,
              "name, the gear texts, then CRC, then the end");

inline uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
inline float LeF32(const uint8_t* p) {
    const uint32_t u = Le32(p);
    float f;
    std::memcpy(&f, &u, sizeof f);
    return f;
}

}  // namespace detail

// Validates and decodes a format-2 image (the gear texts stay in flash). *out is written only on kOk.
inline Status Parse(const uint8_t* data, size_t n, Model* out) {
    using namespace detail;
    namespace a2 = dkdsp::a2;
    if (n != kBinSize || Le32(data + kOffSize) != kBinSize) return Status::kSize;
    if (std::memcmp(data, "DKNM", 4) != 0) return Status::kMagic;
    if (Le16(data + kOffVersion) != kFormatVersion) return Status::kVersion;
    if (Le32(data + kOffCrc) != Crc32(data, kOffCrc)) return Status::kCrc;

    const uint8_t* desc = data + kOffDesc;
    bool shape_ok = desc[0] == a2::kChannels && desc[1] == a2::kChannels && desc[2] == a2::kLayers &&
                    desc[3] == a2::kHeadKernel && data[kOffKernelPad] == 0 && Le16(data + kOffDilationPad) == 0 &&
                    Le32(data + kOffWeightCount) == a2::kWeightCount;
    for (int i = 0; i < a2::kLayers; ++i)
        shape_ok = shape_ok && data[kOffKernels + i] == a2::kKernelSizes[i] &&
                   Le16(data + kOffDilations + 2 * i) == a2::kDilations[i];
    if (!shape_ok) return Status::kShape;

    for (size_t i = 0; i <= a2::kWeightCount; ++i)  // the weights, then head_scale right after them
        if (!std::isfinite(LeF32(data + kOffWeights + 4 * i))) return Status::kWeights;

    out->flags = Le16(data + kOffFlags);
    for (size_t i = 0; i < a2::kWeightCount; ++i) out->weights[i] = LeF32(data + kOffWeights + 4 * i);
    out->head_scale = LeF32(data + kOffHeadScale);
    out->loudness = LeF32(data + kOffLoudness);
    out->input_level_dbu = LeF32(data + kOffInputLevel);
    std::memcpy(out->name, data + kOffName, kNameSize);
    out->name[kNameSize - 1] = '\0';
    return Status::kOk;
}

}  // namespace dkmodel
