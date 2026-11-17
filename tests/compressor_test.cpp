// daisy/src/dsp/compressor.h: the kernel against the reference implementation's fixtures (tools/comp_reference.py,
// docs/compressor-spec.md appendix B), then against the spec's SPICE targets ("Validation plan"), then bypass,
// click-free setting jumps, block-size independence and robustness.
#include "dsp/compressor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "test_files.h"
#include "wav.h"

using namespace dkparam;
using dkdsp::Compressor;
using dkdsp::kBlockSize;

namespace {

constexpr double kFs = 48000.0;
constexpr double kVfs = 1.8395;  // volts per unit sample, as the kernel's
constexpr double kF0 = 330.0;
const std::string kDir = "tests/fixtures/comp/";

size_t Samples(double seconds) { return size_t(std::lround(seconds * kFs)); }

// Sustain's position for pct % of the pot's 100 kOhm: the stage's gain 1 + 10 pct / 100, in dB, over its range.
float SustainPos(double pct) { return float(20.0 * std::log10(1.0 + pct / 10.0) / double(kCompSustainMaxDb)); }

// The compressor on its own, on, its parameters at the table's defaults until set.
struct Rig {
    float v[kParamCount];
    Compressor c;
    Rig() {
        for (int i = 0; i < kParamCount; ++i) v[i] = kParams[i].def;
        v[kCompMode] = kCompBlend;
    }
    // Sustain in %, Level's index (0 -3 dB, 1 0 dB, 2 +3 dB), blend or direct, Balance's position.
    Rig& Set(double sustainPct, int level, bool direct, float balance) {
        v[kCompSustain] = SustainPos(sustainPct);
        v[kCompLevel] = float(level);
        v[kCompMode] = direct ? kCompDirect : kCompBlend;
        v[kCompBalance] = balance;
        return *this;
    }
    void Init() { c.Init(v); }
    // in[0, n) to out, in calls of `block` samples (0: one call).
    void Run(const float* in, float* out, size_t n, size_t block = 0) {
        if (block == 0) block = n;
        for (size_t i = 0; i < n; i += block) c.Process(in + i, out + i, std::min(block, n - i), v);
    }
    std::vector<float> Run(const std::vector<float>& in, size_t block = 0) {
        std::vector<float> out(in.size());
        Run(in.data(), out.data(), in.size(), block);
        return out;
    }
};

// Wet only (Mode direct, Balance 1): y is -vo / kVfs.
std::vector<float> Wet(double sustainPct, const std::vector<float>& x, int level = 1) {
    Rig r;
    r.Set(sustainPct, level, true, 1.0f).Init();
    return r.Run(x);
}
// Dry only (Mode blend, Balance 0): y is -vd / kVfs.
std::vector<float> Dry(double sustainPct, const std::vector<float>& x, int level = 1) {
    Rig r;
    r.Set(sustainPct, level, false, 0.0f).Init();
    return r.Run(x);
}

std::vector<float> ReadWav(const std::string& path) {
    dkwav::Audio a;
    std::string err;
    EXPECT_TRUE(dkwav::Read(path, &a, &err)) << path << ": " << err;
    EXPECT_EQ(a.rate, 48000u) << path;
    return a.samples;
}

// Sample n of a sine of mv millivolts peak at the jack, as tools/comp_reference.py writes it.
float Synth(double mv, double hz, size_t n) {
    return float(mv / 1000.0 / kVfs * std::sin(2.0 * M_PI * hz * double(n) / kFs));
}
std::vector<float> Sine(double mv, double hz, size_t n) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = Synth(mv, hz, i);
    return x;
}
// 330 Hz, a0 for 0.5 s, a1 for 1 s, a0 for 2 s: the spec's step protocol.
std::vector<float> Steps(double a0, double a1) {
    std::vector<float> x(Samples(3.5));
    for (size_t i = 0; i < x.size(); ++i) x[i] = Synth(i >= Samples(0.5) && i < Samples(1.5) ? a1 : a0, kF0, i);
    return x;
}

