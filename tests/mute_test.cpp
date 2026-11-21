// daisy/src/dsp/mute.h: FS1's mute ramp.
#include "dsp/mute.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

using dkdsp::MuteRamp;

namespace {

constexpr float kStep = 1.0f / MuteRamp::kRampSamples;

std::vector<float> Sine(size_t n) {
    std::vector<float> x(n);
    for (size_t i = 0; i < n; ++i) x[i] = 0.5f * std::sin(2.0 * M_PI * 1000.0 * double(i) / 48000.0);
    return x;
}

}  // namespace

TEST(MuteRamp, UnmutedIsBitExact) {
    MuteRamp m;
    m.Init(false);
    const std::vector<float> in = Sine(480);
    std::vector<float> out(in.size());
    m.Process(in.data(), out.data(), in.size());
    for (size_t i = 0; i < in.size(); ++i) ASSERT_EQ(out[i], in[i]) << i;
}

TEST(MuteRamp, LinearFallLandsOnZero) {
    MuteRamp m;
    m.Init(false);
    m.SetMuted(true);
    const std::vector<float> in(480, 1.0f);
    std::vector<float> out(in.size());
    m.Process(in.data(), out.data(), in.size());
    for (int i = 0; i < MuteRamp::kRampSamples; ++i) EXPECT_NEAR(out[i], 1.0f - float(i + 1) * kStep, 1e-6f) << i;
    for (size_t i = MuteRamp::kRampSamples; i < out.size(); ++i) ASSERT_EQ(out[i], 0.0f) << i;
    float prev = 1.0f;
    for (float y : out) {
        EXPECT_LE(std::fabs(y - prev), kStep + 1e-6f);
        prev = y;
    }
}

TEST(MuteRamp, ReversalMidRampHasNoJump) {
    MuteRamp m;
    m.Init(false);
    m.SetMuted(true);
    const std::vector<float> in(600, 1.0f);
    std::vector<float> out(in.size());
    m.Process(in.data(), out.data(), 100);
    m.SetMuted(false);
    m.Process(in.data() + 100, out.data() + 100, in.size() - 100);
    EXPECT_NEAR(out[100], out[99] + kStep, 1e-6f);
    float prev = 1.0f;
    for (float y : out) {
        EXPECT_LE(std::fabs(y - prev), kStep + 1e-6f);
        prev = y;
    }
    EXPECT_EQ(out.back(), 1.0f);
}

TEST(MuteRamp, BlockSizeIndependent) {
    constexpr size_t kN = 2016, kMuteAt = 336, kUnmuteAt = 1008;  // multiples of 1, 7 and 48
    const std::vector<float> in = Sine(kN);
    std::vector<float> ref;
    for (size_t block : {size_t(1), size_t(7), size_t(48)}) {
        MuteRamp m;
        m.Init(false);
        std::vector<float> out(kN);
        for (size_t i = 0; i < kN; i += block) {
            if (i == kMuteAt) m.SetMuted(true);
            if (i == kUnmuteAt) m.SetMuted(false);
            m.Process(in.data() + i, out.data() + i, block);
        }
        if (ref.empty())
            ref = out;
        else
            for (size_t i = 0; i < kN; ++i) ASSERT_EQ(out[i], ref[i]) << "block " << block << " sample " << i;
    }
}

TEST(MuteRamp, PairMatchesOneRampOnEach) {
    MuteRamp m;
    m.Init(false);
    m.SetMuted(true);
    const std::vector<float> ones(300, 1.0f);
    std::vector<float> scratch(ones.size());
    for (size_t i = 0; i < ones.size(); i += 7)
        m.Process(ones.data() + i, scratch.data() + i, std::min<size_t>(7, ones.size() - i));
    m.SetMuted(false);
    MuteRamp m1 = m, m2 = m, pair = m;
    const std::vector<float> a = Sine(480);
    std::vector<float> b(a.size());
    for (size_t i = 0; i < b.size(); ++i) b[i] = -0.25f * a[a.size() - 1 - i];
    std::vector<float> a1(a.size()), b1(b.size()), a2 = a, b2 = b;
    m1.Process(a.data(), a1.data(), a.size());
    m2.Process(b.data(), b1.data(), b.size());
    pair.ProcessPair(a2.data(), b2.data(), a.size());
    for (size_t i = 0; i < a.size(); ++i) {
        ASSERT_EQ(a2[i], a1[i]) << i;
        ASSERT_EQ(b2[i], b1[i]) << i;
    }
    EXPECT_EQ(a1.back(), a.back());
}
