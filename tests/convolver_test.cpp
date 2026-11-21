// daisy/src/dsp/convolver.h: the partitioned FFT IR convolver against direct convolution.
#include "dsp/convolver.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "ir.h"
#include "test_files.h"

using dkdsp::IrConvolver;
using dkdsp::kBlockSize;
using dkdsp::kIrTailParts;
using dkdsp::kIrTailPhases;
using dkdsp::kIrTaps;

namespace {

std::unique_ptr<dkir::Ir> DecayIr() {
    const std::vector<uint8_t> b = ReadFile(RepoPath("tests/fixtures/ir/decay.bin"));
    auto ir = std::make_unique<dkir::Ir>();
    EXPECT_EQ(dkir::Parse(b.data(), b.size(), ir.get()), dkir::Status::kOk);
    return ir;
}

// A convolver with its tail pool.
struct Rig {
    std::vector<float> pool = std::vector<float>(dkdsp::kIrTailPoolFloats);
    std::unique_ptr<IrConvolver> c = std::make_unique<IrConvolver>();
    bool Init() { return c->Init(pool.data()); }
};

std::vector<float> Noise(size_t n) {
    std::vector<float> v(n);
    uint32_t s = 0x2545F491u;
    for (float& x : v) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        x = float(s) / 4294967296.0f - 0.5f;
    }
    return v;
}

std::vector<float> Convolve(IrConvolver& c, const std::vector<float>& in) {
    std::vector<float> out(in.size());
    for (size_t i = 0; i < in.size(); i += kBlockSize) c.Process(in.data() + i, out.data() + i);
    return out;
}

// An impulse into c gives the taps, then silence.
void ExpectImpulseGivesTheTaps(IrConvolver& c, const dkir::Ir& ir) {
    std::vector<float> in((kIrTaps / kBlockSize + 2) * kBlockSize, 0.0f);  // the taps and at least 16 more samples
    in[0] = 1.0f;
    const std::vector<float> out = Convolve(c, in);
    for (size_t i = 0; i < kIrTaps; ++i) ASSERT_NEAR(out[i], ir.taps[i], 1e-6) << i;
    for (size_t i = kIrTaps; i < kIrTaps + 16; ++i) ASSERT_NEAR(out[i], 0.0f, 1e-6) << i;
}

}  // namespace

TEST(IrConvolver, ImpulseGivesTheTaps) {
    const auto ir = DecayIr();
    Rig r;
    ASSERT_TRUE(r.Init());
    r.c->Load(ir->taps);
    ExpectImpulseGivesTheTaps(*r.c, *ir);
}

// A load while the tail is part-way through a frame (a preset switch) restarts the frame and clears the tail's ring.
TEST(IrConvolver, ReloadMidFrameGivesTheTaps) {
    const auto ir = DecayIr();
    Rig r;
    ASSERT_TRUE(r.Init());
    r.c->Load(ir->taps);
    Convolve(*r.c, Noise(21 * kBlockSize));
    r.c->Load(ir->taps);
    ExpectImpulseGivesTheTaps(*r.c, *ir);
}

TEST(IrConvolver, NoiseMatchesDirectConvolution) {
    const auto ir = DecayIr();
    Rig r;
    ASSERT_TRUE(r.Init());
    r.c->Load(ir->taps);
    // Past two turns of the tail's spectrum ring.
    const std::vector<float> in = Noise((2 * kIrTailParts * kIrTailPhases + 14) * kBlockSize);
    const std::vector<float> out = Convolve(*r.c, in);
    double max_err = 0.0;
    for (size_t n = 0; n < in.size(); ++n) {
        double want = 0.0;
        for (size_t k = 0; k <= std::min(n, kIrTaps - 1); ++k) want += double(ir->taps[k]) * in[n - k];
        max_err = std::max(max_err, std::fabs(out[n] - want));
    }
    EXPECT_LE(max_err, 1e-5);
}

TEST(IrConvolver, InPlaceMatchesSeparateBuffers) {
    const auto ir = DecayIr();
    Rig a, b;
    ASSERT_TRUE(a.Init());
    ASSERT_TRUE(b.Init());
    a.c->Load(ir->taps);
    b.c->Load(ir->taps);
    const std::vector<float> in = Noise(50 * kBlockSize);
    const std::vector<float> separate = Convolve(*a.c, in);
    std::vector<float> inplace = in;
    for (size_t i = 0; i < inplace.size(); i += kBlockSize) b.c->Process(inplace.data() + i, inplace.data() + i);
    EXPECT_EQ(inplace, separate);
}

TEST(IrConvolver, ResetSilences) {
    const auto ir = DecayIr();
    Rig r;
    ASSERT_TRUE(r.Init());
    r.c->Load(ir->taps);
    Convolve(*r.c, Noise(20 * kBlockSize));
    r.c->Reset();
    const std::vector<float> out = Convolve(*r.c, std::vector<float>(50 * kBlockSize, 0.0f));
    for (size_t i = 0; i < out.size(); ++i) ASSERT_EQ(out[i], 0.0f) << i;
}
