// The parameter table and the control slots (docs/MIDI.md "Parameters", "Controls"). Knobs 1 and 2 are fixed (input
// trim, output volume); Knobs 3-6 and SW1-SW3 are control slots 0-6, each driving one parameter of kParams or none.
// Host-portable (no libDaisy, no allocation), covered by tests/params_test.cpp.
//
// A parameter's value:
// - continuous (Taper::kLog, Taper::kLinear): its position 0..1 along the taper from min to max, what a knob at that
//   position gives (Units() converts it);
// - choice (Taper::kChoice): its index 0..max (max 1 or 2). A three-value choice's index is the toggle position
//   (controls.h: 0 down, 1 middle, 2 up); a two-value one is 0 with the toggle in the middle and 1 up or down.
// On the wire a value is a u14 code: lround(value * kCodeMax) for a continuous parameter, the index for a choice.
// Ids are append-only: the settings record (settings.h) and hosts store them.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

#include "dsp/filters.h"
#include "dsp/gain.h"
#include "memory.h"

namespace dkparam {

enum ParamId : uint8_t {
    kEqEnable,
    kLowCutFreq,
    kLowCutRes,
    kLowCutSlope,
    kBell1Gain,
    kBell1Freq,
    kBell1Q,
    kBell2Gain,
    kBell2Freq,
    kBell2Q,
    kBell3Gain,
    kBell3Freq,
    kBell3Q,
    kHighCutFreq,
    kHighCutRes,
    kHighCutSlope,
    kCompMode,
    kCompSustain,
    kCompLevel,
    kCompBalance,
    kLoopMode,
    kLoopSend,
    kLoopReturn,
    kMorph,
    kParamCount
};
enum class Taper : uint8_t { kLog = 0, kChoice = 1, kLinear = 2 };  // wire values
enum class OffEnd : uint8_t { kNone = 0, kMin = 1, kMax = 2 };      // wire values

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
};

inline constexpr const char* kSlopeLabels = "24 dB/oct,12 dB/oct,6 dB/oct";  // the cut slopes' choice labels
constexpr uint8_t kLoopMiso = 0, kLoopPre = 1, kLoopPost = 2;               // loop_mode's indices
constexpr uint8_t kCompOff = 0, kCompBlend = 1, kCompDirect = 2;            // comp_mode's indices
constexpr float kCompSustainMaxDb = 20.827854f;  // 20 log10(11): the stage's gain at Sustain's 100 kOhm (compressor.h)
// The bells' default positions: 250 Hz, 515 Hz (an amp's usual mid) and 3 kHz, Q 1. The compressor's: Off, Sustain
// +15.6 dB (position 0.75), Level 0 dB (the toggle's middle), Balance fully compressed. comp_mode is both the stage's
// switch and the output's mix, so one toggle sets it: down Off, middle Blend, up Direct. Balance is the knob's
// rotation; the DSP applies the audio taper. The loop's (docs/LOOP.md): MISO, send and return 0 dB at noon. Morph
// (morph.h): the preset's own model.
inline constexpr ParamDef kParams[kParamCount] = {
    {kEqEnable, "eq_enable", "EQ", "off,on", "EQ", 0.0f, 1.0f, 1.0f, Taper::kChoice, OffEnd::kNone},
    {kLowCutFreq, "low_cut_freq", "Low-cut frequency", "Hz", "EQ", dkdsp::kLowCutMinHz, dkdsp::kLowCutMaxHz, 0.0f,
     Taper::kLog, OffEnd::kMin},
    {kLowCutRes, "low_cut_res", "Low-cut resonance", "Q", "EQ", dkdsp::kResMinQ, dkdsp::kResMaxQ, 0.5f, Taper::kLog,
     OffEnd::kNone},
    {kLowCutSlope, "low_cut_slope", "Low-cut slope", kSlopeLabels, "EQ", 0.0f, 2.0f, 1.0f, Taper::kChoice,
     OffEnd::kNone},
    {kBell1Gain, "bell1_gain", "Bell 1 gain", "dB", "EQ", -dkdsp::kBellGainDb, dkdsp::kBellGainDb, 0.5f,
     Taper::kLinear, OffEnd::kNone},
    {kBell1Freq, "bell1_freq", "Bell 1 frequency", "Hz", "EQ", dkdsp::kBellMinHz[0], dkdsp::kBellMaxHz[0], 0.61175f,
     Taper::kLog, OffEnd::kNone},
    {kBell1Q, "bell1_q", "Bell 1 Q", "Q", "EQ", dkdsp::kBellQMin, dkdsp::kBellQMax, 0.30587f, Taper::kLog,
     OffEnd::kNone},
    {kBell2Gain, "bell2_gain", "Bell 2 gain", "dB", "EQ", -dkdsp::kBellGainDb, dkdsp::kBellGainDb, 0.5f,
     Taper::kLinear, OffEnd::kNone},
    {kBell2Freq, "bell2_freq", "Bell 2 frequency", "Hz", "EQ", dkdsp::kBellMinHz[1], dkdsp::kBellMaxHz[1], 0.29384f,
     Taper::kLog, OffEnd::kNone},
    {kBell2Q, "bell2_q", "Bell 2 Q", "Q", "EQ", dkdsp::kBellQMin, dkdsp::kBellQMax, 0.30587f, Taper::kLog,
     OffEnd::kNone},
    {kBell3Gain, "bell3_gain", "Bell 3 gain", "dB", "EQ", -dkdsp::kBellGainDb, dkdsp::kBellGainDb, 0.5f,
     Taper::kLinear, OffEnd::kNone},
    {kBell3Freq, "bell3_freq", "Bell 3 frequency", "Hz", "EQ", dkdsp::kBellMinHz[2], dkdsp::kBellMaxHz[2], 0.48809f,
     Taper::kLog, OffEnd::kNone},
    {kBell3Q, "bell3_q", "Bell 3 Q", "Q", "EQ", dkdsp::kBellQMin, dkdsp::kBellQMax, 0.30587f, Taper::kLog,
     OffEnd::kNone},
    {kHighCutFreq, "high_cut_freq", "High-cut frequency", "Hz", "EQ", dkdsp::kHighCutMinHz, dkdsp::kHighCutMaxHz,
     1.0f, Taper::kLog, OffEnd::kMax},
    {kHighCutRes, "high_cut_res", "High-cut resonance", "Q", "EQ", dkdsp::kResMinQ, dkdsp::kResMaxQ, 0.5f,
     Taper::kLog, OffEnd::kNone},
    {kHighCutSlope, "high_cut_slope", "High-cut slope", kSlopeLabels, "EQ", 0.0f, 2.0f, 1.0f, Taper::kChoice,
     OffEnd::kNone},
    {kCompMode, "comp_mode", "Compressor mode", "Off,Blend,Direct", "Compressor", 0.0f, 2.0f, 0.0f, Taper::kChoice,
     OffEnd::kNone},
    {kCompSustain, "comp_sustain", "Sustain", "dB", "Compressor", 0.0f, kCompSustainMaxDb, 0.75f, Taper::kLinear,
     OffEnd::kNone},
    {kCompLevel, "comp_level", "Level", "-3 dB,0 dB,+3 dB", "Compressor", 0.0f, 2.0f, 1.0f, Taper::kChoice,
     OffEnd::kNone},
    {kCompBalance, "comp_balance", "Balance", "%", "Compressor", 0.0f, 100.0f, 1.0f, Taper::kLinear, OffEnd::kNone},
    {kLoopMode, "loop_mode", "Loop mode", "MISO,pre-model,post-model", "Loop", 0.0f, 2.0f, 0.0f, Taper::kChoice,
     OffEnd::kNone},
    {kLoopSend, "loop_send", "Send level", "dB", "Loop", -dkdsp::kTrimRangeDb, dkdsp::kTrimRangeDb, 0.5f,
     Taper::kLinear, OffEnd::kNone},
    {kLoopReturn, "loop_return", "Return level", "dB", "Loop", -dkdsp::kTrimRangeDb, dkdsp::kTrimRangeDb, 0.5f,
     Taper::kLinear, OffEnd::kNone},
    {kMorph, "morph", "Morph", "%", "Morph", 0.0f, 100.0f, 0.0f, Taper::kLinear, OffEnd::kNone},
};

