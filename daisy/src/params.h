// The parameter table and the control slots (docs/MIDI.md "Parameters", "Controls"). Knobs 1 and 2 are fixed (input
// trim, output volume); Knobs 3-6 and SW1-SW3 are control slots 0-6, each driving one parameter of kParams or none.
// Host-portable (no libDaisy, no allocation), covered by tests/params_test.cpp.
//
// A parameter's value:
// - continuous (Taper::kLog): its position 0..1 along the taper from min to max, what a knob at that position gives;
// - choice (Taper::kChoice): its index 0..max (max 1 or 2). A three-value choice's index is the toggle position
//   (controls.h: 0 down, 1 middle, 2 up); a two-value one is 0 with the toggle in the middle and 1 up or down.
// On the wire a value is a u14 code: lround(value * kCodeMax) for a continuous parameter, the index for a choice.
// Ids are append-only: the settings record (settings.h) and hosts store them.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "dsp/filters.h"
#include "memory.h"

namespace dkparam {

enum ParamId : uint8_t {
    kLowCutFreq,
    kLowCutRes,
    kLowCutSlope,
    kHighCutFreq,
    kHighCutRes,
    kHighCutSlope,
    kMid,
    kParamCount
};
enum class Taper : uint8_t { kLog = 0, kChoice = 1 };            // wire values
enum class OffEnd : uint8_t { kNone = 0, kMin = 1, kMax = 2 };   // wire values

constexpr uint8_t kNoParam = 0x7F;  // a control slot driving nothing
constexpr uint16_t kCodeMax = 16383;
// Control slots 0-3 are Knobs 3-6 (board knob index slot + kFirstSlotKnob), 4-6 are SW1-SW3.
constexpr int kControlSlots = 7, kKnobSlots = 4, kSwitchSlots = 3, kFirstSlotKnob = 2;
constexpr float kPickup = 0.03f;            // knob travel (0..1) that takes a parameter back from a hold
constexpr float kChoiceHysteresis = 0.05f;  // in index units: a knob on a choice boundary does not chatter
inline constexpr const char* kControlNames[kControlSlots] = {"k3", "k4", "k5", "k6", "sw1", "sw2", "sw3"};

struct ParamDef {
    uint8_t id;
    const char* name;   // the log's and tools/pedal.ts's
    const char* label;  // the editor's
    const char* unit;   // a choice's labels, comma-separated, in index order
    const char* group;
    float min, max;     // the taper's ends; a choice: 0 and its last index
    float def;          // a value
    Taper taper;
    OffEnd off;         // display only: the end within dkdsp::kBypassEnter of which the filter is bypassed
    uint8_t factory;    // the control slot of the factory layout, or kNoParam
};

inline constexpr ParamDef kParams[kParamCount] = {
    {kLowCutFreq, "low_cut_freq", "Low-cut frequency", "Hz", "EQ", dkdsp::kLowCutMinHz, dkdsp::kLowCutMaxHz, 0.0f,
     Taper::kLog, OffEnd::kMin, 2},
    {kLowCutRes, "low_cut_res", "Low-cut resonance", "Q", "EQ", dkdsp::kResMinQ, dkdsp::kResMaxQ, 0.5f, Taper::kLog,
     OffEnd::kNone, 3},
    {kLowCutSlope, "low_cut_slope", "Low-cut slope", "24 dB/oct,12 dB/oct,6 dB/oct", "EQ", 0.0f, 2.0f, 1.0f,
     Taper::kChoice, OffEnd::kNone, 4},
    {kHighCutFreq, "high_cut_freq", "High-cut frequency", "Hz", "EQ", dkdsp::kHighCutMinHz, dkdsp::kHighCutMaxHz,
     1.0f, Taper::kLog, OffEnd::kMax, 1},
    {kHighCutRes, "high_cut_res", "High-cut resonance", "Q", "EQ", dkdsp::kResMinQ, dkdsp::kResMaxQ, 0.5f,
     Taper::kLog, OffEnd::kNone, 0},
    {kHighCutSlope, "high_cut_slope", "High-cut slope", "24 dB/oct,12 dB/oct,6 dB/oct", "EQ", 0.0f, 2.0f, 1.0f,
     Taper::kChoice, OffEnd::kNone, 5},
    {kMid, "mid", "Mid 515 Hz", "-7 dB,off,+5 dB", "EQ", 0.0f, 2.0f, 1.0f, Taper::kChoice, OffEnd::kNone, 6},
};

constexpr bool TableValid() {
    for (int i = 0; i < kParamCount; ++i) {
        const ParamDef& d = kParams[i];
        if (d.id != i) return false;
        if (d.taper == Taper::kChoice) {
            if (d.min != 0.0f || (d.max != 1.0f && d.max != 2.0f)) return false;
        } else if (!(0.0f < d.min && d.min < d.max && 0.0f <= d.def && d.def <= 1.0f)) {
            return false;
        }
        if (d.factory == kNoParam) continue;
        if (d.factory >= kControlSlots) return false;
        if (d.factory >= kKnobSlots && d.taper != Taper::kChoice) return false;
        for (int j = 0; j < i; ++j)
            if (kParams[j].factory == d.factory) return false;
    }
    return true;
}
static_assert(TableValid(), "kParams: ids in order, valid ranges, one parameter per factory slot");
static_assert(kParamCount < kNoParam, "kNoParam is no parameter's id");
static_assert(kKnobSlots + kSwitchSlots == kControlSlots, "the knob slots, then the switch slots");

// Control slot `slot` may drive parameter id: any slot takes kNoParam, a knob any parameter, a switch a choice.
inline bool Accepts(int slot, uint8_t id) {
    if (slot < 0 || slot >= kControlSlots) return false;
    if (id == kNoParam) return true;
    return id < kParamCount && (slot < kKnobSlots || kParams[id].taper == Taper::kChoice);
}

// v as a value of d: clamped into range, a choice rounded to an index; a non-finite v becomes the default.
DK_ITCM inline float Quantize(const ParamDef& d, float v) {
    if (!std::isfinite(v)) return d.def;
    if (d.taper == Taper::kChoice) v = std::round(v);
    const float hi = d.taper == Taper::kChoice ? d.max : 1.0f;
    return v < 0.0f ? 0.0f : v > hi ? hi : v;
}

// The value a knob at `knob` gives d, whose value is `current`. A choice splits the travel evenly and changes only
// once the knob is kChoiceHysteresis past the boundary.
DK_ITCM inline float FromKnob(const ParamDef& d, float knob, float current) {
    const float k = std::fmin(std::fmax(knob, 0.0f), 1.0f);  // vmaxnm/vminnm: no compare and branch
    if (d.taper != Taper::kChoice) return k;
    const float x = k * d.max;
    return std::fabs(x - current) >= 0.5f + kChoiceHysteresis ? std::round(x) : current;
}

// The value a toggle at pos (0 down, 1 middle, 2 up) gives choice d: its index for three values; for two, off (0) in
// the middle and on (1) up or down.
DK_ITCM inline float FromToggle(const ParamDef& d, uint8_t pos) {
    if (d.max == 2.0f) return float(pos > 2 ? 2 : pos);
    return pos == 1 ? 0.0f : 1.0f;
}

// A value (Quantize()d) as its wire code.
inline uint16_t ToCode(const ParamDef& d, float v) {
    return d.taper == Taper::kChoice ? uint16_t(v) : uint16_t(lroundf(v * float(kCodeMax)));
}
// A wire code as a value; codes past the range clamp.
inline float FromCode(const ParamDef& d, uint32_t code) {
    if (d.taper == Taper::kChoice) return float(code < uint32_t(d.max) ? code : uint32_t(d.max));
    return float(code < kCodeMax ? code : kCodeMax) / float(kCodeMax);
}

// The parameter the factory layout puts on control slot `slot`, or kNoParam.
inline uint8_t FactoryParam(int slot) {
    for (const ParamDef& d : kParams)
        if (d.factory == slot) return d.id;
    return kNoParam;
}

}  // namespace dkparam