std::vector<float> Noise(size_t n, float peak, uint32_t seed) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> u(-peak, peak);
    std::vector<float> x(n);
    for (float& s : x) s = u(rng);
    return x;
}

float MaxAbsDiff(const std::vector<float>& a, const std::vector<float>& b) {
    EXPECT_EQ(a.size(), b.size());
    float e = 0.0f;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) e = std::max(e, std::fabs(a[i] - b[i]));
    return e;
}

// The amplitude of hz in y over the largest whole number of its cycles in the last 0.1 s.
double Goertzel(const std::vector<float>& y, double hz) {
    const size_t n = size_t(std::lround(std::floor(0.1 * hz) * kFs / hz));
    double re = 0.0, im = 0.0;
    for (size_t i = y.size() - n; i < y.size(); ++i) {
        const double w = 2.0 * M_PI * hz * double(i) / kFs;
        re += y[i] * std::cos(w);
        im += y[i] * std::sin(w);
    }
    return 2.0 * std::hypot(re, im) / double(n);
}
double GainDb(const std::vector<float>& y, const std::vector<float>& x, double hz) {
    return 20.0 * std::log10(Goertzel(y, hz) / Goertzel(x, hz));
}

double PeakToPeak(const std::vector<float>& y, size_t from, size_t to) {
    const auto [lo, hi] = std::minmax_element(y.begin() + long(from), y.begin() + long(to));
    return double(*hi) - double(*lo);
}
// Wet over dry in dB, peak to peak, over 330 Hz cycle k (from 1) after start.
double PpGain(const std::vector<float>& wet, const std::vector<float>& dry, size_t start, long k) {
    const size_t from = start + size_t(std::lround(double(k - 1) * kFs / kF0));
    const size_t to = start + size_t(std::lround(double(k) * kFs / kF0));
    return 20.0 * std::log10(PeakToPeak(wet, from, to) / PeakToPeak(dry, from, to));
}
// The cycle that holds t seconds after the start.
long CycleAt(double t) { return long(std::floor(t * kF0)) + 1; }

constexpr int kStaticSustain[5] = {10, 25, 50, 75, 100};
constexpr int kStaticMv[9] = {10, 20, 50, 100, 200, 316, 501, 1000, 1585};
// docs/compressor-spec.md "Static curve" (SPICE): wet mV peak, rows kStaticMv, columns kStaticSustain.
constexpr double kSpiceStatic[9][5] = {
    {17, 30, 52, 74, 96},           {34, 60, 104, 148, 192},        {86, 152, 262, 372, 473},
    {171, 303, 502, 553, 569},      {342, 553, 612, 627, 634},      {533, 646, 676, 685, 689},
    {695, 745, 761, 767, 769},      {1007, 1021, 1026, 1027, 1028}, {1427, 1431, 1432, 1433, 1433},
};

// The wet output's amplitude in mV after 1.5 s of mv at 330 Hz: half its peak to peak over the last 436 samples, as
// tools/comp_reference.py's static.csv.
double StaticWetMv(int sustainPct, double mv) {
    const std::vector<float> y = Wet(sustainPct, Sine(mv, kF0, 72000));
    return 1000.0 * kVfs * PeakToPeak(y, y.size() - 436, y.size()) / 2.0;
}

TEST(Compressor, MatchesReferenceSteps) {
    struct Case {
        const char* in;
        const char* ref;
        double sustain;
    };
    for (const Case& c : {Case{"step_30_300.in.wav", "step_30_300.s100.ref.wav", 100.0},
                          Case{"step_30_300.in.wav", "step_30_300.s50.ref.wav", 50.0},
                          Case{"step_10_1000.in.wav", "step_10_1000.s100.ref.wav", 100.0}}) {
        const std::vector<float> x = ReadWav(RepoPath(kDir + c.in));
        const std::vector<float> want = ReadWav(RepoPath(kDir + c.ref));
        ASSERT_EQ(x.size(), 48000u);
        const float e = MaxAbsDiff(Wet(c.sustain, x), want);
        std::printf("%s: max abs diff %.3g\n", c.ref, double(e));
        EXPECT_LE(e, 1e-4f) << c.ref;
    }
}

