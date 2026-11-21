// Little-endian fields and CRC-32 for the Ducky binaries (model.h, ir.h, preset.h) and the link (upload.h).
// Host-portable (no libDaisy, no allocation).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace dkbin {

// IEEE 802.3 / zlib CRC-32, bitwise (no table): reflected poly 0xEDB88320, init and final XOR 0xFFFFFFFF.
inline uint32_t Crc32(const uint8_t* data, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

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

inline void PutLe32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i));
}

inline void PutLeF32(uint8_t* p, float f) {
    uint32_t u;
    std::memcpy(&u, &f, sizeof u);
    PutLe32(p, u);
}

// The CRC-32 trailer after p[0, n): CrcOk checks it, SealCrc writes it.
inline bool CrcOk(const uint8_t* p, size_t n) { return Le32(p + n) == Crc32(p, n); }
inline void SealCrc(uint8_t* p, size_t n) { PutLe32(p + n, Crc32(p, n)); }

// Of two record copies (valid flags, sequence numbers): 0 or 1 for the newest valid one, the higher seq when both are
// (a on a tie); -1 for neither.
inline int NewestCopy(bool va, uint32_t sa, bool vb, uint32_t sb) {
    return va && vb ? (sb > sa ? 1 : 0) : va ? 0 : vb ? 1 : -1;
}

}  // namespace dkbin