constexpr bool TableValid() {
    for (int i = 0; i < kParamCount; ++i) {
        const ParamDef& d = kParams[i];
        if (d.id != i) return false;
        if (d.taper == Taper::kChoice) {
            if (d.min != 0.0f || (d.max != 1.0f && d.max != 2.0f)) return false;
        } else if (!(d.min < d.max && 0.0f <= d.def && d.def <= 1.0f) ||
                   (d.taper == Taper::kLog && !(0.0f < d.min))) {
            return false;
        }
    }
    return true;
}
static_assert(TableValid(), "kParams: ids in order, valid ranges");
static_assert(kParamCount < kNoParam, "kNoParam is no parameter's id");
static_assert(kKnobSlots + kSwitchSlots == kControlSlots, "the knob slots, then the switch slots");

// A continuous value v of d in d's units: min (max / min)^v for a log taper, min + v (max - min) for a linear one.
// Not for a choice.
inline float Units(const ParamDef& d, float v) {
    if (d.taper == Taper::kLinear) return d.min + v * (d.max - d.min);
    return d.min * std::pow(d.max / d.min, v);
}

// Control slot `slot` may drive parameter id: any slot takes kNoParam, a knob any parameter, a switch a choice.
constexpr bool Accepts(int slot, uint8_t id) {
    if (slot < 0 || slot >= kControlSlots) return false;
    if (id == kNoParam) return true;
    return id < kParamCount && (slot < kKnobSlots || kParams[id].taper == Taper::kChoice);
}

// The factory layout, by control slot: what Knobs 3-6 and SW1-SW3 drive with no settings record.
inline constexpr uint8_t kFactoryAssign[kControlSlots] = {
    kBell2Gain,    // K3
    kLowCutFreq,   // K4
    kHighCutFreq,  // K5
    kCompBalance,  // K6
    kLowCutSlope,  // SW1
    kCompMode,     // SW2: down Off, middle Blend, up Direct
    kCompLevel,    // SW3
};
constexpr bool FactoryValid() {
    for (int c = 0; c < kControlSlots; ++c)
        if (!Accepts(c, kFactoryAssign[c])) return false;
    return true;
}
static_assert(FactoryValid(), "kFactoryAssign: a choice on each switch, a parameter or none on each slot");

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

}  // namespace dkparam
