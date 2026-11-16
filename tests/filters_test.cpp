// daisy/src/dsp/filters.h: the knob and toggle maps, the cut and mid responses against their bilinear-exact
// prototypes, bit-exact neutral settings, stability under modulation, and click-free slope and bypass changes.
#include "dsp/filters.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <functional>
#include <vector>

using dkdsp::CutFilter;
using dkdsp::EdgeBypass;
using dkdsp::Eq;
using dkdsp::EqControls;
using dkdsp::kBlockSize;
using dkdsp::MidFilter;
using dkdsp::Slope;
using dkdsp::SlopeFromToggle;

namespace {

using cd = std::complex<double>;
constexpr double kFs = 48000.0;

// Toggle positions (controls.h).
constexpr uint8_t kDown = 0, kMiddle = 1, kUp = 2;

std::vector<float> Sine(double hz, size_t n) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = float(0.5 * std::sin(2.0 * M_PI * hz * double(i) / kFs));
    return x;
}

std::vector<float> Noise(size_t n) {
    std::vector<float> v(n);
    uint32_t s = 0x9E3779B9u;
    for (float& x : v) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        x = float(s) / 4294967296.0f - 0.5f;
    }
    return v;
}

// Runs `block(x, n, block_index)` in place over x in kBlockSize blocks.
void Blocks(std::vector<float>& x, const std::function<void(float*, size_t, size_t)>& block) {
    for (size_t i = 0; i < x.size(); i += kBlockSize) block(x.data() + i, std::min(kBlockSize, x.size() - i), i / kBlockSize);
}

// The gain at `hz` of an in-place block process: a 0.5 sine for 1 s, correlated over the last 0.5 s.
double MeasuredGain(double hz, const std::function<void(float*, size_t)>& process) {
    std::vector<float> y = Sine(hz, 48000);
    Blocks(y, [&](float* x, size_t n, size_t) { process(x, n); });
    double s = 0.0, c = 0.0;
    for (size_t i = 24000; i < y.size(); ++i) {
        const double ph = 2.0 * M_PI * hz * double(i) / kFs;
        s += y[i] * std::sin(ph);
        c += y[i] * std::cos(ph);
    }
    return 2.0 / 24000.0 * std::hypot(s, c) / 0.5;
}

// Bilinear-exact prototypes at W = tan(pi f / fs) / tan(pi fc / fs).
double Omega(double hz, double fc) { return std::tan(M_PI * hz / kFs) / std::tan(M_PI * fc / kFs); }
cd Order1(double w, bool hp) {
    const cd s(0.0, w);
    return (hp ? s : cd(1.0)) / (1.0 + s);
}
cd Order2(double w, double q, bool hp) {
    const cd den = cd(1.0 - w * w, w / q);
    return (hp ? cd(-w * w) : cd(1.0)) / den;
}
double Expected(Slope slope, bool hp, double hz, double fc, double q) {
    const double w = Omega(hz, fc);
    switch (slope) {
        case Slope::k6: return std::abs(Order1(w, hp));
        case Slope::k12: return std::abs(Order2(w, q, hp));
        case Slope::k24: return std::abs(Order2(w, dkdsp::kButterworthQ, hp) * Order2(w, q, hp));
    }
    return 0.0;
}

void ExpectGain(double measured, double expected, const std::string& what) {
    if (20.0 * std::log10(expected) > -40.0)
        EXPECT_NEAR(20.0 * std::log10(measured), 20.0 * std::log10(expected), 0.05) << what;
    else
        EXPECT_NEAR(measured, expected, 1e-3) << what;
}

uint8_t ToggleFor(Slope s) { return s == Slope::k6 ? kUp : s == Slope::k12 ? kMiddle : kDown; }

double MaxStep(const std::vector<float>& y, size_t from, size_t to) {
    double m = 0.0;
    for (size_t i = std::max<size_t>(from, 1); i < to; ++i) m = std::max(m, double(std::fabs(y[i] - y[i - 1])));
    return m;
}

}  // namespace

