// The EQ (docs/EQ.md): low-cut, bells 1-3, high-cut in series, after the IR. Mono, in place, no latency, no libDaisy,
// no allocation. Its inputs are the EQ parameters' values (params.h), read once per control tick.
// Laws: the cuts are CutFilter (filters.h: 6, 12 or 24 dB/oct, resonance, an off end); each bell is the RBJ cookbook
// bell (DesignBell), its analogue prototype under the bilinear transform prewarped at its frequency, realised as
// A. Simper's linear trapezoidal SVF mixed to that response (x + m1 band). Bell coefficients are designed in double;
// every section filters in float.
// Changes: the cuts smooth their positions per tick (CutFilter); bell gains glide in dB, frequencies in log2 Hz and Qs
// in log2 Q (one-pole, kKnobSmoothS, stepped every tick); a bell whose glides moved redesigns its coefficients on that
// tick and ramps them linearly across it (Bell::RampTo), so the output has no steps. A bell starting from 0 dB starts
// at its identity and ramps from there.
// eq_enable crossfades over kFadeSamples. A bell whose gain rests at 0 dB for kIdleTicks stops and its state is
// zeroed, and a cut at its off end is bypassed bit for bit, so the flat EQ's output is bit-identical to its input;
// eq_enable off returns the input untouched.
// Every update counts samples from the tick's start, so the output does not depend on how the caller splits its
// blocks. Covered by tests/eq_test.cpp.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "../memory.h"
#include "../params.h"
#include "blocks.h"
#include "filters.h"
#include "gain.h"

namespace dkdsp {

class Eq {
public:
    static constexpr int kFadeSamples = 960;  // eq_enable crossfade (20 ms)
    static constexpr int kIdleTicks = 200;    // control ticks (100 ms) at 0 dB before a bell stops
    static constexpr int kBells = 3;

    // v: kParamCount values indexed by dkparam::ParamId, already Quantize()d. Settled at v, state zero.
    void Init(const float* v) {
        using namespace dkparam;
        a_ = 1.0 - std::exp(-1.0 / (double(kKnobSmoothS) * double(kBlockRate)));
        for (int b = 0; b < kBells; ++b) {
            Band& s = band_[b];
            const ParamDef& g = kParams[kBell1Gain + 3 * b];
            const ParamDef& f = kParams[kBell1Freq + 3 * b];
            const ParamDef& q = kParams[kBell1Q + 3 * b];
            s.db0 = g.min;
            s.dbSpan = double(g.max) - double(g.min);
            s.dbZero = s.dbSpan / double(kCodeMax);
            s.lf0 = std::log2(double(f.min));
            s.lfSpan = std::log2(double(f.max) / double(f.min));
            s.lq0 = std::log2(double(q.min));
            s.lqSpan = std::log2(double(q.max) / double(q.min));
        }
        enable_ = v[kEqEnable] > 0.5f;
        fade_ = enable_ ? kFadeSamples : 0;
        Start(v);
    }

    // n samples in place; v as for Init, read at each control tick's start.
    DK_ITCM void Process(float* x, size_t n, const float* v) {
        while (n > 0) {
            if (at_ == 0) Control(v);
            const size_t m = n < kBlockSize - at_ ? n : kBlockSize - at_;
            RunChunk(x, m);
            x += m;
            n -= m;
            at_ += m;
            if (at_ == kBlockSize) at_ = 0;
        }
    }

private:
    static constexpr double kDbEps = 1e-3, kLogEps = 1e-5;

    // A one-pole glide stepped per control tick that snaps to its target once within eps (idling needs exact
    // targets).
    struct Smooth {
        double y = 0.0;
        // false when already at t.
        bool Step(double t, double a, double eps) {
            if (y == t) return false;
            y += a * (t - y);
            if (std::fabs(t - y) < eps) y = t;
            return true;
        }
    };
    // One bell: its parameters' ranges, targets (dB, log2 Hz, log2 Q), their glides and the filter.
    struct Band {
        double db0 = 0.0, dbSpan = 0.0, dbZero = 0.0, lf0 = 0.0, lfSpan = 0.0, lq0 = 0.0, lqSpan = 0.0;
        double dbT = 0.0, lfT = 0.0, lqT = 0.0;
        Smooth db, lf, lq;
        bool on = false, dirty = false;  // dirty: a glide moved since the last design
        int idle = 0;                    // control ticks at 0 dB
        Bell f;
    };

