// FS1's mute and the preset switch's fade. Host-portable (no libDaisy, no allocation), covered by tests/mute_test.cpp.
#pragma once

#include <algorithm>
#include <cstddef>

#include "../memory.h"

namespace dkdsp {

// A linear gain ramp between 1 and 0 over kRampSamples, applied per sample, so the result does not depend on the
// block size. At gain 1 the output equals the input bit for bit.
class MuteRamp {
public:
    static constexpr int kRampSamples = 240;  // 5 ms at 48 kHz

    // No ramp: the gain starts at the target.
    void Init(bool muted) { gain_ = target_ = muted ? 0.0f : 1.0f; }
    // A change mid-ramp reverses from the current gain.
    void SetMuted(bool muted) { target_ = muted ? 0.0f : 1.0f; }
    // The gain has landed on 0 (the ramp lands exactly on its target).
    bool Silent() const { return gain_ == 0.0f; }
    // The gain rests on its target, 0 or 1: Process and ProcessPair then multiply by Gain() and change no state, so a
    // caller may fold the multiply into another or, at 1, skip it.
    bool Settled() const { return gain_ == target_; }
    float Gain() const { return gain_; }

    // out[i] = in[i] * gain, the gain first stepping by 1 / kRampSamples toward the target and clamped to it (it lands
    // exactly on 0 or 1). in and out may alias.
    DK_ITCM void Process(const float* in, float* out, size_t n) {
        float g = gain_;
        const float t = target_;
        for (size_t i = 0; i < n; ++i) {
            g = Step(g, t);
            out[i] = in[i] * g;
        }
        gain_ = g;
    }

    // a[i] *= gain and b[i] *= gain with one gain sequence, stepping as Process: both buffers in place with one ramp,
    // so the send and the output fade together.
    DK_ITCM void ProcessPair(float* a, float* b, size_t n) {
        float g = gain_;
        const float t = target_;
        for (size_t i = 0; i < n; ++i) {
            g = Step(g, t);
            a[i] *= g;
            b[i] *= g;
        }
        gain_ = g;
    }

private:
    static constexpr float kStep = 1.0f / kRampSamples;

    // g stepped by kStep toward t, clamped to it.
    [[gnu::always_inline]] static float Step(float g, float t) {
        if (g < t) return std::min(g + kStep, t);
        if (g > t) return std::max(g - kStep, t);
        return g;
    }

    float gain_ = 1.0f, target_ = 1.0f;
};

}  // namespace dkdsp
