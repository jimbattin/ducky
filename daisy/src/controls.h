// Toggle hysteresis, LED2's blink pattern and the expression jack's scaling. Host-portable (no libDaisy, no
// allocation), covered by tests/controls_test.cpp. Toggle positions (board.h): 0 down, 1 middle, 2 up.
#pragma once

#include <cmath>
#include <cstdint>

#include "dsp/blocks.h"
#include "memory.h"

namespace dkctl {

// The control timings below count blocks; every whole millisecond is a whole number of blocks (dkdsp::kBlocksPerMs).
constexpr int BlocksForMs(int ms) { return ms * int(dkdsp::kBlocksPerMs); }

// A toggle accepts a new position only after it has read that position for kToggleStableBlocks consecutive blocks,
// so a lever moving up <-> down never reports the middle it passes through.
constexpr int kToggleStableBlocks = BlocksForMs(30);
class ToggleFilter {
public:
    void Init(uint8_t pos) {
        pos_ = candidate_ = pos;
        count_ = 0;
    }
    // Once per block with the raw position; returns the accepted position.
    DK_ITCM uint8_t Process(uint8_t raw) {
        if (raw == pos_) {
            count_ = 0;
            return pos_;
        }
        if (raw != candidate_) {
            candidate_ = raw;
            count_ = 1;
        } else {
            ++count_;
        }
        if (count_ >= kToggleStableBlocks) {
            pos_ = raw;
            count_ = 0;
        }
        return pos_;
    }

private:
    uint8_t pos_ = 1, candidate_ = 1;
    int count_ = 0;
};

// LED2's preset number: n blinks, each kBlinkOnBlocks on then kBlinkOffBlocks off, then kBlinkPauseBlocks off, repeated.
constexpr int kBlinkOnBlocks = BlocksForMs(150), kBlinkOffBlocks = BlocksForMs(150);
constexpr int kBlinkPauseBlocks = BlocksForMs(5000);  // 5 s dark between repeats
class Blinker {
public:
    // n blinks from the next Tick, repeating; restarts any pattern in progress; n <= 0 is off.
    void Start(int n) {
        n_ = remaining_ = n > 0 ? n : 0;
        phase_ = 0;
    }
    // Once per block: true (on) for the first kBlinkOnBlocks of each of the n periods.
    DK_ITCM bool Tick() {
        if (n_ == 0) return false;
        if (remaining_ == 0) {  // the pause before the next repeat
            if (++phase_ == kBlinkPauseBlocks) {
                phase_ = 0;
                remaining_ = n_;
            }
            return false;
        }
        const bool on = phase_ < kBlinkOnBlocks;
        if (++phase_ == kBlinkOnBlocks + kBlinkOffBlocks) {
            phase_ = 0;
            --remaining_;
        }
        return on;
    }

private:
    int n_ = 0, remaining_ = 0, phase_ = 0;
};

// The expression jack's reading (0..1) as pedal travel: kPedalHeel and below 0, kPedalToe and above 1, linear between.
// Room at the toe for a 10K-50K pedal (its ring is fed through 220R) and the Seed's ADC spread; an empty jack reads
// about 0.04 (no plug detection), about 1 % of travel.
constexpr float kPedalHeel = 0.03f, kPedalToe = 0.92f;
DK_ITCM inline float PedalTravel(float raw) {
    const float x = (raw - kPedalHeel) * (1.0f / (kPedalToe - kPedalHeel));
    return std::fmin(std::fmax(x, 0.0f), 1.0f);
}

}  // namespace dkctl