TEST(Compressor, MatchesReferenceDi) {
    const std::vector<float> x = ReadWav(RepoPath(kDir + "di.in.wav"));
    const std::vector<float> want = ReadWav(RepoPath(kDir + "di.ref.wav"));
    ASSERT_EQ(x.size(), 72000u);
    Rig r;
    r.Set(100.0, 2, false, 0.75f).Init();
    const float e = MaxAbsDiff(r.Run(x), want);
    std::printf("di.ref.wav: max abs diff %.3g\n", double(e));
    EXPECT_LE(e, 1e-4f);
}

TEST(Compressor, MatchesReferenceStatic) {
    std::ifstream f(RepoPath(kDir + "static.csv"));
    ASSERT_TRUE(f.good());
    std::string line;
    std::getline(f, line);
    EXPECT_EQ(line, "sustain,in_mv,wet_mv");
    int rows = 0;
    double worst = 0.0;
    while (std::getline(f, line)) {
        int s = 0, mv = 0;
        double want = 0.0;
        ASSERT_EQ(std::sscanf(line.c_str(), "%d,%d,%lf", &s, &mv, &want), 3) << line;
        const double err = 20.0 * std::log10(StaticWetMv(s, mv) / want);
        worst = std::max(worst, std::fabs(err));
        EXPECT_NEAR(err, 0.0, 0.01) << "sustain " << s << " in " << mv << " mV";
        ++rows;
    }
    EXPECT_EQ(rows, 45);
    std::printf("static.csv: worst %.4f dB\n", worst);
}

TEST(Compressor, StaticCurveMatchesSpice) {
    double worst = 0.0;
    for (int r = 0; r < 9; ++r)
        for (int c = 0; c < 5; ++c) {
            const double err = 20.0 * std::log10(StaticWetMv(kStaticSustain[c], kStaticMv[r]) / kSpiceStatic[r][c]);
            worst = std::max(worst, std::fabs(err));
            EXPECT_NEAR(err, 0.0, 0.25) << "sustain " << kStaticSustain[c] << " in " << kStaticMv[r] << " mV";
        }
    std::printf("static curve against SPICE: worst %.3f dB\n", worst);
}

TEST(Compressor, GainBelowThreshold) {
    const double sustain[6] = {0, 10, 25, 50, 75, 100};
    const double want[6] = {-1.5, 4.7, 9.6, 14.4, 17.4, 19.7};  // "Static curve", second table
    const std::vector<float> x = Sine(10.0, kF0, Samples(0.5));
    for (int k = 0; k < 6; ++k) EXPECT_NEAR(GainDb(Wet(sustain[k], x), x, kF0), want[k], 0.1) << sustain[k] << " %";
}

