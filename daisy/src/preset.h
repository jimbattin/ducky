// The preset store (docs/FIRMWARE.md "Preset store"): 8 slots of 64 KB in QSPI from 0x90400000, each a format-1
// preset (a model binary, an optional IR binary, a name, a CRC), and a two-copy state record holding the last slot.
// Host-portable (no libDaisy, no allocation), covered by tests/preset_test.cpp. The slot format is the contract with
// editor/src/preset/bank.ts.
//
// Slot (format 1, little-endian): "DKPS", u16 version, u16 flags (bit 0 kFlagHasIr), u32 size (kSizeNoIr, or
// kSizeWithIr with the IR flag), name[64] (NUL-terminated printable ASCII), the model binary (dkmodel::kBinSize),
// the IR binary (dkir::kBinSize) when flagged, then the CRC-32 of bytes [0, size - 4). Erased flash (0xFF) follows.
//
// State record (16 bytes at the start of each 4 KB state sector): "DKST", u32 seq, u32 slot (0..7), u32 CRC-32 of the
// first 12 bytes. Every write goes to the sector not holding the newest record, so a torn write leaves the previous
// record readable; at boot the valid record with the higher seq wins.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "ir.h"
#include "model.h"

namespace dkpreset {

constexpr uint32_t kQspiBase = 0x90000000;                // memory-mapped QSPI
constexpr uint32_t kBankOffset = 0x400000;                // QSPI chip offset of slot 0 (0x90400000)
constexpr int kSlots = 8;
constexpr uint32_t kSlotBytes = 0x10000;                  // one 64 KB erase block per slot
constexpr uint32_t kBankBytes = kSlots * kSlotBytes;      // 0x80000, the DFU bank image
constexpr uint32_t kStateOffset[2] = {0x480000, 0x481000};  // QSPI chip offsets of the two state sectors
constexpr uint32_t kStateSectorBytes = 0x1000;            // 4 KB sector, erased as a whole
constexpr uint32_t kMpuBytes = 0x100000;                  // firmware.h region 4: 0x90400000-0x904FFFFF
constexpr uint16_t kFormatVersion = 1;
constexpr uint16_t kFlagHasIr = 0x0001;
constexpr size_t kNameSize = 64;
constexpr size_t kHeaderSize = 76;
constexpr size_t kSizeNoIr = kHeaderSize + dkmodel::kBinSize + 4;  // 7732
constexpr size_t kSizeWithIr = kSizeNoIr + dkir::kBinSize;         // 16024
constexpr size_t kStateBytes = 16;
constexpr uint32_t kStateMagic = 0x54534B44;  // "DKST"

static_assert(kBankOffset + kBankBytes <= kStateOffset[0], "the slots end before the state sectors");
static_assert(kStateOffset[0] + kStateSectorBytes <= kStateOffset[1], "the state sectors do not overlap");
static_assert(kStateOffset[1] + kStateSectorBytes <= kBankOffset + kMpuBytes, "the store lies inside MPU region 4");
static_assert(kSizeWithIr <= kSlotBytes, "a preset fits its slot");
static_assert(kSizeNoIr == 7732 && kSizeWithIr == 16024, "the format-1 sizes");

enum class Status : uint8_t { kOk, kEmpty, kMagic, kVersion, kSize, kCrc, kModel, kIr };

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
    }
    return "?";
}

// No initialisers: it lives in DK_SDRAM.
struct Preset {
    bool has_ir;
    char name[kNameSize];  // NUL-terminated printable ASCII
    dkmodel::Model model;
    dkir::Ir ir;  // valid only with has_ir
};

namespace detail {

constexpr size_t kOffVersion = 4, kOffFlags = 6, kOffSize = 8, kOffName = 12, kOffModel = kHeaderSize,
                 kOffIr = kHeaderSize + dkmodel::kBinSize;
static_assert(kOffName + kNameSize == kHeaderSize, "name, then the model");
static_assert(kOffIr == 7728, "the IR follows the model");

inline void PutLe32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

}  // namespace detail

// Validates and decodes the slot data[0, n). *out holds valid data only on kOk.
inline Status Parse(const uint8_t* data, size_t n, Preset* out) {
    using namespace detail;
    using dkmodel::detail::Le16;
    using dkmodel::detail::Le32;
    if (n < 4) return Status::kSize;
    if (data[0] == 0xFF && data[1] == 0xFF && data[2] == 0xFF && data[3] == 0xFF) return Status::kEmpty;
    if (std::memcmp(data, "DKPS", 4) != 0) return Status::kMagic;
    if (n < kHeaderSize) return Status::kSize;
    const uint16_t flags = Le16(data + kOffFlags);
    if (Le16(data + kOffVersion) != kFormatVersion || (flags & ~kFlagHasIr) != 0) return Status::kVersion;
    const bool has_ir = (flags & kFlagHasIr) != 0;
    const uint32_t size = Le32(data + kOffSize);
    if (size != (has_ir ? kSizeWithIr : kSizeNoIr) || size > n) return Status::kSize;
    if (Le32(data + size - 4) != dkmodel::Crc32(data, size - 4)) return Status::kCrc;
    if (dkmodel::Parse(data + kOffModel, dkmodel::kBinSize, &out->model) != dkmodel::Status::kOk) return Status::kModel;
    if (has_ir && dkir::Parse(data + kOffIr, dkir::kBinSize, &out->ir) != dkir::Status::kOk) return Status::kIr;
    out->has_ir = has_ir;
    std::memcpy(out->name, data + kOffName, kNameSize);
    out->name[kNameSize - 1] = '\0';
    return Status::kOk;
}

// A record with kStateMagic, its CRC and slot < kSlots; seq and slot are its fields (untouched when invalid).
inline bool StateValid(const uint8_t* rec, uint32_t& seq, int& slot) {
    using dkmodel::detail::Le32;
    if (Le32(rec) != kStateMagic || Le32(rec + 12) != dkmodel::Crc32(rec, 12)) return false;
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
    detail::PutLe32(out, kStateMagic);
    detail::PutLe32(out + 4, seq);
    detail::PutLe32(out + 8, uint32_t(slot));
    detail::PutLe32(out + 12, dkmodel::Crc32(out, 12));
}

// Sets the name of the preset image[0, size) (a parsed slot, size its size field): name[64] zero-filled, then len
// (<= kNameSize - 1) bytes of name, and the CRC at size - 4 rewritten.
inline void Rename(uint8_t* image, size_t size, const char* name, size_t len) {
    if (len > kNameSize - 1) len = kNameSize - 1;
    std::memset(image + detail::kOffName, 0, kNameSize);
    std::memcpy(image + detail::kOffName, name, len);
    detail::PutLe32(image + size - 4, dkmodel::Crc32(image, size - 4));
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
