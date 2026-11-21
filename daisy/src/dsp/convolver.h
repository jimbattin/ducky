// The cab IR: two-level partitioned overlap-save FFT convolution, zero latency. Spec: docs/FILTERS.md "IR convolver".
// Host-portable (CMSIS-DSP's C FFT, no libDaisy, no allocation), covered by tests/convolver_test.cpp.
#pragma once

#include <cstddef>
#include <cstring>

#include "../memory.h"
#include "blocks.h"
#include "dsp/transform_functions.h"

namespace dkdsp {

constexpr size_t kIrHeadFft = 64;                            // head real FFT
constexpr size_t kIrHeadHistory = kIrHeadFft - kBlockSize;   // older input samples in each head frame: 40
constexpr size_t kIrTailFrame = 4 * kBlockSize;              // 96: tail partition and hop
constexpr size_t kIrTailPhases = kIrTailFrame / kBlockSize;  // 4 blocks per tail frame
constexpr size_t kIrHeadTaps = 2 * kIrTailFrame;             // 192: one tail frame of slack
constexpr size_t kIrHeadParts = kIrHeadTaps / kBlockSize;    // 8
constexpr size_t kIrTailFft = 256;                           // tail real FFT
constexpr size_t kIrTailHistory = kIrTailFft - kIrTailFrame;  // older input samples in each tail window: 160
constexpr size_t kIrTailParts = (kIrTaps - kIrHeadTaps + kIrTailFrame - 1) / kIrTailFrame;  // 20
// Tail partitions multiply-accumulated in phase k: [kIrTailSplit[k], kIrTailSplit[k + 1]). Phase 0 runs the tail's
// forward FFT and phase 3 its inverse, each about the cost of ten partitions (measured, docs/FILTERS.md).
inline constexpr size_t kIrTailSplit[kIrTailPhases + 1] = {0, 0, 10, 20, 20};
constexpr size_t kIrTailPoolFloats = 2 * kIrTailParts * kIrTailFft;  // partition spectra, then input spectra: 10,240
static_assert(kIrHeadFft - kBlockSize + 1 >= kBlockSize, "the head's overlap-save yields a block of valid outputs");
static_assert(kIrTailHistory + 1 >= kIrTailFrame, "the tail's overlap-save yields a frame of valid outputs");
static_assert(kIrTailFrame % kBlockSize == 0, "a tail frame is whole blocks");
static_assert(kIrTailSplit[kIrTailPhases] == kIrTailParts, "the phases cover every tail partition");
static_assert(kIrHeadFft % 8 == 0 && kIrTailFft % 8 == 0, "the multiply-accumulate runs tiles of 4 complex bins");

// The IR is split in two. The head, taps [0, kIrHeadTaps), runs every block as kIrHeadParts partitions of kBlockSize
// taps: one forward FFT of the newest kIrHeadFft input samples, the spectra of the last kIrHeadParts blocks times the
// partition spectra, summed, one inverse FFT; the last kBlockSize outputs are the block's. The tail, the remaining
// taps, runs once per frame of kIrTailPhases blocks as kIrTailParts partitions of kIrTailFrame taps, its work spread
// over the frame's blocks: at phase 0 of frame f the forward FFT of the kIrTailFft input samples that end with frame
// f - 1; over the frame the last kIrTailParts such spectra times the partition spectra (kIrTailSplit); at the last
// phase the inverse FFT, whose last kIrTailFrame outputs are added to the head's during frame f + 1. So the tail's
// output is two frames (kIrHeadTaps samples) behind its input, which is where its first tap sits.
// Spectra are CMSIS arm_rfft_fast_f32 packed: [DC, Nyquist, re1, im1, ...].
//
// The object holds the head and the working buffers (about 9 KB: the pedal's is in DTCM); the tail's spectra are the
// caller's pool of kIrTailPoolFloats floats. No default member initialisers, so an instance can live in a NOLOAD
// section: Init, Load and Reset write every member before Process runs.
class IrConvolver {
public:
    // tail_pool: kIrTailPoolFloats floats that outlive the convolver, 32-byte aligned where it is cached memory (a
    // tile of each spectrum is then one D-cache line: 2,000 cycles a block on the device). False if CMSIS lacks one
    // of the real FFTs (its tables are compiled out, daisy/cmsis_fft.mk). Once, before the first Load.
    bool Init(float* tail_pool) {
        h2_ = tail_pool;
        x2_ = tail_pool + kIrTailParts * kIrTailFft;
        return arm_rfft_fast_init_f32(&headFft_, kIrHeadFft) == ARM_MATH_SUCCESS &&
               arm_rfft_fast_init_f32(&tailFft_, kIrTailFft) == ARM_MATH_SUCCESS;
    }

