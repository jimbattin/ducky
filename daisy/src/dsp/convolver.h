// The cab IR: uniformly partitioned overlap-save FFT convolution, zero latency. Spec: docs/FILTERS.md "IR convolver".
// Host-portable (CMSIS-DSP's C FFT, no libDaisy, no allocation), covered by tests/convolver_test.cpp.
#pragma once

#include <cstddef>
#include <cstring>

#include "../memory.h"
#include "a2lite.h"
#include "blocks.h"
#include "dsp/transform_functions.h"

namespace dkdsp {

constexpr size_t kIrFft = 128;                                           // real FFT size
constexpr size_t kIrPartition = kBlockSize;                              // taps per partition
constexpr size_t kIrParts = (kIrTaps + kIrPartition - 1) / kIrPartition;  // 43
constexpr size_t kIrHistory = kIrFft - kIrPartition;                     // older input samples in each frame: 80
static_assert(kIrFft - kIrPartition + 1 >= kBlockSize, "overlap-save yields at least one block of valid outputs");
static_assert(kIrFft % 8 == 0, "the multiply-accumulate runs tiles of 4 complex bins");

// Every block: one forward FFT of the newest kIrFft input samples, the spectra of the last kIrParts blocks times the
// kIrParts partition spectra, summed, one inverse FFT; the last kBlockSize outputs are the block's. Spectra are CMSIS
// arm_rfft_fast_f32 packed: [DC, Nyquist, re1, im1, ..., re63, im63].
//
// No default member initialisers, so an instance can live in a NOLOAD section (the bench's DK_D2 one): Init, Load and
// Reset write every member before Process runs.
class IrConvolver {
public:
    // False if CMSIS has no kIrFft-point real FFT (its tables are compiled out, daisy/cmsis_fft.mk).
    bool Init() { return arm_rfft_fast_init_f32(&fft_, kIrFft) == ARM_MATH_SUCCESS; }

    // taps: kIrTaps taps at 48 kHz (dkir::Ir::taps). Then Reset().
    void Load(const float* taps) {
        for (size_t p = 0; p < kIrParts; ++p) {
            for (size_t i = 0; i < kIrPartition; ++i) {
                const size_t k = p * kIrPartition + i;
                scratch_[i] = k < kIrTaps ? taps[k] : 0.0f;
            }
            for (size_t i = kIrPartition; i < kIrFft; ++i) scratch_[i] = 0.0f;
            arm_rfft_fast_f32(&fft_, scratch_, h_[p], 0);
        }
        Reset();
    }

    // Clears the input history: the output restarts from silence.
    void Reset() {
        std::memset(x_, 0, sizeof x_);
        std::memset(frame_, 0, sizeof frame_);
        slot_ = 0;
    }

    // Exactly kBlockSize samples; in and out may alias.
    DK_ITCM void Process(const float* in, float* out) {
        // The newest kIrFft input samples.
        std::memmove(frame_, frame_ + kIrPartition, kIrHistory * sizeof(float));
        std::memcpy(frame_ + kIrHistory, in, kIrPartition * sizeof(float));
        std::memcpy(scratch_, frame_, sizeof frame_);  // CMSIS overwrites its input
        arm_rfft_fast_f32(&fft_, scratch_, x_[slot_], 0);
        std::memcpy(x_[slot_ + kIrParts], x_[slot_], sizeof x_[0]);

        // y = sum over p of X(block - p) * H_p, 4 complex bins per tile. The input spectra run down from the mirror
        // of the newest one, so the pointer never wraps. One island per partition: 16 loads, then 16 multiply-adds
        // ordered so each accumulator's two updates are 8 apart (docs/M7_PERFORMANCE.md).
        for (size_t t = 0; t < kIrFft; t += 8) {
            const float* xp = x_[slot_ + kIrParts] + t;
            const float* hp = h_[0] + t;
            float r0 = FpHere(0.0f), i0 = FpHere(0.0f), r1 = FpHere(0.0f), i1 = FpHere(0.0f);
            float r2 = FpHere(0.0f), i2 = FpHere(0.0f), r3 = FpHere(0.0f), i3 = FpHere(0.0f);
#pragma GCC unroll 1
            for (size_t p = 0; p < kIrParts; ++p) {
                const float xr0 = FpHere(xp[0]), xi0 = FpHere(xp[1]), xr1 = FpHere(xp[2]), xi1 = FpHere(xp[3]);
                const float xr2 = FpHere(xp[4]), xi2 = FpHere(xp[5]), xr3 = FpHere(xp[6]), xi3 = FpHere(xp[7]);
                const float hr0 = FpHere(hp[0]), hi0 = FpHere(hp[1]), hr1 = FpHere(hp[2]), hi1 = FpHere(hp[3]);
                const float hr2 = FpHere(hp[4]), hi2 = FpHere(hp[5]), hr3 = FpHere(hp[6]), hi3 = FpHere(hp[7]);
                r0 += xr0 * hr0;
                i0 += xr0 * hi0;
                r1 += xr1 * hr1;
                i1 += xr1 * hi1;
                r2 += xr2 * hr2;
                i2 += xr2 * hi2;
                r3 += xr3 * hr3;
                i3 += xr3 * hi3;
                r0 -= xi0 * hi0;
                i0 += xi0 * hr0;
                r1 -= xi1 * hi1;
                i1 += xi1 * hr1;
                r2 -= xi2 * hi2;
                i2 += xi2 * hr2;
                r3 -= xi3 * hi3;
                i3 += xi3 * hr3;
                xp -= kIrFft;
                hp += kIrFft;
            }
            float* y = y_ + t;
            y[0] = r0;
            y[1] = i0;
            y[2] = r1;
            y[3] = i1;
            y[4] = r2;
            y[5] = i2;
            y[6] = r3;
            y[7] = i3;
        }

        // Pair 0 holds the real DC and Nyquist bins, not one complex bin: redo it.
        {
            const float* xp = x_[slot_ + kIrParts];
            const float* hp = h_[0];
            float dc = 0.0f, nyquist = 0.0f;
            for (size_t p = 0; p < kIrParts; ++p) {
                dc += xp[0] * hp[0];
                nyquist += xp[1] * hp[1];
                xp -= kIrFft;
                hp += kIrFft;
            }
            y_[0] = dc;
            y_[1] = nyquist;
        }

        arm_rfft_fast_f32(&fft_, y_, t_, 1);
        std::memcpy(out, t_ + kIrHistory, kIrPartition * sizeof(float));
        slot_ = slot_ + 1 == kIrParts ? 0 : slot_ + 1;
    }

private:
    arm_rfft_fast_instance_f32 fft_;
    alignas(32) float h_[kIrParts][kIrFft];      // partition spectra
    alignas(32) float x_[2 * kIrParts][kIrFft];  // input spectra, a ring of kIrParts mirrored at +kIrParts
    alignas(32) float frame_[kIrFft];            // the newest kIrFft input samples
    alignas(32) float scratch_[kIrFft];
    alignas(32) float y_[kIrFft];
    alignas(32) float t_[kIrFft];
    size_t slot_;  // the newest input spectrum
};

}  // namespace dkdsp
