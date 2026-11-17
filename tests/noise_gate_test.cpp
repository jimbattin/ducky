// daisy/src/dsp/noise_gate.h: the per-preset noise gate, fed block peaks directly.
#include "dsp/noise_gate.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using dkdsp::GateSettings;
using dkdsp::NoiseGate;

namespace {

constexpr GateSettings kOn{true, 60, 5, 20, 100, 40};
constexpr float kT = 1e-3f;  // threshold 60

float Db(float db) { return std::pow(10.0f, db / 20.0f); }

NoiseGate Gate(const GateSettings& s) {
    NoiseGate g;
    g.Set(s);
    return g;
}

// Feeds `n` blocks of `peak`, returning the last gain.
float Feed(NoiseGate& g, float peak, int n) {
    float y = 0.0f;
    for (int i = 0; i < n; ++i) y = g.Process(peak);
    return y;
}

}  // namespace

TEST(NoiseGate, Disabled) {
    NoiseGate g = Gate(dkdsp::kGateOff);
    for (int i = 0; i < 1000; ++i)
        for (float peak : {0.0f, 1e-6f, 0.5f}) ASSERT_EQ(g.Process(peak), 1.0f) << i;
}

TEST(NoiseGate, OpensInTheSameBlock) {
    NoiseGate g = Gate(kOn);
    EXPECT_EQ(Feed(g, 0.0f, 2000), 0.0f);
    EXPECT_EQ(g.Process(0.01f), 1.0f);
}

TEST(NoiseGate, HoldThenRelease) {
    NoiseGate g = Gate(kOn);
    Feed(g, 0.1f, 100);
    for (int i = 1; i <= 39; ++i) ASSERT_EQ(g.Process(0.0f), 1.0f) << i;
    EXPECT_LT(g.Process(0.0f), 1.0f);  // block 40
    float y = 0.0f;
    for (int i = 41; i <= 239; ++i) y = g.Process(0.0f);
    EXPECT_NEAR(y, 1e-3f, 2e-5f);  // 60 dB after 100 ms
    for (int i = 240; i <= 380; ++i) y = g.Process(0.0f);
    EXPECT_EQ(y, 0.0f);
}

TEST(NoiseGate, Hysteresis) {
    NoiseGate g = Gate(kOn);
    Feed(g, 0.0f, 2000);
    for (int i = 0; i < 200; ++i) ASSERT_EQ(g.Process(kT * Db(3.0f)), 0.0f) << i;
    EXPECT_EQ(g.Process(kT * Db(7.0f)), 1.0f);
    for (int i = 0; i < 200; ++i) ASSERT_EQ(g.Process(kT * Db(3.0f)), 1.0f) << i;
}

TEST(NoiseGate, ExpanderRatio) {
    for (auto [ratio, db] : {std::pair{uint8_t(40), -9.0f}, std::pair{uint8_t(20), -3.0f}}) {
        GateSettings s = kOn;
        s.ratio = ratio;
        NoiseGate g = Gate(s);
        Feed(g, 0.1f, 100);
        const float y = Feed(g, kT * Db(-3.0f), 2000);
        EXPECT_NEAR(20.0f * std::log10(y), db, 0.1f) << int(ratio);
    }
}

TEST(NoiseGate, SlowAttack) {
    GateSettings s = kOn;
    s.attack = 100;
    NoiseGate g = Gate(s);
    Feed(g, 0.0f, 2000);
    float prev = 0.0f;
    for (int i = 1; i <= 20; ++i) {
        const float y = g.Process(0.01f);
        ASSERT_NEAR(y - prev, 0.05f, 1e-4f) << i;
        prev = y;
    }
    EXPECT_GE(prev, 0.999f);
    EXPECT_EQ(g.Process(0.01f), 1.0f);
}

// A decaying low E with hum under the threshold: once the gate starts closing it never opens again (no sputter).
TEST(NoiseGate, TailNeverReopens) {
    constexpr double kFs = 48000.0;
    constexpr size_t kN = size_t(4 * kFs), kB = dkdsp::kBlockSize;
    std::vector<double> tone(kN), hum(kN);
    double tone_peak = 0.0, hum_peak = 0.0;
    for (size_t i = 0; i < kN; ++i) {
        const double t = double(i) / kFs;
        for (int k = 1; k <= 8; ++k) tone[i] += std::sin(2.0 * M_PI * 82.4 * k * t) / k;
        hum[i] = std::sin(2.0 * M_PI * 60.0 * t) + 0.4 * std::sin(2.0 * M_PI * 180.0 * t);
        tone_peak = std::fmax(tone_peak, std::fabs(tone[i]));
        hum_peak = std::fmax(hum_peak, std::fabs(hum[i]));
    }
    NoiseGate g = Gate(kOn);
    std::vector<float> gains;
    for (size_t b = 0; b + kB <= kN; b += kB) {
        float peak = 0.0f;
        for (size_t i = b; i < b + kB; ++i) {
            const double t = double(i) / kFs;
            const double x = tone[i] / tone_peak * std::pow(10.0, (-10.0 - 25.0 * t) / 20.0) +
                             hum[i] / hum_peak * std::pow(10.0, -76.0 / 20.0);
            peak = std::fmax(peak, float(std::fabs(x)));
        }
        gains.push_back(g.Process(peak));
    }
    size_t first = 0;
    while (first < gains.size() && gains[first] == 1.0f) ++first;
    ASSERT_LT(first, gains.size());
    for (size_t i = first + 1; i < gains.size(); ++i) ASSERT_LE(gains[i], gains[i - 1]) << i;
    // The hum, 16 dB under the threshold, holds the expander at (-16 dB)^3 = -48 dB.
    EXPECT_NEAR(20.0f * std::log10(gains.back()), -48.0f, 1.0f);
}