TEST(Compressor, StageResponseMatchesTable) {
    // "Response from jack to output": rows kHz, columns Sustain (Rf) 100, 50, 25, 10, 2 and 0 %.
    const double hz[9] = {82, 120, 200, 400, 800, 1200, 2000, 3000, 4000};
    const double sustain[6] = {100, 50, 25, 10, 2, 0};
    const double table[9][6] = {
        {18.5, 13.2, 8.5, 3.6, -0.8, -2.4}, {19.0, 13.7, 9.0, 4.1, -0.3, -1.9}, {19.3, 14.1, 9.3, 4.4, 0.0, -1.7},
        {19.8, 14.5, 9.8, 4.8, 0.1, -1.5},  {21.0, 15.7, 10.7, 5.4, 0.4, -1.5}, {22.0, 16.7, 11.7, 6.1, 0.6, -1.5},
        {23.1, 18.1, 13.0, 7.1, 1.0, -1.5}, {23.3, 18.7, 13.7, 7.8, 1.3, -1.5}, {22.9, 18.8, 14.0, 8.1, 1.4, -1.5},
    };
    double worst = 0.0;
    for (int f = 0; f < 9; ++f) {
        const std::vector<float> x = Sine(10.0, hz[f], Samples(0.5));
        // The model omits the output coupling (20 Hz with the Balance pot's load).
        const double coupling = 20.0 * std::log10(hz[f] / std::sqrt(hz[f] * hz[f] + 400.0));
        for (int s = 0; s < 6; ++s) {
            const double err = GainDb(Wet(sustain[s], x), x, hz[f]) - (table[f][s] - coupling);
            worst = std::max(worst, std::fabs(err));
            EXPECT_NEAR(err, 0.0, 0.2) << hz[f] << " Hz, sustain " << sustain[s] << " %";
        }
    }
    std::printf("stage response against the table: worst %.3f dB\n", worst);
}

TEST(Compressor, StepsMatchSpice) {
    // "Attack and release", the step table (SPICE). The reference itself is up to 0.33 dB off from 30 ms on, so the
    // spec's +-0.2 dB there is +-0.4 here.
    struct Case {
        double sustain, a0, a1;
        double cycles[6], lowest, during[4], after[7], end;
    };
    const Case cases[3] = {
        {100, 30, 300, {19.4, 14.1, 8.2, 6.0, 5.1, 4.8}, 4.77, {5.0, 5.8, 7.2, 7.7},
         {7.8, 8.2, 9.0, 10.9, 15.4, 19.0, 20.0}, 20.2},
        {100, 10, 1000, {9.3, 8.3, 3.3, 1.7, 1.1, 0.8}, 0.53, {0.6, 0.6, 0.8, 0.8},
         {0.9, 1.2, 1.8, 3.4, 8.9, 16.5, 19.4}, 20.0},
        {50, 30, 300, {14.9, 14.7, 12.3, 9.3, 7.4, 6.5}, 5.87, {6.0, 6.5, 7.3, 7.5},
         {7.6, 7.9, 8.5, 9.8, 12.6, 14.4, 14.8}, 14.9},
    };
    const double duringS[4] = {0.05, 0.1, 0.2, 0.5};
    const double afterS[7] = {0.02, 0.05, 0.1, 0.2, 0.5, 1.0, 1.5};
    const size_t up = Samples(0.5), down = Samples(1.5);
    for (const Case& c : cases) {
        const std::vector<float> x = Steps(c.a0, c.a1);
        const std::vector<float> wet = Wet(c.sustain, x), dry = Dry(c.sustain, x);
        const std::string name = std::to_string(int(c.sustain)) + " %, " + std::to_string(int(c.a0)) + " -> " +
                                 std::to_string(int(c.a1)) + " mV";
        for (long k = 1; k <= 6; ++k)
            EXPECT_NEAR(PpGain(wet, dry, up, k), c.cycles[k - 1], 0.5) << name << ", cycle " << k;
        double lowest = 1e9;
        for (long k = 1; k <= 330; ++k) lowest = std::min(lowest, PpGain(wet, dry, up, k));
        EXPECT_NEAR(lowest, c.lowest, 0.4) << name << ", lowest";
        std::printf("%s: lowest %.2f dB (SPICE %.2f)\n", name.c_str(), lowest, c.lowest);
        for (int i = 0; i < 4; ++i)
            EXPECT_NEAR(PpGain(wet, dry, up, CycleAt(duringS[i])), c.during[i], 0.4) << name << ", " << duringS[i];
        for (int i = 0; i < 7; ++i)
            EXPECT_NEAR(PpGain(wet, dry, down, CycleAt(afterS[i])), c.after[i], 0.4)
                << name << ", " << afterS[i] << " s after the step down";
        EXPECT_NEAR(PpGain(wet, dry, down, 660), c.end, 0.4) << name << ", 2 s after the step down";
    }
}

