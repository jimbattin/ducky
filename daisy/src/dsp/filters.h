// The EQ after the IR: low-cut (Knobs 5, 6, SW1), the SW3 mid bell, high-cut (Knobs 4, 3, SW2). Spec: docs/FILTERS.md.
// Host-portable (no libDaisy, no allocation), covered by tests/filters_test.cpp.
//
// Every section is a trapezoidal (TPT, Zavalishin/Simper) one-pole or state-variable filter: bilinear-exact responses,
// and well behaved when the coefficients change every block.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "../memory.h"
#include "blocks.h"
#include "gain.h"

namespace dkdsp {

constexpr float kLowCutMinHz = 20.0f, kLowCutMaxHz = 500.0f;
constexpr float kHighCutMinHz = 1500.0f, kHighCutMaxHz = 20000.0f;
constexpr float kResMinQ = 0.5f, kResMaxQ = 2.0f;
constexpr float kButterworthQ = 0.70710678f;
constexpr float kMidHz = 515.0f, kMidQ = 1.0f, kMidBoostDb = 5.0f, kMidCutDb = -7.0f;
// A cut filter is bypassed (dry, bit exact) with its frequency knob within kBypassEnter of the end that turns it off
// (low-cut fully CCW, high-cut fully CW), and comes back beyond kBypassLeave.
constexpr float kBypassEnter = 0.02f, kBypassLeave = 0.04f;
constexpr size_t kFadeSamples = 240;  // 5 ms: slope changes and bypass crossfade over this

// Knob 5, 0..1 -> 20..500 Hz, log.
inline float LowCutHz(float knob) { return kLowCutMinHz * std::pow(kLowCutMaxHz / kLowCutMinHz, knob); }
// Knob 4, 0..1 -> 1.5..20 kHz, log.
inline float HighCutHz(float knob) { return kHighCutMinHz * std::pow(kHighCutMaxHz / kHighCutMinHz, knob); }
// Knobs 6 and 3, 0..1 -> Q 0.5..2, log; 1 at noon.
inline float ResonanceQ(float knob) { return kResMinQ * std::pow(kResMaxQ / kResMinQ, knob); }

enum class Slope : uint8_t { k6, k12, k24 };

// SW1/SW2 position (controls.h: 0 down, 1 middle, 2 up) -> slope: up 6 dB, middle 12 dB, down 24 dB per octave.
inline Slope SlopeFromToggle(uint8_t pos) { return pos >= 2 ? Slope::k6 : pos == 1 ? Slope::k12 : Slope::k24; }
// SW3 position -> mid gain: up +5 dB, middle 0 (bypass), down -7 dB.
inline float MidGainDb(uint8_t pos) { return pos >= 2 ? kMidBoostDb : pos == 1 ? 0.0f : kMidCutDb; }

// A cut filter's off end, with hysteresis so a knob resting at the threshold does not chatter.
class EdgeBypass {
public:
    // at_zero: the filter turns off at knob 0 (low-cut); otherwise at 1 (high-cut).
    void Init(bool at_zero, float knob) {
        at_zero_ = at_zero;
        bypassed_ = at_zero ? knob <= kBypassEnter : knob >= 1.0f - kBypassEnter;
    }
    bool Process(float knob) {
        if (at_zero_)
            bypassed_ = bypassed_ ? knob <= kBypassLeave : knob <= kBypassEnter;
        else
            bypassed_ = bypassed_ ? knob >= 1.0f - kBypassLeave : knob >= 1.0f - kBypassEnter;
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

// A low-cut (high-pass) or high-cut (low-pass) with a 6, 12 or 24 dB/octave slope, resonance on the 12 and 24 dB
// slopes, and an off end. All three slopes run every block whatever the switch and the bypass, so the load is constant
// and a switch crossfades (kFadeSamples) between two settled outputs:
//   6 dB: a one-pole; 12 dB: an SVF at Q; 24 dB: a Butterworth SVF into an SVF at Q.
// At mix 0 (bypassed) the output is the input bit for bit; once a slope fade completes its weights are exactly one-hot.
class CutFilter {
public:
    // high_pass: the low-cut. toggle: the slope switch position (SlopeFromToggle). No fade: starts settled.
    void Init(bool high_pass, float freq_knob, float res_knob, uint8_t toggle) {
        high_pass_ = high_pass;
        freq_.Init(kSampleRate / float(kBlockSize), kKnobSmoothS, freq_knob);
        res_.Init(kSampleRate / float(kBlockSize), kKnobSmoothS, res_knob);
        bypass_.Init(high_pass, freq_knob);
        target_ = SlopeFromToggle(toggle);
        for (int j = 0; j < 3; ++j) from_[j] = j == int(target_) ? 1.0f : 0.0f;
        pos_ = kFadeSamples;
        mix_ = bypass_.Process(freq_knob) ? 0.0f : 1.0f;
        s1_ = q1_ = q2_ = a1_ = a2_ = b1_ = b2_ = 0.0f;
    }

    // Once per block, in place.
    DK_ITCM void Process(float* x, size_t n, float freq_knob, float res_knob, uint8_t toggle) {
        if (n == 0) return;
        const float fk = freq_.Process(freq_knob), rk = res_.Process(res_knob);
        const bool bypassed = bypass_.Process(fk);
        const float g = Prewarp(high_pass_ ? LowCutHz(fk) : HighCutHz(fk));
        const float q = ResonanceQ(rk);

        Run r;
        r.G = g / (1.0f + g);
        r.s12 = MakeSvf(g, q);
        r.sa = MakeSvf(g, kButterworthQ);
        r.sb = r.s12;

        const Slope want = SlopeFromToggle(toggle);
        if (want != target_) {
            for (int j = 0; j < 3; ++j) from_[j] = Weight(j, pos_);
            target_ = want;
            pos_ = 0;
        }
        const size_t end = pos_ + n < kFadeSamples ? pos_ + n : kFadeSamples;
        const float inv_n = 1.0f / float(n);
        for (int j = 0; j < 3; ++j) {
            r.w0[j] = Weight(j, pos_);
            r.dw[j] = (Weight(j, end) - r.w0[j]) * inv_n;
        }
        pos_ = end;

        const float step = float(n) / float(kFadeSamples);
        float m1 = bypassed ? mix_ - step : mix_ + step;
        m1 = m1 < 0.0f ? 0.0f : m1 > 1.0f ? 1.0f : m1;
        r.m0 = mix_;
        r.dm = (m1 - mix_) * inv_n;
        mix_ = m1;

        if (high_pass_)
            RunSamples<true>(x, n, r);
        else
            RunSamples<false>(x, n, r);
    }

private:
    struct Run {
        float G;               // one-pole
        SvfCoefs s12, sa, sb;  // 12 dB; 24 dB Butterworth stage, then its Q stage
        float w0[3], dw[3];    // slope weights (6, 12, 24): w0 + dw (i + 1) at sample i
        float m0, dm;          // wet mix, the same law
    };

    float Weight(int j, size_t pos) const {
        const float one = j == int(target_) ? 1.0f : 0.0f;
        if (pos >= kFadeSamples) return one;
        return from_[j] + (one - from_[j]) * (float(pos) / float(kFadeSamples));
    }

    // The per-sample loop. Statements are interleaved stage by stage so the one-pole, the 12 dB SVF and the 24 dB
    // Butterworth stage fill each other's latency (docs/M7_PERFORMANCE.md "Recursive filters"). Coefficients and
    // states are copied to locals: GCC must assume the float* x may alias the members.
    template <bool kHighPass>
    [[gnu::always_inline]] void RunSamples(float* x, size_t n, const Run& r) {
        const float G = r.G;
        const float k12 = r.s12.k, c1 = r.s12.a1, c2 = r.s12.a2, c3 = r.s12.a3;
        const float ka = r.sa.k, d1 = r.sa.a1, d2 = r.sa.a2, d3 = r.sa.a3;
        const float kb = r.sb.k, e1 = r.sb.a1, e2 = r.sb.a2, e3 = r.sb.a3;
        const float w6 = r.w0[0], w12 = r.w0[1], w24 = r.w0[2], dw6 = r.dw[0], dw12 = r.dw[1], dw24 = r.dw[2];
        const float m0 = r.m0, dm = r.dm;
        float s1 = s1_, q1 = q1_, q2 = q2_, a1 = a1_, a2 = a2_, b1 = b1_, b2 = b2_;
        for (size_t i = 0; i < n; ++i) {
            const float xin = x[i];
            const float t = float(i + 1);
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
    Slope target_ = Slope::k12;  // the slope faded toward
    float from_[3] = {};         // the weights the fade started from
    size_t pos_ = kFadeSamples;  // samples into the fade
    float mix_ = 1.0f;           // wet amount, 0 bypassed
    float s1_ = 0.0f;              // one-pole
    float q1_ = 0.0f, q2_ = 0.0f;  // 12 dB SVF
    float a1_ = 0.0f, a2_ = 0.0f;  // 24 dB Butterworth stage
    float b1_ = 0.0f, b2_ = 0.0f;  // 24 dB Q stage
};

// SW3: a bell at kMidHz, Q kMidQ (constant-Q form: k = 1 / (Q A)), gain smoothed in dB. At 0 dB the output is the
// input bit for bit.
class MidFilter {
public:
    void Init(uint8_t toggle) {
        db_.Init(kSampleRate / float(kBlockSize), kKnobSmoothS, MidGainDb(toggle));
        g_ = Prewarp(kMidHz);
        ic1_ = ic2_ = 0.0f;
    }

    // Once per block, in place.
    DK_ITCM void Process(float* x, size_t n, uint8_t toggle) {
        const float db = db_.Process(MidGainDb(toggle));
        const float A = DbToGain(0.5f * db);  // 10^(dB / 40)
        const SvfCoefs c = MakeSvf(g_, kMidQ * A);
        const float m1 = std::fabs(db) < 1e-3f ? 0.0f : c.k * (A * A - 1.0f);
        const float a1 = c.a1, a2 = c.a2, a3 = c.a3;
        float ic1 = ic1_, ic2 = ic2_;
        for (size_t i = 0; i < n; ++i) {
            const float xin = x[i];
            const float v3 = xin - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            x[i] = xin + m1 * v1;
        }
        ic1_ = ic1;
        ic2_ = ic2;
    }

private:
    Smoother db_;
    float g_ = 0.0f;
    float ic1_ = 0.0f, ic2_ = 0.0f;
};

// The pedal's EQ controls: knobs 0..1, toggles as controls.h reports them (0 down, 1 middle, 2 up).
struct EqControls {
    float low_cut, low_res, high_cut, high_res;
    uint8_t sw1, sw2, sw3;
};

// Low-cut, then mid, then high-cut, in place.
class Eq {
public:
    void Init(const EqControls& c) {
        low_.Init(true, c.low_cut, c.low_res, c.sw1);
        mid_.Init(c.sw3);
        high_.Init(false, c.high_cut, c.high_res, c.sw2);
    }
    DK_ITCM void Process(float* x, size_t n, const EqControls& c) {
        low_.Process(x, n, c.low_cut, c.low_res, c.sw1);
        mid_.Process(x, n, c.sw3);
        high_.Process(x, n, c.high_cut, c.high_res, c.sw2);
    }

private:
    CutFilter low_, high_;
    MidFilter mid_;
};

}  // namespace dkdsp
