// The compressor (docs/compressor-spec.md "DSP model"): the reduced Dinosaural OTC-201 / Aion Convex, between the input
// jack and Knob 1's trim. Mono, in or out of place, no latency, no libDaisy, no allocation. Its inputs are the
// compressor's parameters (params.h), read once per control tick.
// The model works in volts with the circuit's polarity (kVfs volts per unit sample): an input high-pass into the Level
// stage (dry), the 0.894 divider into a non-inverting stage whose feedback conductance Gf is Sustain plus the two
// photocells, a trapezoidal C6, clamped at the op-amp's rails (wet); per sample the wet output drives two detector
// pairs through a 46 Hz coupling, and per tick (24 samples) each pair's charge feeds its reservoir, the reservoir its
// LED (3 Newton steps), the LED its cell, and the cells Gf's target for the next tick, which Gf ramps to linearly.
// The output is the mix of dry and wet (comp_mode's blend or direct, Balance), sign-flipped back, so the block does not
// invert.
// Settings: Sustain is the stage's gain in dB (Rsus = 10 kOhm (10^(dB/20) - 1)); Level picks the Level stage's gain
// and its input corner; Balance is the knob's rotation, through the audio taper (81^v - 1) / 80. Sustain (dB), the
// Level stage's gain and corner and the two mix weights glide (kKnobSmoothS); the gain and weights ramp linearly
// across each tick, so the output has no steps.
// comp_mode off and on crossfades over kFadeSamples (a fade out keeps the last mix that was on); off and faded out,
// the output is the input bit for bit and nothing runs, and turning it on restarts it from rest (the side chain dark)
// at the current settings.
// Every update counts samples from the tick's start, so the output does not depend on how the caller splits its
// blocks. Covered by tests/compressor_test.cpp against the reference implementation (tools/comp_reference.py).
#pragma once

#include <cmath>
#include <cstddef>

#include "../memory.h"
#include "../params.h"
#include "blocks.h"
#include "filters.h"
#include "gain.h"

namespace dkdsp {

class Compressor {
public:
    static constexpr int kFadeSamples = 960;  // comp_mode off and on crossfade (20 ms)

    // v: kParamCount values indexed by dkparam::ParamId, already Quantize()d. Settled at v, the side chain at rest.
    void Init(const float* v) {
        gC5_ = OnePole(float(1.0 / (2.0 * M_PI * 8.2e3 * 10e-9)));
        gDet_ = OnePole(float(1.0 / (2.0 * M_PI * 33e-9 * 104.7e3)));
        for (int l = 0; l < 3; ++l) gInLevel_[l] = OnePole(kInHz[l]);
        ReadMode(v);
        fade_ = enable_ ? kFadeSamples : 0;
        Start(v);
    }

    // n samples from in to out (in == out allowed); v as for Init, read at each control tick's start.
    DK_ITCM void Process(const float* in, float* out, size_t n, const float* v) {
        while (n > 0) {
            if (at_ == 0) Control(v);
            const size_t m = n < kBlockSize - at_ ? n : kBlockSize - at_;
            RunChunk(in, out, m);
            in += m;
            out += m;
            n -= m;
            at_ += m;
            if (at_ == kBlockSize) {
                if (fade_ > 0 || enable_) SideChain();
                at_ = 0;
            }
        }
    }

