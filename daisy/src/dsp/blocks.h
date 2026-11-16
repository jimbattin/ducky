// The audio rate and block size every DSP block and the firmware share. Host-portable (no libDaisy).
#pragma once

#include <cstddef>

namespace dkdsp {

constexpr float kSampleRate = 48000.0f;  // the codec runs at SAI_48KHZ; NAM A2 models are trained at 48 kHz
constexpr size_t kBlockSize = 24;        // 0.5 ms
constexpr size_t kIrTaps = 2048;        // the longest IR, in taps at 48 kHz

}  // namespace dkdsp
