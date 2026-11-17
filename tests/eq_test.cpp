// daisy/src/dsp/eq.h (docs/EQ.md section 4): the model's laws and setting A's table, the digital stage against the
// model within the section 4 tolerance, cut/boost symmetry, bit-exact identity, click-free parameter jumps, output
// independent of the caller's block size, and robustness.
#include "dsp/eq.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "eq_model.h"

using namespace dkparam;
using namespace dktest;
using dkdsp::kBlockSize;

namespace {

constexpr double kFs = 48000.0;
// A +12 dB Q 8 bell at 40 Hz has a pole Q of 16: a 127 ms time constant; 2^17 samples are twenty of them.
constexpr size_t kIrSamples = size_t(1) << 17;
// docs/EQ.md section 4: the stage within this of the model, 20 Hz to 20 kHz.
constexpr double kTol = 0.05;
// Values printed to 2 decimals.
constexpr double kTol2 = 0.0051;

size_t Samples(double seconds) { return size_t(seconds * kFs); }

// A continuous parameter's position for x in its units (inverse of Units).
float Pos(uint8_t id, double x) {
    const ParamDef& d = kParams[id];
    if (d.taper == Taper::kLinear) return float((x - d.min) / (double(d.max) - d.min));
    return float(std::log(x / d.min) / std::log(double(d.max) / d.min));
}
// A slope (6, 12, 24 dB/oct) as the slope parameter's index.
float SlopeIndex(int slope) { return slope == 6 ? 2.0f : slope == 12 ? 1.0f : 0.0f; }
int SlopeOf(float index) { return index >= 2.0f ? 6 : index >= 1.0f ? 12 : 24; }

// The stage on its own, its parameters at the table's defaults until set.
struct Rig {
    float v[kParamCount];
    dkdsp::Eq eq;
    Rig() {
        for (int i = 0; i < kParamCount; ++i) v[i] = kParams[i].def;
    }
    void Apply(const EqSetting& s) {
        v[kEqEnable] = s.enabled ? 1.0f : 0.0f;
        v[kLowCutFreq] = s.lowOn ? Pos(kLowCutFreq, s.lowHz) : 0.0f;
        v[kLowCutRes] = Pos(kLowCutRes, s.lowQ);
        v[kLowCutSlope] = SlopeIndex(s.lowSlope);
        for (int b = 0; b < 3; ++b) {
            v[kBell1Gain + 3 * b] = Pos(uint8_t(kBell1Gain + 3 * b), s.bells[b].gain);
            v[kBell1Freq + 3 * b] = Pos(uint8_t(kBell1Freq + 3 * b), s.bells[b].hz);
            v[kBell1Q + 3 * b] = Pos(uint8_t(kBell1Q + 3 * b), s.bells[b].q);
        }
        v[kHighCutFreq] = s.highOn ? Pos(kHighCutFreq, s.highHz) : 1.0f;
        v[kHighCutRes] = Pos(kHighCutRes, s.highQ);
        v[kHighCutSlope] = SlopeIndex(s.highSlope);
    }
    void Init() { eq.Init(v); }
    // x[0, n) in place, in calls of `block` samples.
    void Run(float* x, size_t n, size_t block = kBlockSize) {
        for (size_t i = 0; i < n; i += block) eq.Process(x + i, std::min(block, n - i), v);
    }
};

// The model's setting for values v (a cut is on past its off end's bypass threshold).
EqSetting SettingOf(const float* v) {
    EqSetting s;
    s.enabled = v[kEqEnable] > 0.5f;
    s.lowOn = v[kLowCutFreq] > dkdsp::kBypassEnter;
    s.lowHz = Units(kParams[kLowCutFreq], v[kLowCutFreq]);
    s.lowQ = Units(kParams[kLowCutRes], v[kLowCutRes]);
    s.lowSlope = SlopeOf(v[kLowCutSlope]);
    for (int b = 0; b < 3; ++b) {
        s.bells[b].gain = Units(kParams[kBell1Gain + 3 * b], v[kBell1Gain + 3 * b]);
        s.bells[b].hz = Units(kParams[kBell1Freq + 3 * b], v[kBell1Freq + 3 * b]);
        s.bells[b].q = Units(kParams[kBell1Q + 3 * b], v[kBell1Q + 3 * b]);
    }
    s.highOn = v[kHighCutFreq] < 1.0f - dkdsp::kBypassEnter;
    s.highHz = Units(kParams[kHighCutFreq], v[kHighCutFreq]);
    s.highQ = Units(kParams[kHighCutRes], v[kHighCutRes]);
    s.highSlope = SlopeOf(v[kHighCutSlope]);
    return s;
}

std::vector<float> Noise(size_t n, float peak, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-peak, peak);
    std::vector<float> x(n);
    for (float& s : x) s = u(rng);
    return x;
}

