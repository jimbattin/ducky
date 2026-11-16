// Toggle hysteresis and LED2's blink pattern. Host-portable (no libDaisy, no allocation), covered by
// tests/controls_test.cpp. Toggle positions (board.h): 0 down, 1 middle, 2 up.
#pragma once

#include <cstdint>

namespace dkctl {

// A toggle accepts a new position only after it has read that position for kToggleStableBlocks consecutive blocks,
// so a lever moving up <-> down never reports the middle it passes through.
constexpr int kToggleStableBlocks = 30;  // 30 ms at the 1 kHz block rate
class ToggleFilter {
public:
    void Init(uint8_t pos) {
        pos_ = candidate_ = pos;
        count_ = 0;
    }
    // Once per block with the raw position; returns the accepted position.
    uint8_t Process(uint8_t raw) {
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

// LED2's preset number: n blinks, each kBlinkOnBlocks on then kBlinkOffBlocks off, then off.
constexpr int kBlinkOnBlocks = 150, kBlinkOffBlocks = 150;  // 150 ms on, 150 ms off at the 1 kHz block rate
class Blinker {
public:
    // n blinks from the next Tick; restarts any pattern in progress; n <= 0 is off.
    void Start(int n) {
        remaining_ = n > 0 ? n : 0;
        phase_ = 0;
    }
    // Once per block: true (on) for the first kBlinkOnBlocks of each of the n periods.
    bool Tick() {
        if (remaining_ == 0) return false;
        const bool on = phase_ < kBlinkOnBlocks;
        if (++phase_ == kBlinkOnBlocks + kBlinkOffBlocks) {
            phase_ = 0;
            --remaining_;
        }
        return on;
    }

private:
    int remaining_ = 0, phase_ = 0;
};

}  // namespace dkctl