TEST(FilterMaps, Knobs) {
    EXPECT_NEAR(dkdsp::LowCutHz(0.0f), 20.0f, 1e-4f);
    EXPECT_NEAR(dkdsp::LowCutHz(1.0f), 500.0f, 1e-3f);
    EXPECT_NEAR(dkdsp::HighCutHz(0.0f), 1500.0f, 1e-3f);
    EXPECT_NEAR(dkdsp::HighCutHz(1.0f), 20000.0f, 1e-2f);
    EXPECT_NEAR(dkdsp::ResonanceQ(0.0f), 0.5f, 1e-6f);
    EXPECT_NEAR(dkdsp::ResonanceQ(0.5f), 1.0f, 1e-6f);
    EXPECT_NEAR(dkdsp::ResonanceQ(1.0f), 2.0f, 1e-6f);
}

TEST(FilterMaps, Toggles) {
    EXPECT_EQ(SlopeFromToggle(kUp), Slope::k6);
    EXPECT_EQ(SlopeFromToggle(kMiddle), Slope::k12);
    EXPECT_EQ(SlopeFromToggle(kDown), Slope::k24);
    EXPECT_EQ(dkdsp::MidGainDb(kUp), 5.0f);
    EXPECT_EQ(dkdsp::MidGainDb(kMiddle), 0.0f);
    EXPECT_EQ(dkdsp::MidGainDb(kDown), -7.0f);
}

TEST(FilterMaps, BypassHysteresis) {
    EdgeBypass low;
    low.Init(true, 0.5f);
    EXPECT_FALSE(low.Process(0.03f));  // active stays active
    EXPECT_TRUE(low.Process(0.02f));   // enters
    EXPECT_TRUE(low.Process(0.03f));   // bypassed stays bypassed
    EXPECT_TRUE(low.Process(0.04f));
    EXPECT_FALSE(low.Process(0.041f));  // leaves only above 0.04

    EdgeBypass high;
    high.Init(false, 0.5f);
    EXPECT_FALSE(high.Process(0.97f));
    EXPECT_TRUE(high.Process(0.98f));
    EXPECT_TRUE(high.Process(0.97f));
    EXPECT_FALSE(high.Process(0.959f));
}

TEST(CutFilterResponse, MatchesPrototype) {
    struct Case {
        bool hp;
        double fc;
        std::vector<double> hz;
    };
    const Case cases[] = {{true, 100.0, {30.0, 100.0, 200.0, 1000.0}}, {false, 5000.0, {1000.0, 5000.0, 8000.0, 16000.0}}};
    for (const Case& c : cases) {
        const float fk = c.hp ? float(std::log(c.fc / 20.0) / std::log(25.0))
                              : float(std::log(c.fc / 1500.0) / std::log(20000.0 / 1500.0));
        const double fc = c.hp ? dkdsp::LowCutHz(fk) : dkdsp::HighCutHz(fk);
        for (float rk : {0.25f, 1.0f}) {
            const double q = dkdsp::ResonanceQ(rk);
            for (Slope slope : {Slope::k6, Slope::k12, Slope::k24}) {
                for (double hz : c.hz) {
                    CutFilter f;
                    f.Init(c.hp, fk, rk, ToggleFor(slope));
                    const double got = MeasuredGain(hz, [&](float* x, size_t n) { f.Process(x, n, fk, rk, ToggleFor(slope)); });
                    ExpectGain(got, Expected(slope, c.hp, hz, fc, q),
                               (c.hp ? "low-cut " : "high-cut ") + std::to_string(int(slope)) + " q " +
                                   std::to_string(q) + " at " + std::to_string(hz));
                }
            }
        }
    }
}

TEST(MidFilterResponse, BoostAndScoop) {
    for (uint8_t pos : {kUp, kDown}) {
        MidFilter f;
        f.Init(pos);
        const double got = MeasuredGain(516.0, [&](float* x, size_t n) { f.Process(x, n, pos); });
        EXPECT_NEAR(20.0 * std::log10(got), pos == kUp ? 5.0 : -7.0, 0.05);
        // The whole bell, against (1 - W^2 + j W k A^2) / (1 - W^2 + j W k).
        for (double hz : {100.0, 300.0, 1000.0, 3000.0}) {
            MidFilter g;
            g.Init(pos);
            const double meas = MeasuredGain(hz, [&](float* x, size_t n) { g.Process(x, n, pos); });
            const double A = std::pow(10.0, dkdsp::MidGainDb(pos) / 40.0), k = 1.0 / (dkdsp::kMidQ * A);
            const double w = Omega(hz, dkdsp::kMidHz);
            ExpectGain(meas, std::abs(cd(1.0 - w * w, w * k * A * A) / cd(1.0 - w * w, w * k)), std::to_string(hz));
        }
    }
}