    // taps: kIrTaps taps at 48 kHz (dkir::Ir::taps). Then Reset().
    void Load(const float* taps) {
        for (size_t p = 0; p < kIrHeadParts; ++p) {
            std::memcpy(scratch_, taps + p * kBlockSize, kBlockSize * sizeof(float));
            std::memset(scratch_ + kBlockSize, 0, (kIrHeadFft - kBlockSize) * sizeof(float));
            arm_rfft_fast_f32(&headFft_, scratch_, h1_[p], 0);
        }
        for (size_t p = 0; p < kIrTailParts; ++p) {
            for (size_t i = 0; i < kIrTailFrame; ++i) {
                const size_t k = kIrHeadTaps + p * kIrTailFrame + i;
                scratch_[i] = k < kIrTaps ? taps[k] : 0.0f;
            }
            std::memset(scratch_ + kIrTailFrame, 0, kIrTailHistory * sizeof(float));
            arm_rfft_fast_f32(&tailFft_, scratch_, h2_ + p * kIrTailFft, 0);
        }
        Reset();
    }

    // Clears the input history: the output restarts from silence, at the start of a tail frame.
    void Reset() {
        std::memset(x1_, 0, sizeof x1_);
        std::memset(frame_, 0, sizeof frame_);
        std::memset(x2_, 0, kIrTailParts * kIrTailFft * sizeof(float));
        std::memset(win_, 0, sizeof win_);
        std::memset(tailIn_, 0, sizeof tailIn_);
        std::memset(tailOut_, 0, sizeof tailOut_);
        slot1_ = slot2_ = phase_ = 0;
    }

