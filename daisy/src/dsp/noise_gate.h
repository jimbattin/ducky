// The per-preset noise gate: a side-chained downward expander run once per block. The detector reads the block's
// input peak (the guitar at the jack, before Knob 1, the model and the IR); the gain it returns is folded into the
// output gain ramp, so the gate costs nothing per sample and adds no latency. Host-portable (no libDaisy, no
// allocation), covered by tests/noise_gate_test.cpp.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "../memory.h"
#include "blocks.h"

namespace dkdsp {

// A preset's gate, in the units it is stored in (preset.h's gate block).
struct GateSettings {
    bool enabled;
    uint8_t threshold;  // dB below full scale, 30..80: closes below it, opens kGateHysteresisDb above
    uint8_t attack;     // 0.1 ms, 5..100
    uint8_t hold;       // ms, 10..100
    uint16_t release;   // ms, 10..1000: the time to fall 60 dB
    uint8_t ratio;      // 0.1, 15..100
};

inline constexpr GateSettings kGateOff{false, 60, 5, 20, 100, 40};
constexpr float kGateHysteresisDb = 6.0f;
constexpr float kGateSilence = 1e-5f;  // -100 dB: gains below it are 0
constexpr size_t kGateMaxHoldBlocks = 100 * kBlocksPerMs;

// Every field inside its range.
inline bool GateValid(const GateSettings& s) {
    return s.threshold >= 30 && s.threshold <= 80 && s.attack >= 5 && s.attack <= 100 && s.hold >= 10 &&
           s.hold <= 100 && s.release >= 10 && s.release <= 1000 && s.ratio >= 15 && s.ratio <= 100;
}

// Each field clamped into its range.
inline GateSettings ClampGate(GateSettings s) {
    auto clamp = [](unsigned v, unsigned lo, unsigned hi) { return v < lo ? lo : v > hi ? hi : v; };
    s.threshold = uint8_t(clamp(s.threshold, 30, 80));
    s.attack = uint8_t(clamp(s.attack, 5, 100));
    s.hold = uint8_t(clamp(s.hold, 10, 100));
    s.release = uint16_t(clamp(s.release, 10, 1000));
    s.ratio = uint8_t(clamp(s.ratio, 15, 100));
    return s;
}

class NoiseGate {
public:
    // Cold. The state is kept, so a change (enabling and disabling included) glides through the ballistics. s within
    // GateValid's ranges (preset.h's GetGate clamps, link.h's ParseGate checks): hold 0 would never end the window.
    void Set(const GateSettings& s) {
        constexpr float kBlockMs = 1000.0f * float(kBlockSize) / kSampleRate;
        enabled_ = s.enabled;
        close_ = std::pow(10.0f, -float(s.threshold) / 20.0f);
        open_thr_ = close_ * std::pow(10.0f, kGateHysteresisDb / 20.0f);
        exp_ = float(s.ratio) * 0.1f - 1.0f;
        attack_step_ = std::fmin(1.0f, kBlockMs / (0.1f * float(s.attack)));
        release_ = std::exp(-std::log(1000.0f) * kBlockMs / float(s.release));
        const size_t n = size_t(s.hold) * kBlocksPerMs;
        if (n != n_) {
            // The window restarts as n blocks at the held level: one entry, which the n-th block from here drops.
            n_ = n;
            dqHead_ = 0;
            dqCount_ = 1;
            dqVal_[0] = level_;
            dqIdx_[0] = k_ - 1;
        }
    }

    // Once per block with the block's input peak; returns the gain for the end of the block.
    DK_ITCM float Process(float peak) {
        // The largest peak of the last n_ blocks. The deque holds, oldest first, the peaks no later peak has reached
        // (each with its block number), so they fall from front to back and the front is the maximum: a few steps a
        // block instead of a scan of the window.
        const float p = std::fmax(peak, 0.0f);
        while (dqCount_ != 0) {
            const size_t back = dqHead_ + dqCount_ - 1;
            if (dqVal_[back < kDequeSize ? back : back - kDequeSize] > p) break;
            --dqCount_;
        }
        const size_t at = dqHead_ + dqCount_ < kDequeSize ? dqHead_ + dqCount_ : dqHead_ + dqCount_ - kDequeSize;
        dqVal_[at] = p;
        dqIdx_[at] = k_;
        ++dqCount_;
        while (k_ - dqIdx_[dqHead_] >= n_) {  // never the entry just pushed
            dqHead_ = dqHead_ + 1 == kDequeSize ? 0 : dqHead_ + 1;
            --dqCount_;
        }
        ++k_;
        const float env = dqVal_[dqHead_];
        level_ = env;

        if (!enabled_ || env >= open_thr_) {
            open_ = true;
        } else if (open_ && env < close_) {
            open_ = false;
            floor_ = 1.0f;
        }

        float target = 1.0f;
        if (!open_) {
            if (floor_ > 0.0f && env < close_) floor_ = std::fmin(floor_, std::pow(env / close_, exp_));
            if (floor_ < kGateSilence) floor_ = 0.0f;
            target = floor_;
        }

        if (target > g_) {
            g_ = std::fmin(target, g_ + attack_step_);
        } else {
            g_ = target + (g_ - target) * release_;
            if (g_ < kGateSilence) g_ = 0.0f;
        }
        return g_;
    }

    // The held detector level, linear.
    float level() const { return level_; }

private:
    static constexpr size_t kDequeSize = kGateMaxHoldBlocks + 1;  // a full window and the block's peak
    float dqVal_[kDequeSize] = {};     // the deque's peaks, a ring from dqHead_
    uint32_t dqIdx_[kDequeSize] = {};  // and the block each arrived in
    size_t n_ = 40, dqHead_ = 0, dqCount_ = 0;
    uint32_t k_ = 0;  // blocks processed (wraps)
    bool enabled_ = false, open_ = true;
    float close_ = 1e-3f, open_thr_ = 2e-3f, exp_ = 3.0f, attack_step_ = 1.0f, release_ = 0.0f, floor_ = 1.0f,
          g_ = 1.0f, level_ = 0.0f;
};

}  // namespace dkdsp