TEST(Eq, NeutralIsBitExact) {
    const EqControls c{0.0f, 0.5f, 1.0f, 0.5f, kMiddle, kMiddle, kMiddle};
    Eq eq;
    eq.Init(c);
    const std::vector<float> in = Noise(48000);
    std::vector<float> y = in;
    Blocks(y, [&](float* x, size_t n, size_t) { eq.Process(x, n, c); });
    for (size_t i = 0; i < y.size(); ++i) ASSERT_EQ(y[i], in[i]) << i;
}

TEST(Eq, StableUnderModulation) {
    for (uint8_t slope : {kDown, kMiddle, kUp}) {
        const size_t blocks = 4 * 48000 / kBlockSize;
        EqControls c{0.0f, 0.0f, 0.0f, 0.0f, slope, slope, kMiddle};
        Eq eq;
        eq.Init(c);
        std::vector<float> y = Noise(blocks * kBlockSize);
        double peak = 0.0;
        bool finite = true;
        Blocks(y, [&](float* x, size_t n, size_t b) {
            const float t = float(b) / float(blocks);
            const float tri = t < 0.5f ? 2.0f * t : 2.0f - 2.0f * t;
            c.low_cut = c.high_cut = tri;
            c.low_res = c.high_res = t;
            c.sw3 = uint8_t((b / 100) % 3);
            eq.Process(x, n, c);
            for (size_t i = 0; i < n; ++i) {
                finite = finite && std::isfinite(x[i]);
                peak = std::max(peak, double(std::fabs(x[i])));
            }
        });
        EXPECT_TRUE(finite) << int(slope);
        EXPECT_LT(peak, 8.0) << int(slope);
    }
}

TEST(CutFilterSwitching, SlopeChangeIsClickFree) {
    const size_t seg = 24000;  // 0.5 s, a whole number of blocks
    const std::vector<float> in = Sine(150.0, 3 * seg);
    const uint8_t toggles[3] = {kUp, kDown, kMiddle};  // 6 -> 24 -> 12 dB
    const float fk = 0.5f, rk = 1.0f;                // 100 Hz, Q 2

    CutFilter f;
    f.Init(true, fk, rk, toggles[0]);
    std::vector<float> y = in;
    Blocks(y, [&](float* x, size_t n, size_t b) { f.Process(x, n, fk, rk, toggles[b * kBlockSize / seg]); });

    double steady = 0.0;
    for (size_t s = 0; s < 3; ++s) steady = std::max(steady, MaxStep(y, s * seg + seg / 2, (s + 1) * seg));
    EXPECT_LE(MaxStep(y, 1, y.size()), 1.25 * steady);

    // Once the fade completes, the output is that of a filter that ran in the new slope all along.
    for (size_t s = 1; s < 3; ++s) {
        CutFilter ref;
        ref.Init(true, fk, rk, toggles[s]);
        std::vector<float> r = in;
        Blocks(r, [&](float* x, size_t n, size_t) { ref.Process(x, n, fk, rk, toggles[s]); });
        for (size_t i = s * seg + dkdsp::kFadeSamples; i < (s + 1) * seg; ++i) ASSERT_EQ(y[i], r[i]) << s << " " << i;
    }
}

TEST(CutFilterSwitching, BypassEngageIsClickFree) {
    const size_t seg = 24000;
    const std::vector<float> in = Sine(150.0, 2 * seg);
    const float rk = 1.0f;

    CutFilter f;
    f.Init(true, 0.1f, rk, kDown);
    std::vector<float> y = in;
    Blocks(y, [&](float* x, size_t n, size_t b) { f.Process(x, n, b * kBlockSize < seg ? 0.1f : 0.0f, rk, kDown); });

    const double steady = std::max(MaxStep(y, seg / 2, seg), MaxStep(y, seg + seg / 2, 2 * seg));
    EXPECT_LE(MaxStep(y, 1, y.size()), 1.25 * steady);
    for (size_t i = seg + seg / 2; i < y.size(); ++i) ASSERT_EQ(y[i], in[i]) << i;  // bypassed: dry, bit exact
}
