// Knob 1 (input trim), Knob 2 (output volume) and the model's level match. Host-portable (no libDaisy, no allocation),
// covered by tests/gain_test.cpp.
#pragma once

#include <cmath>
#include <cstddef>

#include "../memory.h"

namespace dkdsp {

constexpr float kTrimRangeDb = 12.0f;     // Knob 1: -12..+12 dB around the input calibration, 0 at noon; Knob 2's top
constexpr float kLevelTargetDb = -18.0f;  // NeuralAmpModelerPlugin's normalisation target loudness
constexpr float kKnobSmoothS = 0.02f;     // knob smoothing time constant
// The level at the input jack, in dBu (RMS of a sine), that reaches 0 dBFS. From the datasheets, not measured: the
// PCM3060's full scale is 0.6 VCC Vpp (VCC 4.5 V on the Seed 1.2), behind the Seed's 3.6 kOhm series resistor into the
// codec's 10 kOhm input, 3.67 Vpp; the HotHouse input is a unity-gain buffer. The Seed 3 (TAC5242) uses it too: its
// datasheet gives 3.6 Vpp at the pin (+4.3 dBu), and its loop round trip is within 0.5 dB of the Seed 1.2's
// (docs/MEASUREMENTS.md "FX loop").
constexpr float kInputLevelDbu = 4.5f;
// The capture level assumed for a model without input_level_dbu: NeuralAmpModelerPlugin's default input calibration
// level. Assuming Ducky's own level instead would drive such a model ~10 dB harder than a typical capture.
constexpr float kDefaultModelInputDbu = 12.0f;

inline float DbToGain(float db) { return std::pow(10.0f, db * 0.05f); }

// Knob 1, 0..1 -> linear gain, dB linear in the knob: -12 dB at 0, exactly 1 at noon, +12 dB at 1.
inline float InputGain(float knob) { return DbToGain(kTrimRangeDb * (2.0f * knob - 1.0f)); }

// Knob 2, 0..1 -> linear gain: up to noon (2 knob)^3 (exactly 0 at 0, -18 dB at 9 o'clock, exactly 1 at noon);
// above noon dB linear in the knob up to +12 dB at 1.
inline float OutputGain(float knob) {
    if (knob <= 0.5f) {
        const float t = 2.0f * knob;
        return t * t * t;
    }
    return DbToGain(kTrimRangeDb * (2.0f * knob - 1.0f));
}

// comp_balance's audio taper (81^v - 1) / 80: 0.1 at noon, 0 and 1 at the ends. Both compressors' wet share.
inline float CompBalanceTaper(float v) { return (std::pow(81.0f, v) - 1.0f) / 80.0f; }

// A one-pole smoother's coefficient at rate_hz with time constant tau_s, in T's precision.
template <class T>
inline T OnePoleAlpha(T rate_hz, T tau_s) {
    return T(1) - std::exp(T(-1) / (tau_s * rate_hz));
}

// The gain that brings a model of `loudness_db` to kLevelTargetDb; 1 (unity) for a model without loudness.
inline float LevelMatchGain(bool has_loudness, float loudness_db) {
    return has_loudness ? DbToGain(kLevelTargetDb - loudness_db) : 1.0f;
}

// NeuralAmpModelerPlugin's input calibration: the gain that brings a signal at the jack to the digital level it had at
// the model's capture, whose 0 dBFS was `input_level_dbu`; kDefaultModelInputDbu for a model without one.
inline float InputCalibrationGain(bool has_input_level, float input_level_dbu) {
    return DbToGain(kInputLevelDbu - (has_input_level ? input_level_dbu : kDefaultModelInputDbu));
}

// One-pole smoothing of a control updated at rate_hz with time constant tau_s.
class Smoother {
public:
    void Init(float rate_hz, float tau_s, float value) {
        a_ = OnePoleAlpha(rate_hz, tau_s);
        y_ = value;
    }
    DK_ITCM float Process(float target) {
        y_ += a_ * (target - y_);
        return y_;
    }

private:
    float a_ = 1.0f, y_ = 0.0f;
};

// A gain set once per block and ramped linearly across it, so a knob change never steps the signal.
class GainRamp {
public:
    void Init(float gain) { g_ = gain; }
    // out[i] = in[i] * g_i, where g_i moves linearly from the previous call's target to `target` and the last sample's
    // gain is exactly target. n = 0 is a no-op; in and out may alias. A constant gain of 1 is bit exact.
    DK_ITCM void Process(const float* in, float* out, size_t n, float target) {
        if (n == 0) return;
        const float g0 = g_, step = (target - g0) / float(n);
        if (target == g0) {
            // A resting gain: step is a zero, so every g_i is g0 + step (g0, or +0 for a zero of either sign).
            const float g = g0 + step;
            for (size_t i = 0; i + 1 < n; ++i) out[i] = in[i] * g;
        } else {
            for (size_t i = 0; i + 1 < n; ++i) out[i] = in[i] * (g0 + step * float(i + 1));
        }
        out[n - 1] = in[n - 1] * target;
        g_ = target;
    }

private:
    float g_ = 1.0f;
};

}  // namespace dkdsp
