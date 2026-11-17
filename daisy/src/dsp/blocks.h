// The audio rate and block size every DSP block and the firmware share, and the helpers they all use. Host-portable
// (no libDaisy).
#pragma once

#include <cmath>
#include <cstddef>

namespace dkdsp {

constexpr float kSampleRate = 48000.0f;  // the codec runs at SAI_48KHZ; NAM A2 models are trained at 48 kHz
constexpr size_t kBlockSize = 24;        // 0.5 ms
constexpr size_t kIrTaps = 2048;        // the longest IR, in taps at 48 kHz

constexpr float kBlockRate = kSampleRate / float(kBlockSize);  // blocks per second: 2000
constexpr size_t kBlocksPerMs = size_t(kSampleRate) / kBlockSize / 1000;
static_assert(kBlocksPerMs * kBlockSize * 1000 == size_t(kSampleRate), "a whole number of blocks per millisecond");

// The largest |x[i]| of x[0, n); 0 for n = 0.
[[gnu::always_inline]] inline float Peak(const float* x, size_t n) {
    float peak = 0.0f;
    for (size_t i = 0; i < n; ++i) peak = std::fmax(peak, std::fabs(x[i]));
    return peak;
}

// v in an FP register of its own, materialised at this point of the instruction stream. GCC otherwise emits a
// single-use load, or the copy of a shared value, immediately before the first instruction that uses it, which puts
// it in the middle of a multiply-accumulate run (dsp/a2lite.h). The value is unchanged; a no-op on the host.
[[gnu::always_inline]] inline float FpHere(float v) {
#if defined(__arm__)
    __asm__ volatile("" : "+w"(v));
#endif
    return v;
}

}  // namespace dkdsp
