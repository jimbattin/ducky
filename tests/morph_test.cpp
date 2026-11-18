// daisy/src/morph.h: the morph block, the alignment transform against the fixture editor/src/morph/align.ts fitted
// (tests/fixtures/morph, tools/make_morph_fixture.ts), and the blended engine.
#include "morph.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "test_files.h"
#include "wav.h"

using dkmodel::Model;
using dkmorph::Block;
namespace a2 = dkdsp::a2;

namespace {

std::unique_ptr<Model> LoadModel(const std::string& rel) {
    const std::vector<uint8_t> b = ReadFile(RepoPath(rel));
    auto m = std::make_unique<Model>();
    const dkmodel::Status st = dkmodel::Parse(b.data(), b.size(), m.get());
    EXPECT_EQ(st, dkmodel::Status::kOk) << rel << ": " << dkmodel::StatusName(st);
    return st == dkmodel::Status::kOk ? std::move(m) : nullptr;
}

// A2_shuffled.bin is A2.bin's network in other coordinates; the block aligns it back.
std::vector<uint8_t> FixtureBlock() {
    std::vector<uint8_t> b = ReadFile(RepoPath("tests/fixtures/morph/A2_shuffled.morph"));
    EXPECT_EQ(b.size(), dkmorph::kBlockBytes);
    return b;
}

std::vector<float> ReadWav(const std::string& rel) {
    dkwav::Audio a;
    std::string err;
    EXPECT_TRUE(dkwav::Read(RepoPath(rel), &a, &err)) << rel << ": " << err;
    return a.samples;
}

bool Parses(const Block& b) {
    uint8_t raw[dkmorph::kBlockBytes];
    dkmorph::PutBlock(raw, b);
    Block out;
    return dkmorph::ParseBlock(raw, &out);
}

}  // namespace

TEST(MorphBlock, FixtureNamesTheShuffledModel) {
    const std::vector<uint8_t> raw = FixtureBlock();
    ASSERT_EQ(raw.size(), dkmorph::kBlockBytes);
    const std::unique_ptr<Model> shuffled = LoadModel("tests/fixtures/morph/A2_shuffled.bin");
    ASSERT_NE(shuffled, nullptr);
    Block b;
    ASSERT_TRUE(dkmorph::ParseBlock(raw.data(), &b));
    EXPECT_EQ(b.target, 0);
    EXPECT_EQ(b.model_crc, shuffled->crc);
    EXPECT_GE(b.fit, 0.9999f);
    EXPECT_LT(b.unaligned_fit, 0.5f) << "unaligned, the shuffled model does not blend with A2";

    uint8_t again[dkmorph::kBlockBytes];
    dkmorph::PutBlock(again, b);
    EXPECT_TRUE(std::equal(raw.begin(), raw.end(), again));
}

TEST(MorphBlock, RejectsWhatApplyCannotUse) {
    Block b;
    const std::vector<uint8_t> raw = FixtureBlock();
    ASSERT_TRUE(dkmorph::ParseBlock(raw.data(), &b));
    ASSERT_TRUE(Parses(b));

    Block bad = b;
    bad.target = 8;
    EXPECT_FALSE(Parses(bad));
    bad = b;
    bad.t.perm[1] = bad.t.perm[0];  // a channel used twice
    EXPECT_FALSE(Parses(bad));
    bad = b;
    bad.t.perm[2] = 3;
    EXPECT_FALSE(Parses(bad));
    bad = b;
    bad.t.scale[0] = 0.0f;
    EXPECT_FALSE(Parses(bad));
    bad = b;
    bad.t.scale[2] = 17.0f;
    EXPECT_FALSE(Parses(bad));
    bad = b;
    bad.t.scale[1] = std::nanf("");
    EXPECT_FALSE(Parses(bad));
    bad = b;
    for (int c = 0; c < 3; ++c) bad.t.m[3 + c] = 2.0f * bad.t.m[c];  // two parallel rows: singular
    EXPECT_FALSE(Parses(bad));
    bad = b;
    bad.unaligned_fit = INFINITY;
    EXPECT_FALSE(Parses(bad));
}