std::vector<float> Sine(size_t n, double hz, float amp) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = amp * float(std::sin(2.0 * M_PI * hz * double(i) / kFs));
    return x;
}

// 61 points, 1/6 octave apart, 20 Hz to 20 kHz.
std::vector<double> Grid() {
    std::vector<double> f(61);
    for (int k = 0; k <= 60; ++k) f[size_t(k)] = 20.0 * std::pow(1000.0, k / 60.0);
    return f;
}

// The rig's magnitude (dB) at freqs: initialised (settled) at its values, the DFT of its impulse response.
std::vector<double> DigitalDb(Rig& r, const std::vector<double>& freqs) {
    r.Init();
    std::vector<float> y(kIrSamples, 0.0f);
    y[0] = 1.0f;
    r.Run(y.data(), y.size());
    std::vector<double> db;
    for (double f : freqs) {
        const Cplx rot = std::polar(1.0, -2.0 * M_PI * f / kFs);
        Cplx z = 1.0, acc = 0.0;
        for (float v : y) {
            acc += double(v) * z;
            z *= rot;
        }
        db.push_back(Db(acc));
    }
    return db;
}

// Setting A (docs/EQ.md section 3): low-cut 80 Hz 12 dB/oct Q 0.7071; bell 1 -4 dB at 250 Hz Q 1.4; bell 2 +3 dB at
// 1.2 kHz Q 0.7; bell 3 +5 dB at 3.5 kHz Q 2; high-cut 8 kHz 12 dB/oct Q 0.7071.
EqSetting SettingA() {
    EqSetting s;
    s.lowOn = true;
    s.lowHz = 80.0;
    s.lowSlope = 12;
    s.bells[0] = {-4.0, 250.0, 1.4};
    s.bells[1] = {3.0, 1200.0, 0.7};
    s.bells[2] = {5.0, 3500.0, 2.0};
    s.highOn = true;
    s.highHz = 8000.0;
    s.highSlope = 12;
    return s;
}

// The largest |y[i] - y[i - 1]| over [from, to).
double MaxStep(const std::vector<float>& y, size_t from, size_t to) {
    double m = 0.0;
    for (size_t i = std::max<size_t>(from, 1); i < to; ++i) m = std::max(m, double(std::fabs(y[i] - y[i - 1])));
    return m;
}

bool Same(const float* a, const float* b, size_t n) { return std::memcmp(a, b, n * sizeof(float)) == 0; }

}  // namespace

TEST(EqModel, LawsHold) {
    for (double g : {-12.0, -6.0, 6.0, 12.0}) {
        for (double f0 : {40.0, 1000.0, 12000.0}) {
            const EqBell up{g, f0, 1.4}, down{-g, f0, 1.4};
            EXPECT_NEAR(Db(BellModel(up, f0)), g, 1e-9) << g << " dB at " << f0;
            for (double f : {20.0, f0, 10000.0})
                EXPECT_NEAR(std::abs(BellModel(up, f) * BellModel(down, f) - 1.0), 0.0, 1e-9) << "mirror at " << f;
        }
    }
    for (bool hp : {true, false})
        EXPECT_NEAR(Db(CutModel(hp, 1000.0, kButterworthQ, 12, 1000.0)), -3.0103, 1e-4) << hp;
}

TEST(EqModel, SettingAMatchesTable) {
    const double f[] = {30, 50, 80, 120, 250, 500, 1000, 1200, 2000, 3500, 5000, 8000, 12000, 16000};
    const double a[] = {-17.15, -8.86, -3.23, -1.39, -3.77, 0.30, 2.79, 3.08, 2.62, 5.57, 1.35, -2.61, -9.88, -19.10};
    const EqSetting s = SettingA();
    for (size_t i = 0; i < std::size(f); ++i) EXPECT_NEAR(EqModelDb(s, f[i]), a[i], kTol2) << "A at " << f[i] << " Hz";
}

