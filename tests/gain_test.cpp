// daisy/src/dsp/gain.h: the knob gain laws, the level match, Smoother and GainRamp.
#include "dsp/gain.h"

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

using dkdsp::DbToGain;
using dkdsp::GainRamp;
using dkdsp::InputCalibrationGain;
using dkdsp::InputGain;
using dkdsp::LevelMatchGain;
using dkdsp::OutputGain;
using dkdsp::Smoother;

namespace {

void ExpectRel(float actual, float expected, float rel) { EXPECT_NEAR(actual, expected, rel * std::fabs(expected)); }

std::vector<float> Sine(size_t n) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = 0.5f * std::sin(2.0 * M_PI * 1000.0 * double(i) / 48000.0);
    return x;
}

}  // namespace

TEST(GainLaws, InputTrim) {
    EXPECT_EQ(InputGain(0.5f), 1.0f);
    ExpectRel(InputGain(0.0f), DbToGain(-12.0f), 1e-4f);
    ExpectRel(InputGain(1.0f), DbToGain(12.0f), 1e-4f);
    ExpectRel(DbToGain(-12.0f), 0.251189f, 1e-4f);
    ExpectRel(DbToGain(12.0f), 3.98107f, 1e-4f);
}

TEST(GainLaws, OutputVolume) {
    EXPECT_EQ(OutputGain(0.0f), 0.0f);
    EXPECT_EQ(OutputGain(0.5f), 1.0f);
    EXPECT_EQ(OutputGain(0.25f), 0.125f);
    ExpectRel(OutputGain(1.0f), 3.98107f, 1e-4f);
    float prev = OutputGain(0.0f);
    for (int i = 1; i <= 1000; ++i) {
        const float g = OutputGain(float(i) / 1000.0f);
        ASSERT_GE(g, prev) << i;
        prev = g;
    }
    EXPECT_LT(std::fabs(OutputGain(0.5001f) - OutputGain(0.4999f)), 1e-3f);
}

TEST(GainLaws, LevelMatch) {
    EXPECT_EQ(LevelMatchGain(false, -24.0f), 1.0f);
    ExpectRel(LevelMatchGain(true, -24.0f), DbToGain(6.0f), 1e-4f);
    ExpectRel(LevelMatchGain(true, -24.0f), 1.99526f, 1e-4f);
    EXPECT_EQ(LevelMatchGain(true, -18.0f), 1.0f);
}

TEST(GainLaws, InputCalibration) {
    EXPECT_EQ(InputCalibrationGain(true, dkdsp::kInputLevelDbu), 1.0f);
    ExpectRel(InputCalibrationGain(true, 15.0f), DbToGain(-10.5f), 1e-4f);
    ExpectRel(InputCalibrationGain(true, 15.0f), 0.298538f, 1e-4f);
    ExpectRel(InputCalibrationGain(false, 0.0f), DbToGain(-7.5f), 1e-4f);
}

TEST(GainRamp, LandsOnTargetLinearly) {
    GainRamp r;
    r.Init(1.0f);
    const std::vector<float> in(48, 1.0f);
    std::vector<float> out(in.size());
    r.Process(in.data(), out.data(), in.size(), 2.0f);
    EXPECT_EQ(out[47], 2.0f);
    EXPECT_NEAR(out[0], 1.0f + 1.0f / 48.0f, 1e-6f);
    for (size_t i = 1; i < out.size(); ++i) EXPECT_NEAR(out[i] - out[i - 1], 1.0f / 48.0f, 1e-6f) << i;
    r.Process(in.data(), out.data(), in.size(), 2.0f);
    for (size_t i = 0; i < out.size(); ++i) ASSERT_EQ(out[i], 2.0f) << i;
}

TEST(GainRamp, UnityIsBitExactAndAliases) {
    GainRamp r;
    r.Init(1.0f);
    const std::vector<float> in = Sine(480);
    std::vector<float> buf = in;
    for (size_t i = 0; i < buf.size(); i += 48) r.Process(buf.data() + i, buf.data() + i, 48, 1.0f);
    for (size_t i = 0; i < in.size(); ++i) ASSERT_EQ(buf[i], in[i]) << i;
}

TEST(Smoother, OnePoleStep) {
    Smoother s;
    s.Init(1000.0f, 0.02f, 0.0f);
    float prev = 0.0f;
    for (int i = 1; i <= 200; ++i) {
        const float y = s.Process(1.0f);
        ASSERT_GE(y, prev) << i;
        ASSERT_LE(y, 1.0f) << i;
        if (i == 20) {
            EXPECT_NEAR(y, 1.0f - std::exp(-1.0f), 2e-3f);
        }
        prev = y;
    }
    EXPECT_GT(prev, 0.9999f);
}