TEST(Compressor, ClipLevels) {
    const std::vector<float> y = Wet(100.0, Steps(10.0, 1000.0));
    const auto [lo, hi] = std::minmax_element(y.begin(), y.end());
    // Wet volts are -y kVfs.
    EXPECT_NEAR(-double(*lo) * kVfs, 2.50, 0.02);
    EXPECT_NEAR(-double(*hi) * kVfs, -2.85, 0.02);
}

TEST(Compressor, SustainZeroIsFlat) {
    for (double hz : {400.0, 1000.0, 4000.0}) {
        const std::vector<float> x = Sine(10.0, hz, Samples(0.5));
        EXPECT_NEAR(GainDb(Wet(0.0, x), x, hz), -1.5387, 0.05) << hz << " Hz";
    }
}

TEST(Compressor, BlendHalfIsMean) {
    const std::vector<float> x = Sine(300.0, kF0, Samples(0.5));
    Rig r;
    r.Set(100.0, 1, false, float(std::log(41.0) / std::log(81.0))).Init();  // the taper's 0.5
    const std::vector<float> y = r.Run(x), wet = Wet(100.0, x), dry = Dry(100.0, x);
    float e = 0.0f;
    for (size_t i = 0; i < y.size(); ++i) e = std::max(e, std::fabs(y[i] - 0.5f * (wet[i] + dry[i])));
    EXPECT_LE(e, 2e-6f);
}

// No subnormal assertion: the path is linear in a subnormal input on the host, and the device flushes (FPSCR.FZ).
TEST(Compressor, SilenceRecovers) {
    std::vector<float> x = Sine(300.0, kF0, Samples(22.0));
    std::fill(x.begin() + long(Samples(1.0)), x.end(), 0.0f);
    for (size_t i = Samples(21.0); i < x.size(); ++i) x[i] = i % 2 ? 1e-40f : -1e-40f;
    Rig r;
    r.Set(100.0, 1, true, 1.0f).Init();
    const std::vector<float> y = r.Run(x);
    size_t bad = 0;
    float tail = 0.0f;
    for (size_t i = 0; i < y.size(); ++i) {
        bad += !std::isfinite(y[i]);
        if (i >= Samples(21.0)) tail = std::max(tail, std::fabs(y[i]));
    }
    EXPECT_EQ(bad, 0u);
    EXPECT_GE(r.c.gain(), 1.0f - 1e-4f);
    EXPECT_LE(tail, 1e-30f);
}

bool Same(const float* a, const float* b, size_t n) { return std::memcmp(a, b, n * sizeof(float)) == 0; }

TEST(Compressor, BypassIsBitExact) {
    const std::vector<float> x = Noise(Samples(0.5), 0.5f, 3);
    {
        Rig r;
        r.v[kCompMode] = kCompOff;
        r.Init();
        const std::vector<float> y = r.Run(x, 7);
        EXPECT_TRUE(Same(y.data(), x.data(), x.size())) << "out of place";
        std::vector<float> z = x;
        r.Run(z.data(), z.data(), z.size(), 5);
        EXPECT_TRUE(Same(z.data(), x.data(), x.size())) << "in place";
        EXPECT_EQ(r.c.gain(), 1.0f);
    }
    {
        Rig r;
        r.Set(100.0, 1, true, 1.0f).Init();
        const size_t change = Samples(0.1);
        std::vector<float> y(x.size());
        r.Run(x.data(), y.data(), change);
        r.v[kCompMode] = kCompOff;
        r.Run(x.data() + change, y.data() + change, x.size() - change);
        const size_t from = change + Compressor::kFadeSamples + kBlockSize;
        EXPECT_FALSE(Same(y.data(), x.data(), change)) << "on";
        EXPECT_TRUE(Same(y.data() + from, x.data() + from, x.size() - from)) << "comp_mode off";
    }
}