    // The stage's low-frequency gain over Sustain's at the current tick, in (0, 1]; 1 when off.
    float gain() const { return gain_; }

private:
    // Circuit constants (docs/compressor-spec.md "Constants").
    static constexpr float kVfs = 1.8395f;  // volts per unit sample: 0 dBFS = +4.5 dBu peak (gain.h kInputLevelDbu)
    static constexpr float kBuffer = 0.9416f;                           // Q1 into the Level stage
    static constexpr float kLevelGain[3] = {0.697f, 1.0f, 1.413f};      // Level down, middle, up
    static constexpr float kInHz[3] = {32.7f, 32.7f, 46.2f};            // C2 into the Level stage
    static constexpr float kDiv = 0.894f;                               // R9, R10
    static constexpr float kG11 = 1e-4f;                                // 1 / R11
    static constexpr float kG12 = 1.0f / 8200.0f;                       // 1 / R12
    static constexpr float kA2 = 2.0f * 330e-12f * 48000.0f;            // C6, trapezoidal
    static constexpr float kClipLo = -2.85f, kClipHi = 2.50f;           // the op-amp's rails around VB
    static constexpr float kDet = 0.955f;                               // R14 / (R13 + R14)
    static constexpr float kInvRq = 1.0f / 190.0f;                      // pair slope
    static constexpr float kV1[2] = {0.385f, 0.405f};                   // pair knee start: positive, negative half
    static constexpr float kTwoD[2] = {2.0f * 0.120f, 2.0f * 0.135f};   // pair knee width
    static constexpr float kInv4D[2] = {1.0f / (4.0f * 0.120f), 1.0f / (4.0f * 0.135f)};
    static constexpr float kInvC = 1e5f, kW0 = 0.95f, kWMax = 7.8f;     // reservoir: 1 / 10 uF, rest, ceiling (V)
    static constexpr float kRl = 2202.3f, kNvt = 0.03412f, kIs = 2.1e-22f;  // LED: series ohms, n VT, Is (A)
    static constexpr float kTauOn = 0.010f, kTauS = 0.43f, kK = 6.0f;   // cell rise (s), fall (s, 1 / (mA s))
    static constexpr float kRmin = 75.0f, kA = 1711.0f, kP = 1.42f, kNFloor = 1e-6f;  // cell resistance, n floor (mA)
    static constexpr float kBalanceBase = 81.0f;  // audio taper (81^v - 1) / 80: 0.1 at noon, 0 and 1 at the ends
    static constexpr float kTb = float(kBlockSize) / kSampleRate;  // a tick, s

    // One detector pair's side chain: charge this tick (sum of pair currents, A), reservoir (V), LED (A), cell (mA),
    // cell conductance (S).
    struct Half {
        float acc, w, i, n, gc;
    };
    // The settings' targets.
    struct Targets {
        float db, kd, gIn, wDry, wWet;
    };

    static float OnePole(float hz) {
        const float g = Prewarp(hz);
        return g / (1.0f + g);
    }

    Targets TargetsOf(const float* v) const {
        using namespace dkparam;
        const int lv = int(v[kCompLevel]);
        const float x = (std::pow(kBalanceBase, v[kCompBalance]) - 1.0f) / 80.0f;
        Targets t;
        t.db = Units(kParams[kCompSustain], v[kCompSustain]);
        t.kd = -kLevelGain[lv] * kBuffer;
        t.gIn = gInLevel_[lv];
        t.wDry = blend_ ? -(1.0f - x) / kVfs : 0.0f;
        t.wWet = -x / kVfs;
        return t;
    }

    // comp_mode: on or off, and while on, blend or direct.
    void ReadMode(const float* v) {
        const float m = v[dkparam::kCompMode];
        enable_ = m > 0.5f;
        if (enable_) blend_ = m < 1.5f;
    }

    // 1 / Rsus for Sustain at db (a short at 0 dB clamps to 1 S).
    static float SustainConductance(float db) {
        const float r = 10000.0f * (std::pow(10.0f, db / 20.0f) - 1.0f);
        return 1.0f / std::fmax(r, 1.0f);
    }