TEST(Eq, MatchesModelWithinTolerance) {
    const std::vector<double> freqs = Grid();
    double worst = 0.0;
    std::string worstAt;
    const auto check = [&](const EqSetting& s, const std::string& what) {
        Rig r;
        r.Apply(s);
        const std::vector<double> d = DigitalDb(r, freqs);
        for (size_t i = 0; i < freqs.size(); ++i) {
            const double m = EqModelDb(s, freqs[i]), e = std::fabs(d[i] - m);
            EXPECT_NEAR(d[i], m, kTol) << what << " at " << freqs[i] << " Hz";
            if (e > worst) {
                worst = e;
                worstAt = what + " at " + std::to_string(freqs[i]) + " Hz";
            }
        }
    };
    for (int b = 0; b < 3; ++b) {
        const ParamDef& fp = kParams[kBell1Freq + 3 * b];
        for (double f0 : {double(fp.min), double(Units(fp, fp.def)), double(fp.max)})
            for (double g : {-12.0, 12.0})
                for (double q : {0.4, 8.0}) {
                    EqSetting s;
                    s.bells[b] = {g, f0, q};
                    check(s, "bell " + std::to_string(b + 1) + " " + std::to_string(g) + " dB at " +
                                 std::to_string(f0) + " Hz Q " + std::to_string(q));
                }
    }
    for (int slope : {6, 12, 24})
        for (double q : {kButterworthQ, 2.0}) {
            EqSetting lo, hi;
            lo.lowOn = true;
            lo.lowHz = 100.0;
            lo.lowQ = q;
            lo.lowSlope = slope;
            hi.highOn = true;
            hi.highHz = 5000.0;
            hi.highQ = q;
            hi.highSlope = slope;
            const std::string tail = " " + std::to_string(slope) + " dB Q " + std::to_string(q);
            check(lo, "low-cut" + tail);
            check(hi, "high-cut" + tail);
        }
    check(SettingA(), "setting A");
    std::printf("[ worst    ] %.4f dB (%s)\n", worst, worstAt.c_str());
}

TEST(Eq, CutMirrorsBoost) {
    const std::vector<double> freqs = Grid();
    EqSetting up = SettingA(), down;
    up.lowOn = up.highOn = false;
    for (int b = 0; b < 3; ++b) down.bells[b] = {-up.bells[b].gain, up.bells[b].hz, up.bells[b].q};
    Rig ru, rd;
    ru.Apply(up);
    rd.Apply(down);
    const std::vector<double> du = DigitalDb(ru, freqs), dd = DigitalDb(rd, freqs);
    for (size_t i = 0; i < freqs.size(); ++i) EXPECT_LE(std::fabs(du[i] + dd[i]), 0.1) << "at " << freqs[i] << " Hz";
}

TEST(Eq, IdentityIsBitExact) {
    const std::vector<float> x = Noise(Samples(1.0), 0.5f, 3);
    {  // the defaults
        Rig r;
        r.Init();
        std::vector<float> y = x;
        r.Run(y.data(), y.size());
        EXPECT_TRUE(Same(y.data(), x.data(), x.size())) << "defaults";
    }
    {  // a bell's gain at code 8192, as the link delivers the middle
        Rig r;
        r.v[kBell2Gain] = FromCode(kParams[kBell2Gain], 8192);
        r.Init();
        std::vector<float> y = x;
        r.Run(y.data(), y.size());
        EXPECT_TRUE(Same(y.data(), x.data(), x.size())) << "0 dB by code";
    }
    {  // a bell taken to +6 dB, then back to the default after 150 ms
        Rig r;
        r.Init();
        std::vector<float> y = x;
        const size_t change = Samples(0.15);
        r.v[kBell1Gain] = Pos(kBell1Gain, 6.0);
        r.Run(y.data(), change);
        r.v[kBell1Gain] = kParams[kBell1Gain].def;
        r.Run(y.data() + change, y.size() - change);
        EXPECT_FALSE(Same(y.data(), x.data(), change)) << "the boost is heard";
        const size_t from = Samples(0.6);
        EXPECT_TRUE(Same(y.data() + from, x.data() + from, x.size() - from)) << "back at 0 dB";
    }
    {  // eq_enable off after 25 ms, with settings that colour it
        Rig r;
        r.Apply(SettingA());
        r.Init();
        std::vector<float> y = x;
        const size_t change = Samples(0.025);
        r.Run(y.data(), change);
        r.v[kEqEnable] = 0.0f;
        r.Run(y.data() + change, y.size() - change);
        const size_t from = change + size_t(dkdsp::Eq::kFadeSamples) + kBlockSize;
        EXPECT_TRUE(Same(y.data() + from, x.data() + from, x.size() - from)) << "eq_enable off";
    }
}