double MaxStep(const std::vector<float>& y, size_t from, size_t to) {
    double m = 0.0;
    for (size_t i = from + 1; i < to; ++i) m = std::max(m, std::fabs(double(y[i]) - double(y[i - 1])));
    return m;
}

// The criterion of Eq.NoClickOnParamJump: over the 50 ms after a jump of each setting between its ends (a choice:
// between any two of its values), the largest sample-to-sample step stays within 1.25 x the steady state's, the larger
// of the 50 ms before and the last 50 ms.
TEST(Compressor, NoClickOnSettingJump) {
    const size_t settle = Samples(0.301), after = Samples(0.25), window = Samples(0.05);
    const std::vector<float> in = Sine(10.0, 220.0, settle + after);
    for (int id = kCompMode; id <= kCompBalance; ++id) {
        const ParamDef& d = kParams[id];
        const bool choice = d.taper == Taper::kChoice;
        const int values = choice ? int(d.max) + 1 : 2;
        for (int from = 0; from < values; ++from) {
            for (int to = 0; to < values; ++to) {
                if (from == to) continue;
                Rig r;
                r.Set(50.0, 1, false, 0.5f);
                r.v[id] = float(from);
                r.Init();
                std::vector<float> y(in.size());
                r.Run(in.data(), y.data(), settle);
                r.v[id] = float(to);
                r.Run(in.data() + settle, y.data() + settle, after);
                const double steady =
                    std::max(MaxStep(y, settle - window, settle), MaxStep(y, settle + after - window, y.size()));
                EXPECT_LE(MaxStep(y, settle, settle + window), 1.25 * steady) << d.name << " " << from << " -> " << to;
            }
        }
    }
}

// The compressor's settings at random: a choice's index, a continuous position.
void Randomise(Rig& r, std::mt19937& rng) {
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);
    for (int id = kCompMode; id <= kCompBalance; ++id) {
        const ParamDef& d = kParams[id];
        const float u = unit(rng);
        r.v[id] = d.taper == Taper::kChoice ? std::min(std::floor(u * (d.max + 1.0f)), d.max) : u;
    }
}

TEST(Compressor, BlockSizeIndependent) {
    const std::vector<float> x = Noise(Samples(2.0), 0.5f, 7);
    const size_t every = 480;
    std::vector<float> ref;
    for (size_t block : {size_t(1), size_t(6), size_t(8), size_t(24), size_t(48)}) {
        Rig r;
        r.Init();
        std::mt19937 rng(11);
        std::vector<float> y(x.size());
        for (size_t i = 0; i < y.size(); i += every) {
            Randomise(r, rng);
            r.Run(x.data() + i, y.data() + i, std::min(every, y.size() - i), block);
        }
        if (ref.empty())
            ref = y;
        else
            EXPECT_TRUE(Same(y.data(), ref.data(), y.size())) << "block " << block;
    }
}

TEST(Compressor, RandomChangesStayBounded) {
    const std::vector<float> x = Noise(Samples(4.0), 1.0f, 5);
    const size_t every = Samples(0.1);
    Rig r;
    r.Init();
    std::mt19937 rng(13);
    std::vector<float> y(x.size());
    for (size_t i = 0; i < y.size(); i += every) {
        Randomise(r, rng);
        r.Run(x.data() + i, y.data() + i, std::min(every, y.size() - i));
    }
    size_t bad = 0;
    float peak = 0.0f;
    for (float v : y) {
        bad += !std::isfinite(v);
        peak = std::max(peak, std::fabs(v));
    }
    EXPECT_EQ(bad, 0u);
    EXPECT_LT(peak, 4.0f);
}

}  // namespace
