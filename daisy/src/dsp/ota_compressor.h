// The OTA compressor (docs/ota-compressor-spec.md "DSP model"): the reduced MXR Dyna Comp / Ross Compressor, comp_type
// OTA, in series after the OTC-201 (compressor.h), so between the input jack and Knob 1's trim. Mono, in or out of
// place, no latency, no libDaisy, no allocation. Its inputs are the compressor's parameters (params.h), read once per
// control tick.
// The model works in AC volts with the circuit's polarity (kVfs volts per unit sample): an input high-pass into Q1
// (dry), a shelf into the CA3080, whose current Iabc tanh(vd / 2VT) charges its load (RL, C5, trapezoidal) within
// clamps; Q2 splits that into its emitter (the wet output, after a 53 Hz coupling) and its inverted collector, which
// drive the two detector halves (a coupling capacitor into a clamped base, solved exactly by backward Euler); their
// collectors and R13 move the envelope C8, and the envelope sets the next sample's Iabc. Every state runs per sample;
// C8 is kept less Voff and summed with compensation, so float32 follows the reference from compression back to rest.
// The output is the mix of dry and wet (comp_mode's blend or direct, Balance), the wet sign-flipped back, so the block
// does not invert.
// Settings: Sustain is the Sensitivity pot's rotation s, Rs = 527 kOhm (27 / 527)^s (the gain below threshold linear
// in dB over 25.8 dB); Level is a gain of -3, 0 or +3 dB ahead of the circuit; Balance is the knob's rotation, through
// the audio taper (81^v - 1) / 80. s, Level's gain and the two mix weights glide (kKnobSmoothS); 1 / Rs, the gain and
// the weights ramp linearly across each tick, so the output has no steps.
// It runs with comp_mode on and comp_type OTA. Off and on crossfade over kFadeSamples (a fade out keeps the last mix
// that was on), so a comp_type switch fades the OTC out and this in at once; off and faded out, the output is the
// input bit for bit and nothing runs, and turning it on restarts it from rest (C8 charged) at the current settings.
// Every update counts samples from the tick's start, so the output does not depend on how the caller splits its
// blocks. Covered by tests/ota_compressor_test.cpp against the reference implementation (tools/ota_reference.py).
#pragma once

#include <cmath>
#include <cstddef>
#include <limits>

#include "../memory.h"
#include "../params.h"
#include "blocks.h"
#include "filters.h"
#include "gain.h"

namespace dkdsp {

class OtaCompressor {
public:
    static constexpr int kFadeSamples = kCrossfadeSamples;