    // Exactly kBlockSize samples; in and out may alias. One out-of-line copy (noinline, noclone): GCC rejects a clone
    // beside the inline function in its ITCM section (memory.h).
    [[gnu::noinline, gnu::noclone]] DK_ITCM void Process(const float* in, float* out) {
        // A tail frame starts: the spectrum of the kIrTailFft input samples that end with the frame just completed.
        if (phase_ == 0) {
            CopyFloats(win_, win_ + kIrTailFrame, kIrTailHistory);
            CopyFloats(win_ + kIrTailHistory, tailIn_, kIrTailFrame);
            slot2_ = slot2_ + 1 == kIrTailParts ? 0 : slot2_ + 1;
            CopyFloats(scratch_, win_, kIrTailFft);  // CMSIS overwrites its input
            arm_rfft_fast_f32(&tailFft_, scratch_, x2_ + slot2_ * kIrTailFft, 0);
        }

        // The block's input: into the tail frame being collected, and the newest kIrHeadFft samples' spectrum.
        CopyFloats(tailIn_ + phase_ * kBlockSize, in, kBlockSize);
        CopyFloats(frame_, frame_ + kBlockSize, kIrHeadHistory);
        CopyFloats(frame_ + kIrHeadHistory, in, kBlockSize);
        CopyFloats(scratch_, frame_, kIrHeadFft);
        arm_rfft_fast_f32(&headFft_, scratch_, x1_[slot1_], 0);

        // The head: y1 = sum over p of X1(block - p) * H1_p.
        {
            const float* xs[kIrHeadParts];
            const float* hs[kIrHeadParts];
            size_t s = slot1_;
            for (size_t p = 0; p < kIrHeadParts; ++p) {
                xs[p] = x1_[s];
                hs[p] = h1_[p];
                s = s == 0 ? kIrHeadParts - 1 : s - 1;
            }
            Mac(xs, hs, kIrHeadParts, kIrHeadFft, y1_, false, false);
            arm_rfft_fast_f32(&headFft_, y1_, t1_, 1);
        }

        // The tail: this phase's share of y2 = sum over q of X2(frame - 1 - q) * H2_q.
        {
            const size_t q0 = kIrTailSplit[phase_], n = kIrTailSplit[phase_ + 1] - q0;
            const float* xs[kIrTailParts + 1];
            const float* hs[kIrTailParts + 1];
            size_t s = slot2_ >= q0 ? slot2_ - q0 : slot2_ + kIrTailParts - q0;
            for (size_t j = 0; j < n; ++j) {
                xs[j] = x2_ + s * kIrTailFft;
                hs[j] = h2_ + (q0 + j) * kIrTailFft;
                s = s == 0 ? kIrTailParts - 1 : s - 1;
            }
            if (n != 0) {
                xs[n] = xs[0] + 8;  // what Mac prefetches during a tile's last partition: the next tile's first
                hs[n] = hs[0] + 8;
                Mac(xs, hs, n, kIrTailFft, y2_, q0 != 0, true);
            }
        }

        const float* tail = tailOut_ + phase_ * kBlockSize;
        for (size_t i = 0; i < kBlockSize; ++i) out[i] = t1_[kIrHeadHistory + i] + tail[i];

        // The frame's tail sum is complete: the next frame's tail output.
        if (phase_ == kIrTailPhases - 1) {
            arm_rfft_fast_f32(&tailFft_, y2_, scratch_, 1);
            CopyFloats(tailOut_, scratch_ + kIrTailHistory, kIrTailFrame);
        }
        slot1_ = slot1_ + 1 == kIrHeadParts ? 0 : slot1_ + 1;
        phase_ = phase_ + 1 == kIrTailPhases ? 0 : phase_ + 1;
    }

private:
    // y (+)= sum over q < parts of X_q * H_q for packed spectra of `fft` floats: y starts from zero, or from its
    // contents when `accumulate` is set. 4 complex bins per tile, one island per partition: 16 loads, then 16
    // multiply-adds ordered so each accumulator's two updates are 8 apart (docs/M7_PERFORMANCE.md). With `prefetch`
    // each island first asks for the next one's two cache lines (xs[parts] and hs[parts] name the ones after the
    // last partition's), which then load while it computes: for spectra in cached memory, where every line is a miss
    // (the tail's 40 KB against the 16 KB D-cache).
    DK_ITCM static void Mac(const float* const* xs, const float* const* hs, size_t parts, size_t fft, float* y,
                            bool accumulate, bool prefetch) {
        // Pair 0 holds the real DC and Nyquist bins, not one complex bin: summed apart, straight after the first
        // tile, while its cache lines are still loaded.
        const float dc0 = accumulate ? y[0] : 0.0f, ny0 = accumulate ? y[1] : 0.0f;
        for (size_t t = 0; t < fft; t += 8) {
            float* yt = y + t;
            float r0, i0, r1, i1, r2, i2, r3, i3;
            if (accumulate) {
                r0 = FpHere(yt[0]), i0 = FpHere(yt[1]), r1 = FpHere(yt[2]), i1 = FpHere(yt[3]);
                r2 = FpHere(yt[4]), i2 = FpHere(yt[5]), r3 = FpHere(yt[6]), i3 = FpHere(yt[7]);
            } else {
                r0 = FpHere(0.0f), i0 = FpHere(0.0f), r1 = FpHere(0.0f), i1 = FpHere(0.0f);
                r2 = FpHere(0.0f), i2 = FpHere(0.0f), r3 = FpHere(0.0f), i3 = FpHere(0.0f);
            }
#pragma GCC unroll 1
            for (size_t q = 0; q < parts; ++q) {
                const float* xp = xs[q] + t;
                const float* hp = hs[q] + t;
                if (prefetch) {
                    __builtin_prefetch(xs[q + 1] + t);
                    __builtin_prefetch(hs[q + 1] + t);
                }
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
            }
            yt[0] = r0;
            yt[1] = i0;
            yt[2] = r1;
            yt[3] = i1;
            yt[4] = r2;
            yt[5] = i2;
            yt[6] = r3;
            yt[7] = i3;
            if (t == 0) {
                float dc = dc0, nyquist = ny0;
                for (size_t q = 0; q < parts; ++q) {
                    dc += xs[q][0] * hs[q][0];
                    nyquist += xs[q][1] * hs[q][1];
                }
                y[0] = dc;
                y[1] = nyquist;
            }
        }
    }

    arm_rfft_fast_instance_f32 headFft_, tailFft_;
    alignas(32) float h1_[kIrHeadParts][kIrHeadFft];  // head partition spectra
    alignas(32) float x1_[kIrHeadParts][kIrHeadFft];  // head input spectra, a ring
    alignas(32) float frame_[kIrHeadFft];             // the newest kIrHeadFft input samples
    alignas(32) float y1_[kIrHeadFft];
    alignas(32) float t1_[kIrHeadFft];
    alignas(32) float scratch_[kIrTailFft];    // FFT input (both sizes) and the tail's inverse
    alignas(32) float win_[kIrTailFft];        // the kIrTailFft input samples up to the last completed tail frame
    alignas(32) float y2_[kIrTailFft];         // the tail sum being built over the frame
    alignas(32) float tailIn_[kIrTailFrame];   // the input of the tail frame being collected
    alignas(32) float tailOut_[kIrTailFrame];  // the tail's output for the current frame
    float* h2_;  // kIrTailParts tail partition spectra of kIrTailFft floats (the caller's pool)
    float* x2_;  // kIrTailParts tail input spectra, a ring
    size_t slot1_, slot2_;  // the newest head and tail input spectra
    size_t phase_;          // the block within the tail frame
};

}  // namespace dkdsp