    // Settled at v, everything at rest, at a tick's start.
    [[gnu::noinline]] void Start(const float* v) {
        const Targets t = TargetsOf(v);
        sIn_ = sC5_ = sDet_ = vf_ = ig_ = 0.0f;
        for (Half& h : half_) h = Half{0.0f, kW0, 0.0f, kNFloor, 0.0f};
        sustainDb_.Init(kBlockRate, kKnobSmoothS, t.db);
        kd_.Init(kBlockRate, kKnobSmoothS, t.kd);
        gIn_.Init(kBlockRate, kKnobSmoothS, t.gIn);
        wDry_.Init(kBlockRate, kKnobSmoothS, t.wDry);
        wWet_.Init(kBlockRate, kKnobSmoothS, t.wWet);
        kd0_ = kd1_ = t.kd;
        wd0_ = wd1_ = t.wDry;
        ww0_ = ww1_ = t.wWet;
        gInNow_ = t.gIn;
        gSus_ = SustainConductance(t.db);
        gf_ = gfStart_ = gfT_ = gSus_;
        gain_ = 1.0f;
        at_ = 0;
    }

    // Once per control tick: the enable, the glides and their ramps, Gf's ramp to the cells' last conductance.
    [[gnu::noinline]] DK_ITCM void Control(const float* v) {
        ReadMode(v);
        if (fade_ == 0) {
            if (!enable_) {  // nothing runs
                gain_ = 1.0f;
                return;
            }
            Start(v);  // the stage restarts from rest at the current settings
        }
        const Targets t = TargetsOf(v);
        kd0_ = kd1_;
        kd1_ = kd_.Process(t.kd);
        wd0_ = wd1_;
        wd1_ = wDry_.Process(t.wDry);
        ww0_ = ww1_;
        ww1_ = wWet_.Process(t.wWet);
        gInNow_ = gIn_.Process(t.gIn);
        gSus_ = SustainConductance(sustainDb_.Process(t.db));
        gfStart_ = gf_;
        gfT_ = (gSus_ + half_[0].gc) + half_[1].gc;
        gain_ = ((gfT_ + kG11) * gSus_) / (gfT_ * (gSus_ + kG11));
    }

    // One Newton step of the LED's w = RL i + nVT ln(1 + i / Is) from i.
    static float LedStep(float i, float w) {
        const float f = kRl * i + kNvt * std::log1p(i / kIs) - w;
        return std::fmax(i - f / (kRl + kNvt / (i + kIs)), 1e-15f);
    }

    // Half h at a tick's end: reservoir, LED, cell, conductance (docs/compressor-spec.md "Per block").
    static void Tick(Half& h) {
        const float q = h.acc * (1.0f / kSampleRate);
        h.acc = 0.0f;
        h.w = std::fmin(h.w + (q - h.i * kTb) * kInvC, kWMax);
        float i = std::fmax(h.i, 1e-12f);
        i = LedStep(i, h.w);
        i = LedStep(i, h.w);
        i = LedStep(i, h.w);
        h.i = i;
        const float led = i * 1000.0f;
        float n = h.n;
        if (led > n)
            n += (kTb / kTauOn) * (led - n);
        else
            n -= kTb * (n - led) * (1.0f / kTauS + kK * n);
        h.n = std::fmax(n, kNFloor);
        h.gc = 1.0f / (kRmin + kA * std::pow(h.n, -kP));
    }

    // When a running tick's last sample is done.
    [[gnu::noinline]] DK_ITCM void SideChain() {
        Tick(half_[0]);
        Tick(half_[1]);
    }

    // A pair's current for base drive u: zero below v1, a parabola over twoD, then 1 / Rq.
    static float Pair(float u, float v1, float twoD, float inv4D) {
        const float t = std::fmin(std::fmax(u - v1, 0.0f), twoD);
        return (t * t * inv4D + std::fmax(u - v1 - twoD, 0.0f)) * kInvRq;
    }

