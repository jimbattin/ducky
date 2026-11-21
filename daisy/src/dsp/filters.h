// The EQ's sections (docs/FILTERS.md, docs/EQ.md): the cut filters (low-cut, high-cut) and the bell. The stage that
// chains them, low-cut, three bells, high-cut, and drives them from the parameters (params.h) is eq.h.
// Host-portable (no libDaisy, no allocation), covered by tests/filters_test.cpp and tests/eq_test.cpp.
//
// Every section is a trapezoidal (TPT, Zavalishin/Simper) one-pole or state-variable filter: bilinear-exact responses,
// and well behaved when the coefficients change every control tick. The control tick is kBlockSize samples, counted in
// samples: a section's Control() runs at each tick's start and its Run() over any split of the tick, with the same
// output whatever the split.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "../memory.h"
#include "blocks.h"
#include "gain.h"

namespace dkdsp {

constexpr float kLowCutMinHz = 20.0f, kLowCutMaxHz = 500.0f;
constexpr float kHighCutMinHz = 1500.0f, kHighCutMaxHz = 20000.0f;
constexpr float kResMinQ = 0.5f, kResMaxQ = 2.0f;
constexpr float kButterworthQ = 0.70710678f;
// The bells: gain +-kBellGainDb, Q kBellQMin..kBellQMax, bell b's frequency kBellMinHz[b]..kBellMaxHz[b].
constexpr float kBellGainDb = 12.0f, kBellQMin = 0.4f, kBellQMax = 8.0f;
inline constexpr float kBellMinHz[3] = {40.0f, 200.0f, 800.0f}, kBellMaxHz[3] = {800.0f, 5000.0f, 12000.0f};
// A cut filter is bypassed (dry, bit exact) with its frequency position within kBypassEnter of the end that turns it
// off (low-cut at 0, high-cut at 1), and comes back beyond kBypassLeave.
constexpr float kBypassEnter = 0.02f, kBypassLeave = 0.04f;
constexpr size_t kCutFadeSamples = 240;  // 5 ms: slope changes and bypass crossfade over this
static_assert(kCutFadeSamples % kBlockSize == 0, "a fade spans whole control ticks");

// low_cut_freq, 0..1 -> 20..500 Hz, log.
inline float LowCutHz(float pos) { return kLowCutMinHz * std::pow(kLowCutMaxHz / kLowCutMinHz, pos); }
// high_cut_freq, 0..1 -> 1.5..20 kHz, log.
inline float HighCutHz(float pos) { return kHighCutMinHz * std::pow(kHighCutMaxHz / kHighCutMinHz, pos); }
// low_cut_res and high_cut_res, 0..1 -> Q 0.5..2, log; 1 at 0.5.
inline float ResonanceQ(float pos) { return kResMinQ * std::pow(kResMaxQ / kResMinQ, pos); }

enum class Slope : uint8_t { k6, k12, k24 };

// A slope parameter's index (laid out as a toggle position, controls.h: 0 down, 1 middle, 2 up) -> slope: 2 6 dB,
// 1 12 dB, 0 24 dB per octave.
inline Slope SlopeFromToggle(uint8_t pos) { return pos >= 2 ? Slope::k6 : pos == 1 ? Slope::k12 : Slope::k24; }

// A cut filter's off end, with hysteresis so a knob resting at the threshold does not chatter.
class EdgeBypass {
public:
    // at_zero: the filter turns off at position 0 (low-cut); otherwise at 1 (high-cut).
    void Init(bool at_zero, float pos) {
        at_zero_ = at_zero;
        bypassed_ = at_zero ? pos <= kBypassEnter : pos >= 1.0f - kBypassEnter;
    }
    bool Process(float pos) {
        if (at_zero_)
            bypassed_ = bypassed_ ? pos <= kBypassLeave : pos <= kBypassEnter;
        else
            bypassed_ = bypassed_ ? pos >= 1.0f - kBypassLeave : pos >= 1.0f - kBypassEnter;
        return bypassed_;
    }

private:
    bool at_zero_ = true, bypassed_ = false;
};

// Coefficients of a TPT state-variable filter at g = tan(pi f / fs) with damping k = 1 / Q.
struct SvfCoefs {
    float k, a1, a2, a3;
};

inline SvfCoefs MakeSvf(float g, float q) {
    SvfCoefs c;
    c.k = 1.0f / q;
    c.a1 = 1.0f / (1.0f + g * (g + c.k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

inline float Prewarp(float hz) { return std::tan(float(M_PI) * hz / kSampleRate); }

// A trapezoidal one-pole's gain G = g / (1 + g) at hz.
inline float OnePoleGain(float hz) {
    const float g = Prewarp(hz);
    return g / (1.0f + g);
}

// A low-cut (high-pass) or high-cut (low-pass) with a 6, 12 or 24 dB/octave slope, resonance on the 12 and 24 dB
// slopes, and an off end. All three slopes run every tick whatever the slope and the bypass, so the load is constant
// and a slope change crossfades (kCutFadeSamples) between two settled outputs:
//   6 dB: a one-pole; 12 dB: an SVF at Q; 24 dB: a Butterworth SVF into an SVF at Q.
// At mix 0 (bypassed) the output is the input bit for bit; once a slope fade completes its weights are exactly one-hot.
class CutFilter {
public:
    // high_pass: the low-cut. freq, res: positions 0..1; slope: the slope parameter's index (SlopeFromToggle). No
    // fade: starts settled, state zero, at a tick's start.
    void Init(bool high_pass, float freq, float res, uint8_t slope) {
        high_pass_ = high_pass;
        freq_.Init(kBlockRate, kKnobSmoothS, freq);
        res_.Init(kBlockRate, kKnobSmoothS, res);
        bypass_.Init(high_pass, freq);
        target_ = SlopeFromToggle(slope);
        for (int j = 0; j < 3; ++j) from_[j] = j == int(target_) ? 1.0f : 0.0f;
        pos_ = kCutFadeSamples;
        mix_ = bypass_.Process(freq) ? 0.0f : 1.0f;
        s1_ = q1_ = q2_ = a1_ = a2_ = b1_ = b2_ = 0.0f;
        fkDesigned_ = rkDesigned_ = std::numeric_limits<float>::quiet_NaN();
    }

    // Once per control tick, at its start: the smoothers, the bypass, the coefficients (designed when a smoothed
    // control has moved: at rest they are the ones in place), the slope fade and the mix across the tick.
    DK_ITCM void Control(float freq, float res, uint8_t slope) {
        const float fk = freq_.Process(freq), rk = res_.Process(res);
        const bool bypassed = bypass_.Process(fk);
        if (fk != fkDesigned_ || rk != rkDesigned_) {
            const float g = Prewarp(high_pass_ ? LowCutHz(fk) : HighCutHz(fk));
            const float q = ResonanceQ(rk);
            r_.G = g / (1.0f + g);
            r_.s12 = MakeSvf(g, q);
            r_.sa = MakeSvf(g, kButterworthQ);
            r_.sb = r_.s12;
            fkDesigned_ = fk;
            rkDesigned_ = rk;
        }

        const Slope want = SlopeFromToggle(slope);
        if (want != target_) {
            for (int j = 0; j < 3; ++j) from_[j] = Weight(j, pos_);
            target_ = want;
            pos_ = 0;
        }
        const size_t end = pos_ + kBlockSize < kCutFadeSamples ? pos_ + kBlockSize : kCutFadeSamples;
        for (int j = 0; j < 3; ++j) {
            r_.w0[j] = Weight(j, pos_);
            r_.dw[j] = (Weight(j, end) - r_.w0[j]) * kInvBlockSize;
        }
        pos_ = end;

        constexpr float kStep = float(kBlockSize) / float(kCutFadeSamples);
        float m1 = bypassed ? mix_ - kStep : mix_ + kStep;
        m1 = m1 < 0.0f ? 0.0f : m1 > 1.0f ? 1.0f : m1;
        r_.m0 = mix_;
        r_.dm = (m1 - mix_) * kInvBlockSize;
        mix_ = m1;
    }

    // m samples in place, starting `at` samples into the current tick (at + m <= kBlockSize).
    DK_ITCM void Run(float* x, size_t m, size_t at) {
        if (high_pass_)
            RunSamples<true>(x, m, at);
        else
            RunSamples<false>(x, m, at);
    }

private:
    struct Tick {
        float G;               // one-pole
        SvfCoefs s12, sa, sb;  // 12 dB; 24 dB Butterworth stage, then its Q stage
        float w0[3], dw[3];    // slope weights (6, 12, 24): w0 + dw t at sample t - 1 of the tick
        float m0, dm;          // wet mix, the same law
    };

    float Weight(int j, size_t pos) const {
        const float one = j == int(target_) ? 1.0f : 0.0f;
        if (pos >= kCutFadeSamples) return one;
        return from_[j] + (one - from_[j]) * (float(pos) / float(kCutFadeSamples));
    }

    // The per-sample loop. Statements are interleaved stage by stage so the one-pole, the 12 dB SVF and the 24 dB
    // Butterworth stage fill each other's latency (docs/M7_PERFORMANCE.md "Recursive filters"). Coefficients and
    // states are copied to locals: GCC must assume the float* x may alias the members.
    template <bool kHighPass>
    [[gnu::always_inline]] void RunSamples(float* x, size_t n, size_t at) {
        const Tick& r = r_;
        const float G = r.G;
        const float k12 = r.s12.k, c1 = r.s12.a1, c2 = r.s12.a2, c3 = r.s12.a3;
        const float ka = r.sa.k, d1 = r.sa.a1, d2 = r.sa.a2, d3 = r.sa.a3;
        const float kb = r.sb.k, e1 = r.sb.a1, e2 = r.sb.a2, e3 = r.sb.a3;
        const float w6 = r.w0[0], w12 = r.w0[1], w24 = r.w0[2], dw6 = r.dw[0], dw12 = r.dw[1], dw24 = r.dw[2];
        const float m0 = r.m0, dm = r.dm;
        float s1 = s1_, q1 = q1_, q2 = q2_, a1 = a1_, a2 = a2_, b1 = b1_, b2 = b2_;
        for (size_t i = 0; i < n; ++i) {
            const float xin = x[i];
            const float t = float(at + i + 1);
            const float v = (xin - s1) * G;
            const float v3 = xin - q2;
            const float u3 = xin - a2;
            const float lp = v + s1;
            const float v1 = c1 * q1 + c2 * v3;
            const float u1 = d1 * a1 + d2 * u3;
            const float v2 = q2 + c2 * q1 + c3 * v3;
            const float u2 = a2 + d2 * a1 + d3 * u3;
            s1 = lp + v;
            q1 = 2.0f * v1 - q1;
            a1 = 2.0f * u1 - a1;
            q2 = 2.0f * v2 - q2;
            a2 = 2.0f * u2 - a2;
            const float y6 = kHighPass ? xin - lp : lp;
            const float y12 = kHighPass ? xin - k12 * v1 - v2 : v2;
            const float ya = kHighPass ? xin - ka * u1 - u2 : u2;

            const float z3 = ya - b2;
            const float z1 = e1 * b1 + e2 * z3;
            const float z2 = b2 + e2 * b1 + e3 * z3;
            b1 = 2.0f * z1 - b1;
            b2 = 2.0f * z2 - b2;
            const float y24 = kHighPass ? ya - kb * z1 - z2 : z2;

            const float wet = (w6 + dw6 * t) * y6 + (w12 + dw12 * t) * y12 + (w24 + dw24 * t) * y24;
            x[i] = xin + (m0 + dm * t) * (wet - xin);
        }
        s1_ = s1;
        q1_ = q1;
        q2_ = q2;
        a1_ = a1;
        a2_ = a2;
        b1_ = b1;
        b2_ = b2;
    }

    bool high_pass_ = true;
    Smoother freq_, res_;
    EdgeBypass bypass_;
    Slope target_ = Slope::k12;     // the slope faded toward
    float from_[3] = {};            // the weights the fade started from
    size_t pos_ = kCutFadeSamples;  // samples into the fade
    float mix_ = 1.0f;              // wet amount, 0 bypassed
    Tick r_ = {};                   // the current tick's coefficients and ramps
    // The smoothed frequency and resonance positions r_'s coefficients were designed at; NaN: none yet.
    float fkDesigned_ = std::numeric_limits<float>::quiet_NaN(), rkDesigned_ = fkDesigned_;
    float s1_ = 0.0f;              // one-pole
    float q1_ = 0.0f, q2_ = 0.0f;  // 12 dB SVF
    float a1_ = 0.0f, a2_ = 0.0f;  // 24 dB Butterworth stage
    float b1_ = 0.0f, b2_ = 0.0f;  // 24 dB Q stage
};

// A bell's SVF coefficients and band mix (output x + m1 band).
struct BellCoefs {
    double a1, a2, a3, m1;
};

// What a bell's frequency and Q fix: g = tan(pi f0 / fs), and q.
struct BellShape {
    double g, q;
};

inline BellShape ShapeOf(double f0, double q, double fs) { return {std::tan(M_PI * f0 / fs), q}; }

// The RBJ cookbook bell of gain db at shape s (constant-Q form: k = 1 / (Q A)), as A. Simper's linear trapezoidal SVF
// mixed to it: exactly its bilinear transform prewarped at the shape's frequency. Designed in double (the M7's FPU
// is fpv5-d16). At 0 dB m1 is 0.
inline BellCoefs DesignBell(const BellShape& s, double db) {
    const double A = std::pow(10.0, db / 40.0);
    const double g = s.g, k = 1.0 / (s.q * A);
    const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1;
    return {a1, a2, g * a2, k * (A * A - 1.0)};
}

// One bell in float, its coefficients designed in double (DesignBell). A coefficient change ramps linearly across one
// control tick: a step in a coefficient is a step in the output. The ramp counts samples from the tick's start
// (c0 + dc t), so the output does not depend on how the tick is split.
class Bell {
public:
    void Clear() { ic1_ = ic2_ = 0.0f; }
    // At once.
    void Set(const BellCoefs& c) {
        Assign(to_, c);
        for (int i = 0; i < 4; ++i) {
            c0_[i] = to_[i];
            dc_[i] = 0.0f;
        }
        ramping_ = false;
    }
    // From the current coefficients to c across the coming tick. Call at the tick's start.
    void RampTo(const BellCoefs& c) {
        for (int i = 0; i < 4; ++i) c0_[i] = to_[i];
        Assign(to_, c);
        for (int i = 0; i < 4; ++i) dc_[i] = (to_[i] - c0_[i]) * kInvBlockSize;
        ramping_ = true;
    }
    // At the tick's start with no new target: a ramp in progress ends at its target.
    void Settle() {
        if (!ramping_) return;
        for (int i = 0; i < 4; ++i) {
            c0_[i] = to_[i];
            dc_[i] = 0.0f;
        }
        ramping_ = false;
    }

    // m samples in place, starting `at` samples into the current tick (at + m <= kBlockSize).
    DK_ITCM void Run(float* x, size_t m, size_t at) {
        float ic1 = ic1_, ic2 = ic2_;
        if (!ramping_) {
            const float a1 = c0_[0], a2 = c0_[1], a3 = c0_[2], m1 = c0_[3];
            for (size_t i = 0; i < m; ++i) {
                const float xin = x[i];
                const float v3 = xin - ic2;
                const float v1 = a1 * ic1 + a2 * v3;
                const float v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0f * v1 - ic1;
                ic2 = 2.0f * v2 - ic2;
                x[i] = xin + m1 * v1;
            }
        } else {
            const float a10 = c0_[0], a20 = c0_[1], a30 = c0_[2], m10 = c0_[3];
            const float da1 = dc_[0], da2 = dc_[1], da3 = dc_[2], dm1 = dc_[3];
            for (size_t i = 0; i < m; ++i) {
                const float xin = x[i];
                const float t = float(at + i + 1);
                const float a1 = a10 + da1 * t, a2 = a20 + da2 * t, a3 = a30 + da3 * t, m1 = m10 + dm1 * t;
                const float v3 = xin - ic2;
                const float v1 = a1 * ic1 + a2 * v3;
                const float v2 = ic2 + a2 * ic1 + a3 * v3;
                ic1 = 2.0f * v1 - ic1;
                ic2 = 2.0f * v2 - ic2;
                x[i] = xin + m1 * v1;
            }
        }
        ic1_ = ic1;
        ic2_ = ic2;
    }

private:
    static void Assign(float* to, const BellCoefs& c) {
        to[0] = float(c.a1);
        to[1] = float(c.a2);
        to[2] = float(c.a3);
        to[3] = float(c.m1);
    }

    float c0_[4] = {}, dc_[4] = {}, to_[4] = {};  // {a1, a2, a3, m1}: the tick's start, its per-sample step, target
    float ic1_ = 0.0f, ic2_ = 0.0f;
    bool ramping_ = false;
};

}  // namespace dkdsp