// A jump of each parameter between its ends does not click: over the 50 ms after it, the largest sample-to-sample
// step stays within 1.25 x the steady-state largest step, the larger of the 50 ms before, the 200-250 ms after and
// (the sine's level changes on the way: a bell's frequency sweeps through it) any setting along the jump's path,
// from the model.
TEST(Eq, NoClickOnParamJump) {
    constexpr double kHz = 220.0;
    constexpr float kAmp = 0.25f;
    // 0.301 s: a whole number of ticks, 66.2 cycles of the sine: the jump lands near a peak, not a zero crossing.
    const size_t settle = Samples(0.301), after = Samples(0.25), window = Samples(0.05);
    const std::vector<float> in = Sine(settle + after, kHz, kAmp);
    const double perUnit = kAmp * 2.0 * std::sin(M_PI * kHz / kFs);  // a unit-gain sine's largest step
    for (int id = 0; id < kParamCount; ++id) {
        const ParamDef& d = kParams[id];
        const float ends[2] = {0.0f, d.taper == Taper::kChoice ? d.max : 1.0f};
        for (int dir = 0; dir < 2; ++dir) {
            Rig r;
            for (int b = 0; b < 3; ++b) r.v[kBell1Gain + 3 * b] = Pos(uint8_t(kBell1Gain + 3 * b), 6.0);
            r.v[kLowCutFreq] = 0.3f;
            r.v[kHighCutFreq] = 0.5f;
            r.v[kLowCutSlope] = r.v[kHighCutSlope] = 1.0f;
            r.v[id] = ends[dir];
            const float from = r.v[id], to = ends[1 - dir];
            r.Init();
            std::vector<float> y = in;
            r.Run(y.data(), settle);
            r.v[id] = to;
            r.Run(y.data() + settle, after);

            double steady = std::max(MaxStep(y, settle - window, settle), MaxStep(y, settle + after - window, y.size()));
            if (d.taper != Taper::kChoice) {
                float path[kParamCount];
                std::copy(r.v, r.v + kParamCount, path);
                for (int k = 0; k <= 64; ++k) {
                    path[id] = from + (to - from) * float(k) / 64.0f;
                    steady = std::max(steady, perUnit * std::abs(EqModel(SettingOf(path), kHz)));
                }
            }
            EXPECT_LE(MaxStep(y, settle, settle + window), 1.25 * steady) << d.name << " " << from << " -> " << to;
        }
    }
}

TEST(Eq, BlockSizeIndependent) {
    const std::vector<float> x = Noise(Samples(2.0), 0.5f, 7);
    const size_t every = 480;
    std::vector<float> ref;
    for (size_t block : {size_t(1), size_t(6), size_t(8), size_t(24), size_t(48)}) {
        Rig r;
        r.Init();
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> unit(0.0f, 1.0f);
        std::vector<float> y = x;
        for (size_t i = 0; i < y.size(); i += every) {
            for (int id = 0; id < kParamCount; ++id) {
                const ParamDef& d = kParams[id];
                const float u = unit(rng);
                r.v[id] = d.taper == Taper::kChoice ? std::min(std::floor(u * (d.max + 1.0f)), d.max) : u;
            }
            r.Run(y.data() + i, std::min(every, y.size() - i), block);
        }
        if (ref.empty())
            ref = y;
        else
            EXPECT_TRUE(Same(y.data(), ref.data(), y.size())) << "block " << block;
    }
}

TEST(Eq, RandomChangesStayBounded) {
    const std::vector<float> x = Noise(Samples(4.0), 1.0f, 5);
    const size_t every = Samples(0.1);
    Rig r;
    r.Init();
    std::mt19937 rng(13);
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    std::vector<float> y = x;
    for (size_t i = 0; i < y.size(); i += every) {
        for (int id = 0; id < kParamCount; ++id) {
            const ParamDef& d = kParams[id];
            const float u = unit(rng);
            r.v[id] = d.taper == Taper::kChoice ? std::min(std::floor(u * (d.max + 1.0f)), d.max) : u;
        }
        r.Run(y.data() + i, std::min(every, y.size() - i));
    }
    size_t bad = 0;
    float peak = 0.0f;
    for (float v : y) {
        bad += !std::isfinite(v);
        peak = std::max(peak, std::fabs(v));
    }
    EXPECT_EQ(bad, 0u);
    EXPECT_LT(peak, 1000.0f);
}
