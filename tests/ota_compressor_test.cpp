// daisy/src/dsp/ota_compressor.h: the kernel against the reference implementation's fixtures (tools/ota_reference.py,
// docs/ota-compressor-spec.md appendix B), then against the spec's SPICE tables ("Agreement with SPICE"), then
// bypass, click-free setting and type switches, block-size independence and robustness.
#include "dsp/ota_compressor.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "dsp/compressor.h"
#include "test_files.h"
#include "wav.h"

using namespace dkparam;
using dkdsp::Compressor;
using dkdsp::kBlockSize;
using dkdsp::OtaCompressor;

namespace {

constexpr double kFs = 48000.0;
constexpr double kVfs = 1.8395;  // volts per unit sample, as the kernel's
constexpr double kF0 = 330.0;
const std::string kInDir = "tests/fixtures/comp/";  // the inputs are the OTC's (tools/comp_reference.py)
const std::string kDir = "tests/fixtures/ota/";

size_t Samples(double seconds) { return size_t(std::lround(seconds * kFs)); }

// The OTA compressor on its own, on, its parameters at the table's defaults until set.
struct Rig {
    float v[kParamCount];
    OtaCompressor c;
    Rig() {
        for (int i = 0; i < kParamCount; ++i) v[i] = kParams[i].def;
        v[kCompType] = kCompOta;
        v[kCompMode] = kCompBlend;
    }
    // Sustain's position, Level's index (0 -3 dB, 1 0 dB, 2 +3 dB), blend or direct, Balance's position.
    Rig& Set(float sustain, int level, bool direct, float balance) {
        v[kCompSustain] = sustain;
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

// Wet only (Mode direct, Balance 1): y is -wet / kVfs.
std::vector<float> Wet(float sustain, const std::vector<float>& x, int level = 1) {
    Rig r;
    r.Set(sustain, level, true, 1.0f).Init();
    return r.Run(x);
}
// Dry only (Mode blend, Balance 0): y is vq / kVfs.
std::vector<float> Dry(float sustain, const std::vector<float>& x, int level = 1) {
    Rig r;
    r.Set(sustain, level, false, 0.0f).Init();
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

double PeakToPeak(const std::vector<float>& y, size_t from, size_t to) {
    const auto [lo, hi] = std::minmax_element(y.begin() + long(from), y.begin() + long(to));
    return double(*hi) - double(*lo);
}
// y over x in dB, peak to peak, over 330 Hz cycle k (from 1) after start.
double PpGain(const std::vector<float>& y, const std::vector<float>& x, size_t start, long k) {
    const size_t from = start + size_t(std::lround(double(k - 1) * kFs / kF0));
    const size_t to = start + size_t(std::lround(double(k) * kFs / kF0));
    return 20.0 * std::log10(PeakToPeak(y, from, to) / PeakToPeak(x, from, to));
}
// The cycle that holds t seconds after the start.
long CycleAt(double t) { return long(std::floor(t * kF0)) + 1; }

// The wet output's amplitude in mV after 1.5 s of mv at 330 Hz: half its peak to peak over the last 436 samples, as
// tools/ota_reference.py's static.csv.
double StaticWetMv(float sustain, double mv) {
    const std::vector<float> y = Wet(sustain, Sine(mv, kF0, 72000));
    return 1000.0 * kVfs * PeakToPeak(y, y.size() - 436, y.size()) / 2.0;
}

// The kernel's float32 against the float64 reference (measured: 4.8e-7, 4.8e-7 and 1.2e-6 on the steps, 6.0e-7 on the
// DI, 0.00016 mV on static.csv).
constexpr float kRefLimit = 2e-5f;

TEST(OtaCompressor, MatchesReferenceSteps) {
    struct Case {
        const char* in;
        const char* ref;
        float sustain;
    };
    for (const Case& c : {Case{"step_30_300.in.wav", "step_30_300.s100.ref.wav", 1.0f},
                          Case{"step_30_300.in.wav", "step_30_300.s50.ref.wav", 0.5f},
                          Case{"step_10_1000.in.wav", "step_10_1000.s100.ref.wav", 1.0f}}) {
        const std::vector<float> x = ReadWav(RepoPath(kInDir + c.in));
        const std::vector<float> want = ReadWav(RepoPath(kDir + c.ref));
        ASSERT_EQ(x.size(), 48000u);
        const float e = MaxAbsDiff(Wet(c.sustain, x), want);
        std::printf("%s: max abs diff %.3g\n", c.ref, double(e));
        EXPECT_LE(e, kRefLimit) << c.ref;
    }
}

TEST(OtaCompressor, MatchesReferenceDi) {
    const std::vector<float> x = ReadWav(RepoPath(kInDir + "di.in.wav"));
    const std::vector<float> want = ReadWav(RepoPath(kDir + "di.ref.wav"));
    ASSERT_EQ(x.size(), 72000u);
    Rig r;
    r.Set(0.75f, 2, false, 0.75f).Init();
    const float e = MaxAbsDiff(r.Run(x), want);
    std::printf("di.ref.wav: max abs diff %.3g\n", double(e));
    EXPECT_LE(e, kRefLimit);
}

TEST(OtaCompressor, MatchesReferenceStatic) {
    std::ifstream f(RepoPath(kDir + "static.csv"));
    ASSERT_TRUE(f.good());
    std::string line;
    std::getline(f, line);
    EXPECT_EQ(line, "sustain_pct,in_mv,wet_mv");
    int rows = 0;
    double worst = 0.0;
    while (std::getline(f, line)) {
        int s = 0, mv = 0;
        double want = 0.0;
        ASSERT_EQ(std::sscanf(line.c_str(), "%d,%d,%lf", &s, &mv, &want), 3) << line;
        const double err = StaticWetMv(float(s) / 100.0f, mv) - want;  // mV
        worst = std::max(worst, std::fabs(err));
        EXPECT_NEAR(err, 0.0, 1000.0 * kVfs * kRefLimit) << "sustain " << s << " in " << mv << " mV";
        ++rows;
    }
    EXPECT_EQ(rows, 50);
    std::printf("static.csv: worst %.2g mV\n", worst);
}

constexpr float kStaticSustain[3] = {0.0f, 0.5f, 1.0f};
constexpr int kStaticMv[8] = {2, 5, 10, 20, 50, 100, 200, 500};
// docs/ota-compressor-spec.md "Static curve" (SPICE): wet mV peak, rows kStaticMv, columns kStaticSustain.
constexpr double kSpiceStatic[8][3] = {
    {8.15, 35.32, 149.40},    {20.37, 88.28, 363.11},   {40.74, 176.50, 420.87},  {81.44, 349.05, 438.69},
    {202.86, 426.82, 448.26}, {376.00, 441.05, 451.29}, {421.19, 446.65, 452.17}, {433.28, 445.36, 448.91},
};

TEST(OtaCompressor, StaticCurveMatchesSpice) {
    double worst = 0.0;
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 3; ++c) {
            const double err = 20.0 * std::log10(StaticWetMv(kStaticSustain[c], kStaticMv[r]) / kSpiceStatic[r][c]);
            worst = std::max(worst, std::fabs(err));
            EXPECT_NEAR(err, 0.0, 1.0) << "sustain " << kStaticSustain[c] << " in " << kStaticMv[r] << " mV";
        }
    std::printf("static curve against SPICE: worst %.3f dB\n", worst);
}

TEST(OtaCompressor, StepsMatchSpice) {
    // docs/ota-compressor-spec.md "Steps" (SPICE): 30 mV, 300 mV from 0.1 s to 0.5 s, then 30 mV. Within 1.5 dB in the
    // first 30 ms after each edge, 1.0 dB from there on; 50 % and 90 % of the reduction within 2 cycles.
    struct Case {
        float sustain;
        double cycles[5], hold[2], after[5];
        long reach[2];
    };
    const Case cases[2] = {
        {1.0f, {15.83, 11.25, 6.47, 3.99, 3.56}, {3.55, 3.55}, {6.65, 11.76, 15.82, 20.38, 23.39}, {2, 4}},
        {0.5f, {16.30, 16.62, 16.04, 13.41, 6.60}, {3.48, 3.47}, {5.09, 6.89, 8.70, 11.23, 14.67}, {5, 12}},
    };
    const long cycles[5] = {1, 2, 3, 5, 10};
    const double holdS[2] = {0.2, 0.4};
    const double afterS[5] = {0.51, 0.55, 0.6, 0.7, 0.9};
    const std::vector<float> x = ReadWav(RepoPath(kInDir + "step_30_300.in.wav"));
    const size_t up = Samples(0.1);
    for (const Case& c : cases) {
        const std::vector<float> y = Wet(c.sustain, x);
        const std::string name = "sustain " + std::to_string(c.sustain);
        for (int i = 0; i < 5; ++i)
            EXPECT_NEAR(PpGain(y, x, up, cycles[i]), c.cycles[i], 1.5) << name << ", cycle " << cycles[i];
        for (int i = 0; i < 2; ++i) EXPECT_NEAR(PpGain(y, x, 0, CycleAt(holdS[i])), c.hold[i], 1.0) << name;
        for (int i = 0; i < 5; ++i)
            EXPECT_NEAR(PpGain(y, x, 0, CycleAt(afterS[i])), c.after[i], afterS[i] < 0.53 ? 1.5 : 1.0)
                << name << ", " << afterS[i] << " s";
        const double before = PpGain(y, x, 0, CycleAt(0.1) - 1), final = PpGain(y, x, 0, CycleAt(0.4));
        for (int f = 0; f < 2; ++f) {
            const double target = before - (f == 0 ? 0.5 : 0.9) * (before - final);
            long k = 1;
            while (k < 100 && PpGain(y, x, up, k) > target) ++k;
            EXPECT_NEAR(double(k), double(c.reach[f]), 2.0) << name << (f == 0 ? ", 50 %" : ", 90 %");
        }
    }
}

TEST(OtaCompressor, BlendHalfIsMean) {
    const std::vector<float> x = Sine(300.0, kF0, Samples(0.5));
    Rig r;
    r.Set(1.0f, 1, false, float(std::log(41.0) / std::log(81.0))).Init();  // the taper's 0.5
    const std::vector<float> y = r.Run(x), wet = Wet(1.0f, x), dry = Dry(1.0f, x);
    float e = 0.0f;
    for (size_t i = 0; i < y.size(); ++i) e = std::max(e, std::fabs(y[i] - 0.5f * (wet[i] + dry[i])));
    EXPECT_LE(e, 2e-6f);
}

// No subnormal assertion: the path is linear in a subnormal input on the host, and the device flushes (FPSCR.FZ).
TEST(OtaCompressor, SilenceRecovers) {
    std::vector<float> x = Sine(300.0, kF0, Samples(22.0));
    std::fill(x.begin() + long(Samples(1.0)), x.end(), 0.0f);
    for (size_t i = Samples(21.0); i < x.size(); ++i) x[i] = i % 2 ? 1e-40f : -1e-40f;
    Rig r;
    r.Set(1.0f, 1, true, 1.0f).Init();
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

TEST(OtaCompressor, BypassIsBitExact) {
    const std::vector<float> x = Noise(Samples(0.5), 0.5f, 3);
    for (const bool otc : {false, true}) {
        Rig r;
        if (otc) {
            r.Set(1.0f, 1, true, 1.0f);
            r.v[kCompType] = kCompOtc;
        } else {
            r.v[kCompMode] = kCompOff;
        }
        r.Init();
        const std::string what = otc ? "comp_type OTC" : "comp_mode off";
        const std::vector<float> y = r.Run(x, 7);
        EXPECT_TRUE(Same(y.data(), x.data(), x.size())) << what << ", out of place";
        std::vector<float> z = x;
        r.Run(z.data(), z.data(), z.size(), 5);
        EXPECT_TRUE(Same(z.data(), x.data(), x.size())) << what << ", in place";
        EXPECT_EQ(r.c.gain(), 1.0f) << what;
    }
    {
        Rig r;
        r.Set(1.0f, 1, true, 1.0f).Init();
        const size_t change = Samples(0.1);
        std::vector<float> y(x.size());
        r.Run(x.data(), y.data(), change);
        r.v[kCompMode] = kCompOff;
        r.Run(x.data() + change, y.data() + change, x.size() - change);
        const size_t from = change + OtaCompressor::kFadeSamples + kBlockSize;
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
TEST(OtaCompressor, NoClickOnSettingJump) {
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
                r.Set(0.5f, 1, false, 0.5f);
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

// The pedal's order: the OTC, then the OTA on its output, in place; comp_type switched both ways. The output has no
// click, and once its fade is done the outgoing kernel is a bit-exact bypass with its meter at 1.
TEST(OtaCompressor, TypeSwitchHasNoClick) {
    const size_t settle = Samples(0.301), after = Samples(0.25), window = Samples(0.05);
    const std::vector<float> in = Sine(10.0, 220.0, settle + after);
    for (const uint8_t from : {kCompOtc, kCompOta}) {
        float v[kParamCount];
        for (int i = 0; i < kParamCount; ++i) v[i] = kParams[i].def;
        v[kCompMode] = kCompBlend;
        v[kCompSustain] = 0.5f;
        v[kCompType] = float(from);
        Compressor otc;
        OtaCompressor ota;
        otc.Init(v);
        ota.Init(v);
        std::vector<float> mid(in.size()), y(in.size());
        const auto run = [&](size_t at, size_t n) {
            for (size_t i = at; i < at + n; i += kBlockSize) {
                const size_t m = std::min(kBlockSize, at + n - i);
                otc.Process(in.data() + i, mid.data() + i, m, v);
                ota.Process(mid.data() + i, y.data() + i, m, v);
            }
        };
        run(0, settle);
        v[kCompType] = float(from == kCompOtc ? kCompOta : kCompOtc);
        run(settle, after);
        const std::string name = from == kCompOtc ? "OTC -> OTA" : "OTA -> OTC";
        const double steady =
            std::max(MaxStep(y, settle - window, settle), MaxStep(y, settle + after - window, y.size()));
        EXPECT_LE(MaxStep(y, settle, settle + window), 1.25 * steady) << name;
        const size_t done = settle + OtaCompressor::kFadeSamples + kBlockSize;
        if (from == kCompOtc) {
            EXPECT_TRUE(Same(mid.data() + done, in.data() + done, in.size() - done)) << name;
            EXPECT_EQ(otc.gain(), 1.0f) << name;
        } else {
            EXPECT_TRUE(Same(y.data() + done, mid.data() + done, in.size() - done)) << name;
            EXPECT_EQ(ota.gain(), 1.0f) << name;
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

TEST(OtaCompressor, BlockSizeIndependent) {
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

TEST(OtaCompressor, RandomChangesStayBounded) {
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
