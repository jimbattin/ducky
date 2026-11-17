// The settings record (docs/FIRMWARE.md "Settings record"): the control slots' assignments and every parameter's
// value, global to the pedal (presets hold neither), written by the link's SAVE into two 4 KB QSPI sectors after the
// preset store's state sectors. As with the state record, every write goes to the sector not holding the newest
// record, so a torn write leaves the previous one readable; at boot the valid record with the higher seq wins, and
// with none (or only records of another version) no control is assigned and the defaults apply. Host-portable (no
// libDaisy, no allocation), covered by tests/settings_test.cpp.
//
// Record (little-endian): "DKSG", u32 seq, u8 version (2), u8 control slots (7), u8 parameter count N, u8 0,
// assign[7] (a parameter id or kNoParam per control slot), u8 0, f32 value[N] (by id), then the CRC-32 of the bytes
// before it. A record from firmware with fewer parameters leaves the later ones at their defaults; one with more has
// its extra values ignored.
#pragma once

#include <cstddef>
#include <cstdint>

#include "binary.h"
#include "params.h"
#include "preset.h"

namespace dkparam {

constexpr uint32_t kSettingsOffset[2] = {0x482000, 0x483000};  // QSPI chip offsets of the two settings sectors
constexpr uint32_t kSettingsMagic = 0x47534B44;                // "DKSG"
constexpr uint8_t kSettingsVersion = 2;  // 2: the parametric EQ's table (version 1's ids are not read)
constexpr size_t kSettingsHeader = 20;
constexpr size_t kSettingsBytes = kSettingsHeader + 4 * kParamCount + 4;

static_assert(dkpreset::kStateOffset[1] + dkpreset::kStateSectorBytes <= kSettingsOffset[0],
              "the settings sectors follow the state sectors");
static_assert(kSettingsOffset[0] + dkpreset::kStateSectorBytes <= kSettingsOffset[1],
              "the settings sectors do not overlap");
static_assert(kSettingsOffset[1] + dkpreset::kStateSectorBytes <= dkpreset::kBankOffset + dkpreset::kMpuBytes,
              "the settings sectors lie inside MPU region 4");
static_assert(kControlSlots == 7, "the record's header holds 7 assignments");
static_assert(kSettingsHeader + 4 * size_t(kNoParam - 1) + 4 <= dkpreset::kStateSectorBytes,
              "SettingsValid reads inside the sector for any parameter count it accepts");

struct Settings {
    uint8_t assign[kControlSlots];  // a parameter id or kNoParam per control slot
    float value[kParamCount];
};

// No control assigned and every parameter at its default: the settings with no record.
inline Settings FactorySettings() {
    Settings s;
    for (int c = 0; c < kControlSlots; ++c) s.assign[c] = kNoParam;
    for (int i = 0; i < kParamCount; ++i) s.value[i] = kParams[i].def;
    return s;
}

// kSettingsBytes bytes into out.
inline void BuildSettings(uint8_t* out, uint32_t seq, const Settings& s) {
    dkbin::PutLe32(out, kSettingsMagic);
    dkbin::PutLe32(out + 4, seq);
    out[8] = kSettingsVersion;
    out[9] = kControlSlots;
    out[10] = kParamCount;
    out[11] = 0;
    for (int c = 0; c < kControlSlots; ++c) out[12 + c] = s.assign[c];
    out[19] = 0;
    for (int i = 0; i < kParamCount; ++i) {
        uint32_t bits;
        std::memcpy(&bits, &s.value[i], sizeof bits);
        dkbin::PutLe32(out + kSettingsHeader + 4 * i, bits);
    }
    dkbin::PutLe32(out + kSettingsBytes - 4, dkbin::Crc32(out, kSettingsBytes - 4));
}

// A record with kSettingsMagic, version kSettingsVersion, 7 control slots, a parameter count in 1..kNoParam - 1 and
// its CRC; seq is its field (untouched when invalid). rec is readable for a whole sector.
inline bool SettingsValid(const uint8_t* rec, uint32_t& seq) {
    if (dkbin::Le32(rec) != kSettingsMagic || rec[8] != kSettingsVersion || rec[9] != kControlSlots) return false;
    const size_t n = rec[10];
    if (n < 1 || n >= kNoParam) return false;
    const size_t body = kSettingsHeader + 4 * n;
    if (dkbin::Le32(rec + body) != dkbin::Crc32(rec, body)) return false;
    seq = dkbin::Le32(rec + 4);
    return true;
}

// The record (0 for a, 1 for b) holding the newest valid settings, with its seq: the higher seq when both are valid;
// -1 for none (seq untouched).
inline int NewestSettings(const uint8_t* a, const uint8_t* b, uint32_t& seq) {
    uint32_t sa = 0, sb = 0;
    const bool va = SettingsValid(a, sa), vb = SettingsValid(b, sb);
    const int s = va && vb ? (sb > sa ? 1 : 0) : va ? 0 : vb ? 1 : -1;
    if (s >= 0) seq = s == 1 ? sb : sa;
    return s;
}

// A valid record (SettingsValid) decoded: each stored value Quantize()d, the parameters it does not hold at their
// defaults; an assignment its slot does not accept (Accepts) becomes kNoParam.
inline Settings DecodeSettings(const uint8_t* rec) {
    Settings s = FactorySettings();
    const int n = rec[10] < kParamCount ? rec[10] : int(kParamCount);
    for (int i = 0; i < n; ++i) s.value[i] = Quantize(kParams[i], dkbin::LeF32(rec + kSettingsHeader + 4 * i));
    for (int c = 0; c < kControlSlots; ++c) s.assign[c] = Accepts(c, rec[12 + c]) ? rec[12 + c] : kNoParam;
    return s;
}

}  // namespace dkparam