// The editor's fit applied by the pedal's code brings the shuffled model back to A2's own weights: the two languages
// agree on the permutation's direction and the matrix's orientation.
TEST(MorphApply, FixtureTransformRecoversA2) {
    const std::unique_ptr<Model> a2m = LoadModel("tests/fixtures/nam/A2.bin");
    const std::unique_ptr<Model> shuffled = LoadModel("tests/fixtures/morph/A2_shuffled.bin");
    ASSERT_NE(a2m, nullptr);
    ASSERT_NE(shuffled, nullptr);
    Block b;
    ASSERT_TRUE(dkmorph::ParseBlock(FixtureBlock().data(), &b));

    std::vector<float> want(a2::kWeightCount), got(a2::kWeightCount);
    dkmorph::Apply(dkmorph::kIdentity, a2m->weights, a2m->head_scale, want.data());
    dkmorph::Apply(b.t, shuffled->weights, shuffled->head_scale, got.data());
    float worst = 0.0f, differs = 0.0f;
    for (size_t i = 0; i < a2::kWeightCount; ++i) {
        worst = std::max(worst, std::fabs(got[i] - want[i]));
        differs = std::max(differs, std::fabs(shuffled->weights[i] - want[i]));
    }
    EXPECT_LE(worst, 1e-4f);
    EXPECT_GT(differs, 0.1f) << "the shuffled model's own weights are far from A2's";

    // The identity only folds the head scale in.
    for (size_t i = 0; i < a2::kHeadWOff; ++i) ASSERT_EQ(want[i], a2m->weights[i]) << i;
    EXPECT_EQ(want[a2::kHeadWOff], a2m->head_scale * a2m->weights[a2::kHeadWOff]);
}

TEST(MorphBlend, PositionReachesBothEnds) {
    EXPECT_EQ(dkmorph::BlendPosition(0.0f), 0.0f);
    EXPECT_EQ(dkmorph::BlendPosition(0.015f), 0.0f);
    EXPECT_NEAR(dkmorph::BlendPosition(0.26f), 0.25f, 1e-6f);
    EXPECT_NEAR(dkmorph::BlendPosition(0.5f), 0.5f, 1e-6f);
    EXPECT_EQ(dkmorph::BlendPosition(0.985f), 1.0f);
    EXPECT_EQ(dkmorph::BlendPosition(1.0f), 1.0f);
}

// A2 morphing into its shuffled self is A2 at every knob position, since the aligned target is A2 again: the blended
// engine renders the reference at 0, 1 and in between. Unaligned (the identity), the midpoint is another network.
TEST(MorphBlend, AlignedBlendRendersTheReference) {
    const std::unique_ptr<Model> a2m = LoadModel("tests/fixtures/nam/A2.bin");
    const std::unique_ptr<Model> shuffled = LoadModel("tests/fixtures/morph/A2_shuffled.bin");
    ASSERT_NE(a2m, nullptr);
    ASSERT_NE(shuffled, nullptr);
    Block b;
    ASSERT_TRUE(dkmorph::ParseBlock(FixtureBlock().data(), &b));
    const std::vector<float> in = ReadWav("tests/fixtures/nam/input.wav");
    const std::vector<float> want = ReadWav("tests/fixtures/nam/A2.input.ref.wav");
    ASSERT_EQ(in.size(), want.size());

    std::vector<float> scratch(2 * a2::kWeightCount);
    auto w = std::make_unique<dkmorph::Weights>();
    auto engine = std::make_unique<dkdsp::A2Lite>();
    auto render = [&](const dkmorph::Transform& t, float knob) {
        dkmorph::Prepare(t, *a2m, *shuffled, scratch.data(), w.get());
        engine->Load(a2m->weights, a2m->head_scale);
        engine->Blend(w->a, w->d, knob);
        std::vector<float> out(in.size());
        for (size_t i = 0; i < in.size(); i += dkdsp::kBlockSize)
            engine->Process(in.data() + i, out.data() + i, std::min(dkdsp::kBlockSize, in.size() - i));
        float worst = 0.0f;
        for (size_t i = 0; i < in.size(); ++i) worst = std::max(worst, std::fabs(out[i] - want[i]));
        return worst;
    };
    EXPECT_LE(render(b.t, 0.0f), 1e-5f);
    EXPECT_LE(render(b.t, 0.37f), 1e-3f);
    EXPECT_LE(render(b.t, 1.0f), 1e-3f);
    EXPECT_GT(render(dkmorph::kIdentity, 0.5f), 1e-2f);
}
