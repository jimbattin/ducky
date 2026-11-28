// The settings record (docs/FIRMWARE.md "Settings record"): the control slots' assignments, the expression pedal's
// and MIDI IN's MIDI channels and every parameter's value, global to the pedal (presets hold none of them), written by
// the link's SAVE into two 4 KB QSPI sectors after the preset store's state sectors. As with the state record, every
// write goes to the sector not holding the newest record, so a torn write leaves the previous one readable; at boot
// the valid record with the higher seq wins, and with none (or only records of another version) the factory layout
// (params.h kFactoryAssign) and the defaults apply. Host-portable (no libDaisy, no allocation), covered by
// tests/settings_test.cpp.
//
// Record (little-endian): "DKSG", u32 seq, u8 version (4), u8 control slots (9), u8 parameter count N, u8 expression
// channel (0 omni, 1-16), u8 MIDI IN channel (0 omni, 1-16), assign[9] (a parameter id or kNoParam per control slot),
// f32 value[N] (by id) from byte 22, then the CRC-32 of the bytes before it. A record from firmware with fewer
// parameters leaves the later ones at their defaults; one with more has its extra values ignored.
// Version 3 records (written before PEDAL) still decode: control slots 8, or 7 before the expression slot, the
// expression channel at byte 11 (0 in 7-slot records), assign[slots] from byte 12, values from byte 20; the slots they
// do not hold take the factory layout's, MIDI IN's channel is omni.
#pragma once

#include <cstddef>
#include <cstdint>

#include "binary.h"
#include "midi.h"
#include "params.h"
#include "preset.h"