    // v: kParamCount values indexed by dkparam::ParamId, already Quantize()d. Settled at v, C8 at rest.
    void Init(const float* v) {
        gIn_ = OnePoleGain(kFin);
        gSh_ = OnePoleGain(kFp);
        gOut_ = OnePoleGain(kFout);
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

    // Iabc over its value at rest at the current Sustain, at the last tick's end, in [0, 1]; 1 when off.
    float gain() const { return gain_; }

private:
    // Circuit constants (docs/ota-compressor-spec.md "Constants").
    // Volts per unit sample: 0 dBFS = +4.5 dBu RMS sine (1.8395 V peak; gain.h kInputLevelDbu).
    static constexpr float kVfs = 1.8395f;
    static constexpr float kBuf = 0.92238f, kFin = 41.81f;    // Q1 into the OTA: gain, input high-pass (Hz)
    static constexpr float kK0 = float(2.0 / 17.0), kFp = 9018.9f;  // the input shelf: low-frequency gain, pole (Hz)
    static constexpr float kInvVt2 = float(1.0 / 0.051704);   // 1 / 2VT
    static constexpr double kRl = 134.5e3, kA6 = 2.0 * 1e-9 * 48000.0;  // the OTA's load; C5, trapezoidal
    static constexpr float kLoadKeep = float((kA6 - 1.0 / kRl) / (kA6 + 1.0 / kRl));
    static constexpr float kLoadIn = float(1.0 / (kA6 + 1.0 / kRl));
    static constexpr float kClipLo = -1.244f, kClipHi = 2.944f;  // the OTA's node: its output clamp, Q2's saturation
    static constexpr float kKe = 0.98120f, kKc = 1.1552f;  // Q2: emitter over the OTA's node, collector over emitter
    static constexpr float kFout = 53.05f;                 // C9 into R14 + RV2
    // C8's feed, Q3 and the bias pin. The envelope is kept as C8 less Voff (what Iabc reads: in compression a few
    // tenths of a volt, so float32 resolves it finely), from kEnvFloor (VSAT less Voff) up towards kEnvTop.
    static constexpr double kVcc = 9.0, kR13 = 150e3, kVoff = 1.1499, kBetaF = 109.32;
    static constexpr float kEnvTop = float(kVcc - kVoff), kEnvFloor = float(0.1 - kVoff), kInvR13 = float(1.0 / kR13);
    static constexpr float kInvBetaF = float(1.0 / kBetaF);
    static constexpr float kDt8 = float(1.0 / (48000.0 * 10e-6));  // T / C8
    // A detector half: coupling C7 = 10 nF, R12 = 1 MOhm; base knee and slope; diode knee and slope; beta.
    static constexpr double kA = 1.0 / (48000.0 * 10e-9), kG0 = 1e-6, kVbe = 0.399, kRbe = 268.0;
    static constexpr double kVd = 0.543, kRd = 25.0, kBetaD = 149.6;
    static constexpr float kIk0 = float(1.0 / (1.0 + kA * kG0)), kIk1 = float(1.0 / (1.0 + kA * (kG0 + 1.0 / kRbe)));
    static constexpr float kIk2 = float(1.0 / (1.0 + kA * (kG0 + 1.0 / kRd)));
    static constexpr float kU1 = float(kVbe * (1.0 + kA * kG0)), kU2 = float(-kVd * (1.0 + kA * kG0));
    static constexpr float kIcGain = float(kBetaD / kRbe / (1.0 + kA * (kG0 + 1.0 / kRbe)));  // BETA_D IK1 / RBE
    // Sustain: 1 / Rs = (1 / 527 kOhm) (527 / 27)^s.
    static constexpr float kInvRsMin = float(1.0 / 527e3), kRsRatio = float(527.0 / 27.0);
    static constexpr float kLevelGain[3] = {0.70794578f, 1.0f, 1.41253754f};  // Level down, middle, up

    // The settings' targets.
    struct Targets {
        float pos, gl, wDry, wWet;
    };

    Targets TargetsOf(const float* v) const {
        using namespace dkparam;
        const float x = CompBalanceTaper(v[kCompBalance]);
        Targets t;
        t.pos = v[kCompSustain];
        t.gl = kLevelGain[int(v[kCompLevel])];
        t.wDry = blend_ ? (1.0f - x) / kVfs : 0.0f;
        t.wWet = -x / kVfs;
        return t;
    }

    // comp_mode and comp_type: on (comp_mode on, comp_type OTA) or off, and while on, blend or direct.
    void ReadMode(const float* v) {
        const float m = v[dkparam::kCompMode];
        enable_ = m > 0.5f && v[dkparam::kCompType] > 0.5f;
        if (enable_) blend_ = m < 1.5f;
    }

    static float InvRs(float pos) { return kInvRsMin * std::pow(kRsRatio, pos); }
    // Iabc at rest for 1 / Rs = g: C8 where R13's current meets Q3's base current, less Voff, over Rs.
    static float RestIabc(float g) {
        return g * float(kVcc - kVoff) / (1.0f + float(kR13 / kBetaF) * g);
    }

    // Settled at v, everything at rest, at a tick's start.
    [[gnu::noinline]] void Start(const float* v) {
        const Targets t = TargetsOf(v);
        KeepTargets(t, v);
        sIn_ = sSh_ = sOut_ = vo_ = i_ = vcTop_ = vcBottom_ = 0.0f;
        pos_.Init(kBlockRate, kKnobSmoothS, t.pos);
        gl_.Init(kBlockRate, kKnobSmoothS, t.gl);
        wDry_.Init(kBlockRate, kKnobSmoothS, t.wDry);
        wWet_.Init(kBlockRate, kKnobSmoothS, t.wWet);
        gl0_ = gl1_ = t.gl;
        wd0_ = wd1_ = t.wDry;
        ww0_ = ww1_ = t.wWet;
        posNow_ = t.pos;
        invRs_ = invRs0_ = invRs1_ = InvRs(t.pos);
        iabcRest_ = RestIabc(invRs_);
        const float g = invRs_ * kInvBetaF;  // C8 at rest
        env_ = kEnvTop * kInvR13 / (kInvR13 + g);
        envLo_ = 0.0f;
        iabc_ = iabcRest_;
        gain_ = 1.0f;
        at_ = 0;
    }

    // The targets of v and the settings they came from.
    void KeepTargets(const Targets& t, const float* v) {
        using namespace dkparam;
        targets_ = t;
        keyLevel_ = v[kCompLevel];
        keyBalance_ = v[kCompBalance];
        keySustain_ = v[kCompSustain];
        keyBlend_ = blend_;
    }

    // Once per control tick: the enable, the glides and their ramps. The targets (a pow) and 1 / Rs (a pow) are
    // recomputed only when what they come from has changed.
    [[gnu::noinline]] DK_ITCM void Control(const float* v) {
        using namespace dkparam;
        ReadMode(v);
        if (fade_ == 0) {
            if (!enable_) {  // nothing runs
                gain_ = 1.0f;
                return;
            }
            Start(v);  // the circuit restarts from rest at the current settings
        }
        if (v[kCompLevel] != keyLevel_ || v[kCompBalance] != keyBalance_ || v[kCompSustain] != keySustain_ ||
            blend_ != keyBlend_)
            KeepTargets(TargetsOf(v), v);
        const Targets& t = targets_;
        gl0_ = gl1_;
        gl1_ = gl_.Process(t.gl);
        wd0_ = wd1_;
        wd1_ = wDry_.Process(t.wDry);
        ww0_ = ww1_;
        ww1_ = wWet_.Process(t.wWet);
        const float pos = pos_.Process(t.pos);
        if (pos != posNow_) {
            invRs_ = InvRs(pos);
            iabcRest_ = RestIabc(invRs_);
            posNow_ = pos;
        }
        invRs0_ = invRs1_;
        invRs1_ = invRs_;
    }

    // When a running tick's last sample is done: the meter.
    [[gnu::noinline]] DK_ITCM void SideChain() { gain_ = std::fmin(iabc_ / iabcRest_, 1.0f); }

    // A detector half driven by vs, its coupling at vc: the collector current (docs/ota-compressor-spec.md "Per
    // sample"). The base node's backward-Euler equation is piecewise linear, so it is solved exactly. vb - VBE is
    // (u - U1) IK1 above the knee: taken from u - U1, it does not carry the rounding of U1 IK0 against VBE, which
    // near the knee is a sizeable part of it.
    static float Half(float vs, float& vc) {
        const float u = vs - vc;
        const float above = std::fmax(u - kU1, 0.0f);
        const float vb = std::fmin(std::fmax(u, kU2), kU1) * kIk0 + above * kIk1 + std::fmin(u - kU2, 0.0f) * kIk2;
        vc = vs - vb;
        return kIcGain * above;
    }

    // tanh's (7, 6) Pade form, clamped to +-1.
    static float Tanh(float x) {
        const float x2 = x * x;
        const float y = x * (135135.0f + x2 * (17325.0f + x2 * (378.0f + x2))) /
                        (135135.0f + x2 * (62370.0f + x2 * (3150.0f + 28.0f * x2)));
        return std::fmin(std::fmax(y, -1.0f), 1.0f);
    }

    // m samples from in to out, at_ samples into the current tick.
    DK_ITCM void RunChunk(const float* in, float* out, size_t m) {
        const int want = enable_ ? kFadeSamples : 0;
        if (fade_ == 0 && want == 0) {  // true bypass
            if (in != out) CopyFloats(out, in, m);
            return;
        }
        const bool fading = fade_ != want;
        float dry[kBlockSize];
        if (fading) CopyFloats(dry, in, m);
        const float gIn = gIn_, gSh = gSh_, gOut = gOut_;
        const float gl0 = gl0_, glD = gl1_ - gl0_, wd0 = wd0_, wdD = wd1_ - wd0_, ww0 = ww0_, wwD = ww1_ - ww0_;
        const float rs0 = invRs0_, rsD = invRs1_ - invRs0_;
        float sIn = sIn_, sSh = sSh_, sOut = sOut_, vo = vo_, iPrev = i_, env = env_, envLo = envLo_, iabc = iabc_;
        float vcTop = vcTop_, vcBottom = vcBottom_;
        for (size_t k = 0; k < m; ++k) {
            const float t = float(at_ + k + 1) * kInvBlockSize;
            const float v = kVfs * in[k];
            float a = (v - sIn) * gIn;  // input high-pass
            float lp = a + sIn;
            sIn = lp + a;
            const float vq = kBuf * (gl0 + glD * t) * (v - lp);  // dry: Q1's emitter
            a = (vq - sSh) * gSh;                                  // input shelf
            lp = a + sSh;
            sSh = lp + a;
            const float vd = kK0 * lp + (vq - lp);  // the OTA's drive, inverting input
            iabc = std::fmax(env, 0.0f) * (rs0 + rsD * t);
            const float i = -iabc * Tanh(vd * kInvVt2);
            vo = std::fmin(std::fmax(kLoadKeep * vo + kLoadIn * (i + iPrev), kClipLo), kClipHi);  // C5, RL
            iPrev = i;
            const float ve = kKe * vo;  // Q2's emitter; its collector is -kKc ve
            const float ic = Half(-kKc * ve, vcTop) + Half(ve, vcBottom);
            // C8 by forward Euler, summed with compensation: near rest a step is below half of env's last place, and
            // a plain float32 sum stalls about 30 mV short of rest.
            const float step = kDt8 * ((kEnvTop - env) * kInvR13 - ic - iabc * kInvBetaF) - envLo;
            const float sum = env + step;
            envLo = (sum - env) - step;
            env = std::fmax(sum, kEnvFloor);
            a = (ve - sOut) * gOut;  // output coupling
            lp = a + sOut;
            sOut = lp + a;
            out[k] = (wd0 + wdD * t) * vq + (ww0 + wwD * t) * (ve - lp);
        }
        sIn_ = sIn;
        sSh_ = sSh;
        sOut_ = sOut;
        vo_ = vo;
        i_ = iPrev;
        env_ = env;
        envLo_ = envLo;
        iabc_ = iabc;
        vcTop_ = vcTop;
        vcBottom_ = vcBottom;
        if (!fading) return;
        FinishCrossfade(out, dry, m, fade_, want);
    }

    float gIn_ = 0.0f, gSh_ = 0.0f, gOut_ = 0.0f;
    float sIn_ = 0.0f, sSh_ = 0.0f, sOut_ = 0.0f;  // filter states
    float vo_ = 0.0f, i_ = 0.0f;                   // the OTA's node, its last current
    float env_ = 0.0f, envLo_ = 0.0f, iabc_ = 0.0f;  // C8 less Voff, its sum's lost low part; the last Iabc
    float vcTop_ = 0.0f, vcBottom_ = 0.0f;         // the detector couplings
    Smoother pos_, gl_, wDry_, wWet_;              // Sustain's position, Level's gain, the mix weights over kVfs
    float gl0_ = 0.0f, gl1_ = 0.0f, wd0_ = 0.0f, wd1_ = 0.0f, ww0_ = 0.0f, ww1_ = 0.0f;  // the tick's ramps
    float invRs_ = 0.0f, invRs0_ = 0.0f, invRs1_ = 0.0f;  // 1 / Rs of the glided position; the tick's ramp
    float iabcRest_ = 1.0f, gain_ = 1.0f;
    float posNow_ = std::numeric_limits<float>::quiet_NaN();  // the glided position invRs_ is of
    Targets targets_ = {};  // TargetsOf the settings in the keys (NaN: none yet)
    float keyLevel_ = std::numeric_limits<float>::quiet_NaN(), keyBalance_ = keyLevel_, keySustain_ = keyLevel_;
    bool keyBlend_ = false;
    bool enable_ = false, blend_ = true;  // blend_: the mix of the last tick that was on
    int fade_ = 0;
    size_t at_ = 0;
};

}  // namespace dkdsp
