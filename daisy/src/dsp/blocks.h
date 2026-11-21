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
constexpr float kInvBlockSize = 1.0f / float(kBlockSize);

constexpr int kCrossfadeSamples = 960;  // comp_mode, comp_type and eq_enable crossfade (20 ms)
// The tail of an on/off crossfade over kCrossfadeSamples (the compressors, the EQ): per sample, fade steps one toward
// want (kCrossfadeSamples or 0) and x[i] goes from dry[i] toward the wet x[i] by fade / kCrossfadeSamples. At a full
// fade the wet sample is kept as is (dry + (x - dry) can round); at 0 the blend is dry exactly.
[[gnu::always_inline]] inline void FinishCrossfade(float* x, const float* dry, size_t m, int& fade, int want) {
    for (size_t i = 0; i < m; ++i) {
        fade += fade < want ? 1 : fade > want ? -1 : 0;
        if (fade != kCrossfadeSamples) x[i] = dry[i] + float(fade) / float(kCrossfadeSamples) * (x[i] - dry[i]);
    }
}

// The largest |x[i]| of x[0, n); 0 for n = 0. Four running maxima, so no maximum waits for the one before it
// (docs/M7_PERFORMANCE.md); a maximum is exact, so the grouping changes no bit.
[[gnu::always_inline]] inline float Peak(const float* x, size_t n) {
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
    const size_t whole = n & ~size_t(3);
    for (size_t i = 0; i < whole; i += 4) {
        a = std::fmax(a, std::fabs(x[i]));
        b = std::fmax(b, std::fabs(x[i + 1]));
        c = std::fmax(c, std::fabs(x[i + 2]));
        d = std::fmax(d, std::fabs(x[i + 3]));
    }
    for (size_t i = whole; i < n; ++i) a = std::fmax(a, std::fabs(x[i]));
    return std::fmax(std::fmax(a, b), std::fmax(c, d));
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

// dst[0, n) = src[0, n); the ranges may overlap when dst is below src. For the audio path, in place of memcpy,
// memmove and std::copy: newlib-nano's memcpy and memmove are byte loops in SRAM, and GCC calls them for any copy
// whose size it does not expand inline, a plain copy loop included. FpHere keeps this one float loads and stores, 8
// loads then 8 stores (docs/M7_PERFORMANCE.md).
[[gnu::always_inline]] inline void CopyFloats(float* dst, const float* src, size_t n) {
    const size_t whole = n & ~size_t(7);
    for (size_t i = 0; i < whole; i += 8) {
        const float a = FpHere(src[i]), b = FpHere(src[i + 1]), c = FpHere(src[i + 2]), d = FpHere(src[i + 3]);
        const float e = FpHere(src[i + 4]), f = FpHere(src[i + 5]), g = FpHere(src[i + 6]), h = FpHere(src[i + 7]);
        dst[i] = a;
        dst[i + 1] = b;
        dst[i + 2] = c;
        dst[i + 3] = d;
        dst[i + 4] = e;
        dst[i + 5] = f;
        dst[i + 6] = g;
        dst[i + 7] = h;
    }
    for (size_t i = whole; i < n; ++i) dst[i] = FpHere(src[i]);
}

}  // namespace dkdsp