    // m samples from in to out, at_ samples into the current tick.
    DK_ITCM void RunChunk(const float* in, float* out, size_t m) {
        const int want = enable_ ? kFadeSamples : 0;
        if (fade_ == 0 && want == 0) {  // true bypass
            if (in != out)
                for (size_t i = 0; i < m; ++i) out[i] = in[i];
            return;
        }
        const bool fading = fade_ != want;
        float dry[kBlockSize];
        if (fading)
            for (size_t i = 0; i < m; ++i) dry[i] = in[i];
        const float gIn = gInNow_, gC5 = gC5_, gDet = gDet_;
        const float kd0 = kd0_, kdD = kd1_ - kd0_, wd0 = wd0_, wdD = wd1_ - wd0_, ww0 = ww0_, wwD = ww1_ - ww0_;
        const float gfStart = gfStart_, gfD = gfT_ - gfStart_;
        float sIn = sIn_, sC5 = sC5_, sDet = sDet_, vf = vf_, igPrev = ig_, gf = gf_;
        float accP = half_[0].acc, accN = half_[1].acc;
        for (size_t i = 0; i < m; ++i) {
            const float t = float(at_ + i + 1) * (1.0f / float(kBlockSize));
            const float v = kVfs * in[i];
            float a = (v - sIn) * gIn;  // input high-pass
            const float lp = a + sIn;
            sIn = lp + a;
            const float vd = (kd0 + kdD * t) * (v - lp);  // dry: buffer and Level stage, inverted
            const float vp = kDiv * vd;                   // pin 5
            a = (vp - sC5) * gC5;                         // C5's voltage
            const float vc5 = a + sC5;
            sC5 = vc5 + a;
            const float ig = vp * kG11 + (vp - vc5) * kG12;  // current in the ground leg
            const float gfPrev = gf;
            gf = gfStart + gfD * t;
            vf = ((kA2 - gfPrev) * vf + ig + igPrev) / (kA2 + gf);  // C6 dvf/dt = ig - Gf vf, trapezoidal
            const float vo = std::fmin(std::fmax(vp + vf, kClipLo), kClipHi);  // pin 7
            vf = vo - vp;
            igPrev = ig;
            a = (vo - sDet) * gDet;  // detector coupling
            const float lp2 = a + sDet;
            sDet = lp2 + a;
            const float u = kDet * (vo - lp2);
            accP += Pair(u, kV1[0], kTwoD[0], kInv4D[0]);
            accN += Pair(-u, kV1[1], kTwoD[1], kInv4D[1]);
            out[i] = (wd0 + wdD * t) * vd + (ww0 + wwD * t) * vo;
        }
        sIn_ = sIn;
        sC5_ = sC5;
        sDet_ = sDet;
        vf_ = vf;
        ig_ = igPrev;
        gf_ = gf;
        half_[0].acc = accP;
        half_[1].acc = accN;
        if (!fading) return;
        // Full fade: the wet sample as is (the blend's dry + (x - dry) can round); 0: dry + 0 is dry.
        for (size_t i = 0; i < m; ++i) {
            fade_ += fade_ < want ? 1 : fade_ > want ? -1 : 0;
            if (fade_ != kFadeSamples) out[i] = dry[i] + float(fade_) / float(kFadeSamples) * (out[i] - dry[i]);
        }
    }

    float gC5_ = 0.0f, gDet_ = 0.0f, gInLevel_[3] = {};
    float sIn_ = 0.0f, sC5_ = 0.0f, sDet_ = 0.0f, vf_ = 0.0f, ig_ = 0.0f;  // filter states, C6, the last ig
    float gf_ = 0.0f, gfStart_ = 0.0f, gfT_ = 0.0f;  // Gf at the last sample, at the tick's start, its target
    Half half_[2] = {};                              // positive, negative half
    Smoother sustainDb_, kd_, gIn_, wDry_, wWet_;    // kd: -gL gB; wDry, wWet: the mix weights over -kVfs
    float kd0_ = 0.0f, kd1_ = 0.0f, wd0_ = 0.0f, wd1_ = 0.0f, ww0_ = 0.0f, ww1_ = 0.0f;  // the tick's ramps
    float gInNow_ = 0.0f, gSus_ = 1.0f, gain_ = 1.0f;
    bool enable_ = false, blend_ = true;  // blend_: the mix of the last tick that was on
    int fade_ = 0;
    size_t at_ = 0;
};

}  // namespace dkdsp