namespace dkparam {

constexpr uint32_t kSettingsOffset[2] = {0x482000, 0x483000};  // QSPI chip offsets of the two settings sectors
constexpr uint32_t kSettingsMagic = 0x47534B44;                // "DKSG"
constexpr uint8_t kSettingsVersion = 4;  // 4: PEDAL and MIDI IN's channel (versions 1 and 2 are not read)
constexpr size_t kSettingsHeader = 22;
constexpr size_t kSettingsBytes = kSettingsHeader + 4 * kParamCount + 4;

static_assert(dkpreset::kStateOffset[1] + dkpreset::kStateSectorBytes <= kSettingsOffset[0],
              "the settings sectors follow the state sectors");
static_assert(kSettingsOffset[0] + dkpreset::kStateSectorBytes <= kSettingsOffset[1],
              "the settings sectors do not overlap");
static_assert(kSettingsOffset[1] + dkpreset::kStateSectorBytes <= dkpreset::kBankOffset + dkpreset::kMpuBytes,
              "the settings sectors lie inside MPU region 4");
static_assert(kControlSlots == 9, "the record's header holds 9 assignments");
static_assert(13 + kControlSlots == kSettingsHeader, "the header ends after the assignments");
constexpr uint8_t kSettingsVersionBeforePedal = 3;  // records written before PEDAL and MIDI IN's channel
constexpr size_t kSettingsHeaderBeforePedal = 20;
constexpr uint8_t kSettingsSlotsBeforePedal = 8;    // version 3's control slots
constexpr uint8_t kSettingsSlotsBeforeExp = 7;      // version 3's control slots before the expression slot
static_assert(kSettingsHeader + 4 * size_t(kNoParam - 1) + 4 <= dkpreset::kStateSectorBytes,
              "SettingsValid reads inside the sector for any parameter count it accepts");

struct Settings {
    uint8_t assign[kControlSlots];  // a parameter id or kNoParam per control slot
    uint8_t exp_channel;            // the expression pedal's MIDI channel: dkmidi::kOmni or 1-16
    uint8_t midi_in_channel;        // MIDI IN's channel (a BurningBungalow's): dkmidi::kOmni or 1-16
    float value[kParamCount];
};

// The factory layout (kFactoryAssign) and every parameter at its default: the settings with no record.
inline Settings FactorySettings() {
    Settings s;
    for (int c = 0; c < kControlSlots; ++c) s.assign[c] = kFactoryAssign[c];
    for (int i = 0; i < kParamCount; ++i) s.value[i] = kParams[i].def;
    s.exp_channel = dkmidi::kOmni;
    s.midi_in_channel = dkmidi::kOmni;
    return s;
}

// kSettingsBytes bytes into out.
inline void BuildSettings(uint8_t* out, uint32_t seq, const Settings& s) {
    dkbin::PutLe32(out, kSettingsMagic);
    dkbin::PutLe32(out + 4, seq);
    out[8] = kSettingsVersion;
    out[9] = kControlSlots;
    out[10] = kParamCount;
    out[11] = s.exp_channel;
    out[12] = s.midi_in_channel;
    for (int c = 0; c < kControlSlots; ++c) out[13 + c] = s.assign[c];
    for (int i = 0; i < kParamCount; ++i) {
        uint32_t bits;
        std::memcpy(&bits, &s.value[i], sizeof bits);
        dkbin::PutLe32(out + kSettingsHeader + 4 * i, bits);
    }
    dkbin::SealCrc(out, kSettingsBytes - 4);
}

// The header size of a record with kSettingsMagic: kSettingsHeader for version 4 with 9 control slots,
// kSettingsHeaderBeforePedal for version 3 with 8 (or kSettingsSlotsBeforeExp); 0 for anything else.
inline size_t SettingsHeader(const uint8_t* rec) {
    if (rec[8] == kSettingsVersion && rec[9] == kControlSlots) return kSettingsHeader;
    if (rec[8] == kSettingsVersionBeforePedal &&
        (rec[9] == kSettingsSlotsBeforePedal || rec[9] == kSettingsSlotsBeforeExp))
        return kSettingsHeaderBeforePedal;
    return 0;
}

// A record with kSettingsMagic, a version and control slot count SettingsHeader knows, a parameter count in
// 1..kNoParam - 1 and its CRC; seq is its field (untouched when invalid). rec is readable for a whole sector.
inline bool SettingsValid(const uint8_t* rec, uint32_t& seq) {
    if (dkbin::Le32(rec) != kSettingsMagic) return false;
    const size_t header = SettingsHeader(rec);
    if (header == 0) return false;
    const size_t n = rec[10];
    if (n < 1 || n >= kNoParam) return false;
    const size_t body = header + 4 * n;
    if (!dkbin::CrcOk(rec, body)) return false;
    seq = dkbin::Le32(rec + 4);
    return true;
}

// The record (0 for a, 1 for b) holding the newest valid settings, with its seq: the higher seq when both are valid;
// -1 for none (seq untouched).
inline int NewestSettings(const uint8_t* a, const uint8_t* b, uint32_t& seq) {
    uint32_t sa = 0, sb = 0;
    const bool va = SettingsValid(a, sa), vb = SettingsValid(b, sb);
    const int s = dkbin::NewestCopy(va, sa, vb, sb);
    if (s >= 0) seq = s == 1 ? sb : sa;
    return s;
}

// A valid record (SettingsValid) decoded: each stored value Quantize()d, the parameters it does not hold at their
// defaults, the slots it does not hold at the factory layout's; an assignment its slot does not accept (Accepts)
// becomes kNoParam; a channel past 16 becomes omni.
inline Settings DecodeSettings(const uint8_t* rec) {
    Settings s = FactorySettings();
    const size_t header = SettingsHeader(rec);
    const size_t assign = header == kSettingsHeader ? 13 : 12;
    const int n = rec[10] < kParamCount ? rec[10] : int(kParamCount);
    for (int i = 0; i < n; ++i) s.value[i] = Quantize(kParams[i], dkbin::LeF32(rec + header + 4 * i));
    for (int c = 0; c < rec[9]; ++c) s.assign[c] = Accepts(c, rec[assign + c]) ? rec[assign + c] : kNoParam;
    s.exp_channel = rec[11] <= dkmidi::kChannels ? rec[11] : dkmidi::kOmni;
    if (header == kSettingsHeader) s.midi_in_channel = rec[12] <= dkmidi::kChannels ? rec[12] : dkmidi::kOmni;
    return s;
}

}  // namespace dkparam
