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

    // out[i] = in[i] * gain, the gain first stepping by 1 / kRampSamples toward the target and clamped to it (it lands
    // exactly on 0 or 1). in and out may alias.
    DK_ITCM void Process(const float* in, float* out, size_t n) {
        constexpr float kStep = 1.0f / kRampSamples;
        for (size_t i = 0; i < n; ++i) {
            if (gain_ < target_)
                gain_ = std::min(gain_ + kStep, target_);
            else if (gain_ > target_)
                gain_ = std::max(gain_ - kStep, target_);
            out[i] = in[i] * gain_;
        }
    }

private:
    float gain_ = 1.0f, target_ = 1.0f;
};

}  // namespace dkdsp
