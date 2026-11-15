// Toggle hysteresis. Host-portable (no libDaisy, no allocation), covered by tests/controls_test.cpp.
// Toggle positions (board.h): 0 down, 1 middle, 2 up.
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

}  // namespace dkctl