    // Bell b's targets from v. A gain within one 14-bit code step of 0 dB is 0: 0 dB is not on the code grid (code
    // 8192 is +0.0007 dB), and only an exact 0 lets a bell stop and the flat EQ stay bit-exact.
    static void Targets(Band& s, int b, const float* v) {
        using namespace dkparam;
        const double db = s.db0 + double(v[kBell1Gain + 3 * b]) * s.dbSpan;
        s.dbT = std::fabs(db) < s.dbZero ? 0.0 : db;
        s.lfT = s.lf0 + double(v[kBell1Freq + 3 * b]) * s.lfSpan;
        s.lqT = s.lq0 + double(v[kBell1Q + 3 * b]) * s.lqSpan;
    }

    // Everything settled at v, all state zero, the bells not at 0 dB on with their coefficients, at a tick's start.
    [[gnu::noinline]] void Start(const float* v) {
        using namespace dkparam;
        low_.Init(true, v[kLowCutFreq], v[kLowCutRes], uint8_t(v[kLowCutSlope]));
        high_.Init(false, v[kHighCutFreq], v[kHighCutRes], uint8_t(v[kHighCutSlope]));
        for (int b = 0; b < kBells; ++b) {
            Band& s = band_[b];
            Targets(s, b, v);
            s.db.y = s.dbT;
            s.lf.y = s.lfT;
            s.lq.y = s.lqT;
            s.f.Clear();
            s.on = s.dbT != 0.0;
            s.idle = 0;
            s.dirty = false;
            if (s.on) s.f.Set(Design(s, s.db.y));
        }
        at_ = 0;
    }

    // Once per control tick: the enable, the cuts, the bells' glides, bells starting and stopping, coefficients of
    // what moved. noinline (with Start): inlined, the double-precision design made every sample save d8-d14.
    [[gnu::noinline]] DK_ITCM void Control(const float* v) {
        using namespace dkparam;
        enable_ = v[kEqEnable] > 0.5f;
        if (fade_ == 0) {
            if (!enable_) return;  // nothing runs
            Start(v);              // the stage restarts from zero state at the current settings
        }
        low_.Control(v[kLowCutFreq], v[kLowCutRes], uint8_t(v[kLowCutSlope]));
        high_.Control(v[kHighCutFreq], v[kHighCutRes], uint8_t(v[kHighCutSlope]));
        for (int b = 0; b < kBells; ++b) {
            Band& s = band_[b];
            Targets(s, b, v);
            bool moved = s.db.Step(s.dbT, a_, kDbEps);
            moved = s.lf.Step(s.lfT, a_, kLogEps) || moved;
            moved = s.lq.Step(s.lqT, a_, kLogEps) || moved;
            s.dirty = s.dirty || moved;
            if (!s.on) {
                if (s.dbT != 0.0) {
                    s.on = true;  // from zero state at its identity (0 dB), ramped below to the glide's first step
                    s.idle = 0;
                    s.f.Set(Design(s, 0.0));
                    s.dirty = true;
                }
            } else if (s.db.y == 0.0 && s.dbT == 0.0) {
                if (++s.idle >= kIdleTicks) {
                    s.on = false;
                    s.dirty = false;
                    s.f.Clear();
                }
            } else {
                s.idle = 0;
            }
            if (s.on && s.dirty) {
                s.f.RampTo(Design(s, s.db.y));
                s.dirty = false;
            } else {
                s.f.Settle();
            }
        }
    }

    // A bell's coefficients at gain db and its glides' current frequency and Q.
    static BellCoefs Design(const Band& s, double db) {
        return DesignBell(std::exp2(s.lf.y), std::exp2(s.lq.y), db, double(kSampleRate));
    }

    // m samples in place, at_ samples into the current tick.
    DK_ITCM void RunChunk(float* x, size_t m) {
        const int want = enable_ ? kFadeSamples : 0;
        if (fade_ == 0 && want == 0) return;  // true bypass
        const bool fading = fade_ != want;
        float dry[kBlockSize];
        if (fading)
            for (size_t i = 0; i < m; ++i) dry[i] = x[i];
        low_.Run(x, m, at_);
        for (Band& s : band_)
            if (s.on) s.f.Run(x, m, at_);
        high_.Run(x, m, at_);
        if (!fading) return;
        // Full fade: the wet sample as is (the blend's dry + (x - dry) can round); 0: dry + 0 is dry.
        for (size_t i = 0; i < m; ++i) {
            fade_ += fade_ < want ? 1 : fade_ > want ? -1 : 0;
            if (fade_ != kFadeSamples) x[i] = dry[i] + float(fade_) / float(kFadeSamples) * (x[i] - dry[i]);
        }
    }

    CutFilter low_, high_;
    Band band_[kBells];
    double a_ = 1.0;
    bool enable_ = true;
    int fade_ = 0;
    size_t at_ = 0;
};

}  // namespace dkdsp
