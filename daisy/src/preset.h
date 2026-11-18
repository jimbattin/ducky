// The preset store (docs/FIRMWARE.md "Preset store"): 8 slots of 64 KB in QSPI from 0x90400000, each a format-3
// preset (a model binary, an optional IR binary, a name, a noise gate, an optional morph block with its path's
// waypoints, a CRC), and a two-copy state record holding the last slot. Host-portable (no libDaisy, no allocation),
// covered by tests/preset_test.cpp. The slot format is the contract with editor/src/preset/bank.ts,
// editor/src/preset/gate.ts and editor/src/preset/morph.ts.
//
// Slot (format 3, little-endian): "DKPS", u16 version, u16 flags (bit 0 kFlagHasIr, bit 1 kFlagHasMorph, bits 2-3
// the morph's waypoint count K, 0 without kFlagHasMorph), u32 size (ImageSize of the flags), name[64]
// (NUL-terminated printable ASCII), the gate block (kGateSize: u8 flags with bit 0 enabled, u8 threshold, u8 attack,
// u8 hold, u16 release, u8 ratio, u8 0; units in dsp/noise_gate.h), the model binary (dkmodel::kBinSize), the IR
// binary (dkir::kBinSize) when flagged, the morph block (dkmorph::kBlockBytes, morph.h) when flagged, its K
// waypoints (dkmorph::kWaypointBytes each: a2::kWeightCount finite f32, a weight stream in dkmodel::Model::weights'
// order with the head scale folded in, in the model's coordinates), then the CRC-32 of bytes [0, size - 4). Erased
// flash (0xFF) follows.
//
// State record (16 bytes at the start of each 4 KB state sector): "DKST", u32 seq, u32 slot (0..7), u32 CRC-32 of the
// first 12 bytes. Every write goes to the sector not holding the newest record, so a torn write leaves the previous
// record readable; at boot the valid record with the higher seq wins.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "binary.h"
#include "dsp/noise_gate.h"
#include "ir.h"
#include "model.h"
#include "morph.h"

