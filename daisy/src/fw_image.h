// A firmware image for the FIRMWARE update (link.h, docs/MIDI.md "Firmware update"): the .bin of a pedal build, which
// the Daisy bootloader copies from QSPI 0x90040000 into AXI SRAM 0x24000000 and starts. It begins with the vector
// table; a Tag at kTagOffset (the linker script's .ducky_image, defined by ducky.cpp only) marks an image that can
// itself be updated this way. CheckImage is FIRMWARE COMMIT's check before anything is erased. Host-portable, covered
// by tests/fw_image_test.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

#include "binary.h"

namespace dkfwimg {

constexpr uint32_t kQspiOffset = 0x40000;      // the chip offset of 0x90040000
constexpr uint32_t kSramBase = 0x24000000;     // where the bootloader copies the image
constexpr uint32_t kMaxImageBytes = 0x78000;   // the linker script's SRAM: 512 KB - 32 KB
constexpr uint32_t kTagOffset = 0x400;         // after the vector table (the linker script asserts it fits)
constexpr uint32_t kTagBytes = 16;
constexpr uint32_t kMinImageBytes = kTagOffset + kTagBytes;
constexpr uint32_t kTagMagic = 0x57464B44;     // "DKFW", little-endian
constexpr uint32_t kTagVersion = 1;
constexpr uint32_t kStackLo = 0x20000000, kStackHi = 0x20020000;  // DTCM: the initial SP (_estack) lies in it
constexpr uint32_t kMpuWindowEnd = 0xC0000;    // the chip-offset end of the readable window (firmware.h region 5)
static_assert(kQspiOffset + kMaxImageBytes <= kMpuWindowEnd, "COMMIT's read-back stays in MPU region 5");

struct Tag {
    uint32_t magic, version;
    uint32_t board;  // dkboard::kBoardId
    uint32_t flags;  // the Identity Reply's flags (bit 0: dklink::kIdentityWeightMorph)
};
static_assert(sizeof(Tag) == kTagBytes, "the tag's layout");

enum class Check : uint8_t { kOk, kCrc, kVectors, kTag, kBoard };

inline const char* CheckName(Check c) {
    switch (c) {
        case Check::kOk: return "ok";
        case Check::kCrc: return "crc";
        case Check::kVectors: return "vectors";
        case Check::kTag: return "tag";
        case Check::kBoard: return "board";
    }
    return "?";
}

// The image's checks, in order: its CRC-32 is crc; its initial SP lies in DTCM and its reset vector is a Thumb address
// inside the image as loaded in SRAM; the tag's magic and version; the tag's board. The caller guarantees
// kMinImageBytes <= n <= kMaxImageBytes.
inline Check CheckImage(const uint8_t* img, size_t n, uint32_t crc, uint32_t board) {
    if (dkbin::Crc32(img, n) != crc) return Check::kCrc;
    const uint32_t sp = dkbin::Le32(img), reset = dkbin::Le32(img + 4);
    if (sp < kStackLo || sp > kStackHi || (reset & 1u) == 0) return Check::kVectors;
    const uint32_t entry = reset & ~1u;
    if (entry < kSramBase || entry >= kSramBase + n) return Check::kVectors;
    const uint8_t* t = img + kTagOffset;
    if (dkbin::Le32(t) != kTagMagic || dkbin::Le32(t + 4) != kTagVersion) return Check::kTag;
    if (dkbin::Le32(t + 8) != board) return Check::kBoard;
    return Check::kOk;
}

}  // namespace dkfwimg