namespace dkpreset {

constexpr uint32_t kQspiBase = 0x90000000;                // memory-mapped QSPI
constexpr uint32_t kBankOffset = 0x400000;                // QSPI chip offset of slot 0 (0x90400000)
constexpr int kSlots = 8;
constexpr uint32_t kSlotBytes = 0x10000;                  // one 64 KB erase block per slot
constexpr uint32_t kBankBytes = kSlots * kSlotBytes;      // 0x80000, the DFU bank image
constexpr uint32_t kStateOffset[2] = {0x480000, 0x481000};  // QSPI chip offsets of the two state sectors
constexpr uint32_t kStateSectorBytes = 0x1000;            // 4 KB sector, erased as a whole
constexpr uint32_t kMpuBytes = 0x100000;                  // firmware.h region 4: 0x90400000-0x904FFFFF
constexpr uint16_t kFormatVersion = 3;
constexpr uint16_t kFlagHasIr = 0x0001;
constexpr uint16_t kFlagHasMorph = 0x0002;
constexpr int kFlagWaypointsShift = 2;  // the morph's waypoint count, 0..dkmorph::kMaxWaypoints
constexpr uint16_t kFlagWaypointsMask = 0x000C;
constexpr size_t kNameSize = 64;
constexpr size_t kGateSize = 8;
constexpr size_t kHeaderSize = 84;
constexpr size_t kSizeNoIr = kHeaderSize + dkmodel::kBinSize + 4;  // 7996
constexpr size_t kSizeWithIr = kSizeNoIr + dkir::kBinSize;         // 16288
constexpr size_t kSizeMax = kSizeWithIr + dkmorph::kBlockBytes + dkmorph::kMaxWaypoints * dkmorph::kWaypointBytes;
constexpr size_t kStateBytes = 16;
constexpr uint32_t kStateMagic = 0x54534B44;  // "DKST"

static_assert(kBankOffset + kBankBytes <= kStateOffset[0], "the slots end before the state sectors");
static_assert(kStateOffset[0] + kStateSectorBytes <= kStateOffset[1], "the state sectors do not overlap");
static_assert(kStateOffset[1] + kStateSectorBytes <= kBankOffset + kMpuBytes, "the store lies inside MPU region 4");
static_assert(kSizeMax <= kSlotBytes, "a preset fits its slot");
static_assert(kSizeNoIr == 7996 && kSizeWithIr == 16288, "the format-3 sizes");
static_assert(dkmorph::kTargets == kSlots, "a morph block's target is a slot");
static_assert((kFlagWaypointsMask >> kFlagWaypointsShift) == dkmorph::kMaxWaypoints, "the flag holds every count");

// A preset image's size: waypoints (0..dkmorph::kMaxWaypoints) counts only with has_morph.
constexpr size_t ImageSize(bool has_ir, bool has_morph, int waypoints) {
    return (has_ir ? kSizeWithIr : kSizeNoIr) +
           (has_morph ? dkmorph::kBlockBytes + size_t(waypoints) * dkmorph::kWaypointBytes : 0);
}
static_assert(kSizeMax == ImageSize(true, true, dkmorph::kMaxWaypoints) && kSizeMax == 38792, "the largest image");

// Whether size is ImageSize of some preset.
constexpr bool IsImageSize(size_t size) {
    for (int ir = 0; ir < 2; ++ir) {
        if (size == ImageSize(ir != 0, false, 0)) return true;
        for (int k = 0; k <= dkmorph::kMaxWaypoints; ++k)
            if (size == ImageSize(ir != 0, true, k)) return true;
    }
    return false;
}

enum class Status : uint8_t { kOk, kEmpty, kMagic, kVersion, kSize, kCrc, kModel, kIr, kMorph };

inline const char* StatusName(Status s) {
    switch (s) {
        case Status::kOk: return "ok";
        case Status::kEmpty: return "empty";
        case Status::kMagic: return "magic";
        case Status::kVersion: return "version";
        case Status::kSize: return "size";
        case Status::kCrc: return "crc";
        case Status::kModel: return "model";
        case Status::kIr: return "ir";
        case Status::kMorph: return "morph";
    }
    return "?";
}

// No initialisers: it lives in DK_SDRAM.
struct Preset {
    bool has_ir;
    char name[kNameSize];  // NUL-terminated printable ASCII
    dkmodel::Model model;
    dkir::Ir ir;  // valid only with has_ir
    dkdsp::GateSettings gate;
    bool has_morph;
    dkmorph::Block morph;  // valid only with has_morph
    int waypoint_count;    // the morph's path: 0 (a straight blend) without has_morph
    float waypoints[dkmorph::kMaxWaypoints][dkdsp::a2::kWeightCount];  // the first waypoint_count: morph.h "Path"
};

namespace detail {

constexpr size_t kOffVersion = 4, kOffFlags = 6, kOffSize = 8, kOffName = 12, kOffGate = 76, kOffModel = kHeaderSize,
                 kOffIr = kHeaderSize + dkmodel::kBinSize;
static_assert(kOffName + kNameSize == kOffGate, "name, then the gate");
static_assert(kOffGate + kGateSize == kHeaderSize, "the gate, then the model");
static_assert(kOffIr == 7992, "the IR follows the model");

// Where an image's morph block starts: after the model, and the IR when it has one.
constexpr size_t MorphOffset(bool has_ir) { return kOffIr + (has_ir ? dkir::kBinSize : 0); }
static_assert(MorphOffset(true) + 4 == kSizeWithIr, "without a morph block the CRC follows the IR");

// The CRC at size - 4 of image[0, size), recomputed.
inline void Reseal(uint8_t* image, size_t size) { dkbin::PutLe32(image + size - 4, dkbin::Crc32(image, size - 4)); }

}  // namespace detail

// The kGateSize-byte gate block of g.
inline void PutGate(uint8_t* out, const dkdsp::GateSettings& g) {
    out[0] = g.enabled ? 1 : 0;
    out[1] = g.threshold;
    out[2] = g.attack;
    out[3] = g.hold;
    out[4] = uint8_t(g.release);
    out[5] = uint8_t(g.release >> 8);
    out[6] = g.ratio;
    out[7] = 0;
}

// A gate block decoded, each field clamped into its range (the slot CRC already guards against corruption).
inline dkdsp::GateSettings GetGate(const uint8_t* p) {
    return dkdsp::ClampGate({(p[0] & 1) != 0, p[1], p[2], p[3], uint16_t(p[4] | p[5] << 8), p[6]});
}

// Validates and decodes the slot data[0, n). *out holds valid data only on kOk.
inline Status Parse(const uint8_t* data, size_t n, Preset* out) {
    using namespace detail;
    using dkbin::Le16;
    using dkbin::Le32;
    if (n < 4) return Status::kSize;
    if (data[0] == 0xFF && data[1] == 0xFF && data[2] == 0xFF && data[3] == 0xFF) return Status::kEmpty;
    if (std::memcmp(data, "DKPS", 4) != 0) return Status::kMagic;
    if (n < kHeaderSize) return Status::kSize;
    const uint16_t flags = Le16(data + kOffFlags);
    const bool has_ir = (flags & kFlagHasIr) != 0, has_morph = (flags & kFlagHasMorph) != 0;
    const int waypoints = (flags & kFlagWaypointsMask) >> kFlagWaypointsShift;
    if (Le16(data + kOffVersion) != kFormatVersion ||
        (flags & ~(kFlagHasIr | kFlagHasMorph | kFlagWaypointsMask)) != 0 || (waypoints > 0 && !has_morph))
        return Status::kVersion;
    const uint32_t size = Le32(data + kOffSize);
    if (size != ImageSize(has_ir, has_morph, waypoints) || size > n) return Status::kSize;
    if (Le32(data + size - 4) != dkbin::Crc32(data, size - 4)) return Status::kCrc;
    if (dkmodel::Parse(data + kOffModel, dkmodel::kBinSize, &out->model) != dkmodel::Status::kOk) return Status::kModel;
    if (has_ir && dkir::Parse(data + kOffIr, dkir::kBinSize, &out->ir) != dkir::Status::kOk) return Status::kIr;
    const uint8_t* morph = data + MorphOffset(has_ir);
    if (has_morph && !dkmorph::ParseBlock(morph, &out->morph)) return Status::kMorph;
    for (int k = 0; k < waypoints; ++k) {
        const uint8_t* w = morph + dkmorph::kBlockBytes + size_t(k) * dkmorph::kWaypointBytes;
        for (size_t i = 0; i < dkdsp::a2::kWeightCount; ++i) {
            out->waypoints[k][i] = dkbin::LeF32(w + 4 * i);
            if (!std::isfinite(out->waypoints[k][i])) return Status::kMorph;
        }
    }
    out->waypoint_count = waypoints;
    out->has_ir = has_ir;
    out->has_morph = has_morph;
    out->gate = GetGate(data + kOffGate);
    std::memcpy(out->name, data + kOffName, kNameSize);
    out->name[kNameSize - 1] = '\0';
    return Status::kOk;
}

// A record with kStateMagic, its CRC and slot < kSlots; seq and slot are its fields (untouched when invalid).
inline bool StateValid(const uint8_t* rec, uint32_t& seq, int& slot) {
    using dkbin::Le32;
    if (Le32(rec) != kStateMagic || Le32(rec + 12) != dkbin::Crc32(rec, 12)) return false;
    const uint32_t s = Le32(rec + 8);
    if (s >= uint32_t(kSlots)) return false;
    seq = Le32(rec + 4);
    slot = int(s);
    return true;
}

// The record (0 for a, 1 for b) holding the newest valid state, with its fields in seq and slot: the higher seq when
// both are valid; -1 for none (seq and slot untouched).
inline int NewestState(const uint8_t* a, const uint8_t* b, uint32_t& seq, int& slot) {
    uint32_t sa = 0, sb = 0;
    int ka = -1, kb = -1;
    const bool va = StateValid(a, sa, ka), vb = StateValid(b, sb, kb);
    const int s = va && vb ? (sb > sa ? 1 : 0) : va ? 0 : vb ? 1 : -1;
    if (s >= 0) {
        seq = s == 1 ? sb : sa;
        slot = s == 1 ? kb : ka;
    }
    return s;
}

// kStateBytes bytes into out.
inline void BuildState(uint8_t* out, uint32_t seq, int slot) {
    dkbin::PutLe32(out, kStateMagic);
    dkbin::PutLe32(out + 4, seq);
    dkbin::PutLe32(out + 8, uint32_t(slot));
    dkbin::PutLe32(out + 12, dkbin::Crc32(out, 12));
}

// Sets the name of the preset image[0, size) (a parsed slot, size its size field): name[64] zero-filled, then len
// (<= kNameSize - 1) bytes of name, and the CRC at size - 4 rewritten.
inline void Rename(uint8_t* image, size_t size, const char* name, size_t len) {
    if (len > kNameSize - 1) len = kNameSize - 1;
    std::memset(image + detail::kOffName, 0, kNameSize);
    std::memcpy(image + detail::kOffName, name, len);
    detail::Reseal(image, size);
}

// Sets the gate of the preset image[0, size) (a parsed slot, size its size field), and rewrites the CRC at size - 4.
inline void SetGate(uint8_t* image, size_t size, const dkdsp::GateSettings& g) {
    PutGate(image + detail::kOffGate, g);
    detail::Reseal(image, size);
}

// The preset image (a parsed slot) with its morph block set to block[0, dkmorph::kBlockBytes), or removed when block
// is null, written to out (kSizeMax bytes, not overlapping image): the name, the gate, the model and the IR are
// kept, the image's waypoints are dropped (a block set here is a straight blend), the flags and the size field
// follow, and the CRC is rewritten. The new size.
inline size_t SetMorph(const uint8_t* image, const uint8_t* block, uint8_t* out) {
    using namespace detail;
    const uint16_t flags = dkbin::Le16(image + kOffFlags);
    const size_t body = MorphOffset((flags & kFlagHasIr) != 0);  // up to the morph block
    std::memcpy(out, image, body);
    const uint16_t kept = flags & ~(kFlagHasMorph | kFlagWaypointsMask);
    const uint16_t new_flags = block != nullptr ? kept | kFlagHasMorph : kept;
    out[kOffFlags] = uint8_t(new_flags);
    out[kOffFlags + 1] = uint8_t(new_flags >> 8);
    if (block != nullptr) std::memcpy(out + body, block, dkmorph::kBlockBytes);
    const size_t new_size = body + (block != nullptr ? dkmorph::kBlockBytes : 0) + 4;
    dkbin::PutLe32(out + kOffSize, uint32_t(new_size));
    Reseal(out, new_size);
    return new_size;
}

// The slot preset a morphs with, -1 for none: presets[k] is slot k's preset, valid where filled[k]. The block's
// target while it is filled, another slot and still holds the model the block was fitted to; else the first other
// filled slot that holds that model (the target moved); else none.
inline int ResolveMorph(const Preset* presets, const bool* filled, int a) {
    if (!filled[a] || !presets[a].has_morph) return -1;
    const dkmorph::Block& m = presets[a].morph;
    const int t = m.target;
    if (t != a && filled[t] && presets[t].model.crc == m.model_crc) return t;
    for (int k = 0; k < kSlots; ++k)
        if (k != a && filled[k] && presets[k].model.crc == m.model_crc) return k;
    return -1;
}

// The next filled slot after from, wrapping: from itself when it is the only filled slot; -1 when none is filled.
inline int NextFilled(const bool filled[kSlots], int from) {
    for (int i = 1; i <= kSlots; ++i) {
        const int k = (from + i) % kSlots;
        if (filled[k]) return k;
    }
    return -1;
}

// The slot to boot into: saved when it is in 0..7 and filled, else the first filled slot, else -1.
inline int BootSlot(const bool filled[kSlots], int saved) {
    if (saved >= 0 && saved < kSlots && filled[saved]) return saved;
    return NextFilled(filled, kSlots - 1);
}

}  // namespace dkpreset
