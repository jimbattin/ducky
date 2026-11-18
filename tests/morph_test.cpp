// daisy/src/morph.h: the morph block, the alignment transform against the fixture editor/src/morph/align.ts fitted,
// the blended engine, and a path's segments and waypoints, the editor's path preset among them (tests/fixtures/morph,
// tools/make_morph_fixture.ts).
#include "morph.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#include "preset.h"
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

    std::vector<float> scratch(a2::kWeightCount);
    auto w = std::make_unique<dkmorph::Weights>();
    auto engine = std::make_unique<dkdsp::A2Lite>();
    auto render = [&](const dkmorph::Transform& t, float knob) {
        dkmorph::Prepare(t, *a2m, *shuffled, nullptr, 0, scratch.data(), w.get());
        EXPECT_EQ(w->segments, 1);
        engine->Load(a2m->weights, a2m->head_scale);
        engine->Blend(w->a[0], w->d[0], knob);
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

TEST(MorphPath, SegmentAt) {
    auto expect = [](float t, int segments, int s, float f) {
        const dkmorph::Segment g = dkmorph::SegmentAt(t, segments);
        EXPECT_EQ(g.s, s) << "t " << t << " of " << segments;
        EXPECT_NEAR(g.f, f, 1e-6f) << "t " << t << " of " << segments;
    };
    // A straight blend: the position itself.
    expect(0.0f, 1, 0, 0.0f);
    expect(0.37f, 1, 0, 0.37f);
    expect(1.0f, 1, 0, 1.0f);
    // One waypoint.
    expect(0.25f, 2, 0, 0.5f);
    expect(0.5f, 2, 1, 0.0f);
    expect(1.0f, 2, 1, 1.0f);
    // Three: a knot starts the later segment, and 1 ends the last.
    expect(0.0f, 4, 0, 0.0f);
    expect(0.125f, 4, 0, 0.5f);
    expect(0.25f, 4, 1, 0.0f);
    expect(0.3f, 4, 1, 0.2f);
    expect(0.5f, 4, 2, 0.0f);
    expect(0.6875f, 4, 2, 0.75f);
    expect(0.75f, 4, 3, 0.0f);
    expect(0.99f, 4, 3, 0.96f);
    expect(1.0f, 4, 3, 1.0f);
    EXPECT_EQ(dkmorph::SegmentAt(0.25f, 4).f, 0.0f) << "a knot is exactly its node";
    EXPECT_EQ(dkmorph::SegmentAt(1.0f, 4).f, 1.0f);
}

namespace {

// A2 and its shuffled self, unaligned: two far-apart points in weight space (the same network at both, another one
// between), with what renders a path between them as the pedal's morph step does. The renders start kSettle samples
// before the ones compared: a blend keeps the histories of the model loaded before it, which a network in other
// coordinates takes its receptive field to replace.
constexpr size_t kSettle = 7200, kCompared = 9600, kFrom = 48000 - kSettle;  // the input's first second is silence
static_assert(kSettle > size_t(a2::kReceptiveField) && kSettle % dkdsp::kBlockSize == 0, "the histories turn over");

struct PathRig {
    std::unique_ptr<Model> a = LoadModel("tests/fixtures/nam/A2.bin");
    std::unique_ptr<Model> b = LoadModel("tests/fixtures/morph/A2_shuffled.bin");
    std::vector<float> in = ReadWav("tests/fixtures/nam/input.wav");
    std::vector<float> na = std::vector<float>(a2::kWeightCount), nb = std::vector<float>(a2::kWeightCount);
    std::vector<float> scratch = std::vector<float>(a2::kWeightCount);
    std::unique_ptr<dkmorph::Weights> w = std::make_unique<dkmorph::Weights>();
    std::unique_ptr<dkdsp::A2Lite> engine = std::make_unique<dkdsp::A2Lite>();

    PathRig() {
        EXPECT_GE(in.size(), kFrom + kSettle + kCompared);
        in.erase(in.begin(), in.begin() + std::min(in.size(), kFrom));
        in.resize(kSettle + kCompared);
        dkmorph::Apply(dkmorph::kIdentity, a->weights, a->head_scale, na.data());
        dkmorph::Apply(dkmorph::kIdentity, b->weights, b->head_scale, nb.data());
    }

    // The stream at c of the way from A to B: a waypoint on the chord.
    std::vector<float> Chord(float c) const {
        std::vector<float> s(a2::kWeightCount);
        for (size_t i = 0; i < s.size(); ++i) s[i] = na[i] + c * (nb[i] - na[i]);
        return s;
    }

    std::vector<float> Run() {
        std::vector<float> out(in.size());
        for (size_t i = 0; i < in.size(); i += dkdsp::kBlockSize)
            engine->Process(in.data() + i, out.data() + i, std::min(dkdsp::kBlockSize, in.size() - i));
        return out;
    }

    // The path through the waypoints (each a stream), rendered at blend position t.
    std::vector<float> Render(const std::vector<std::vector<float>>& waypoints, float t) {
        std::vector<float> flat;
        for (const std::vector<float>& p : waypoints) flat.insert(flat.end(), p.begin(), p.end());
        dkmorph::Prepare(dkmorph::kIdentity, *a, *b, flat.empty() ? nullptr : flat.data(), int(waypoints.size()),
                         scratch.data(), w.get());
        EXPECT_EQ(w->segments, int(waypoints.size()) + 1);
        engine->Load(a->weights, a->head_scale);
        const dkmorph::Segment g = dkmorph::SegmentAt(t, w->segments);
        engine->Blend(w->a[g.s], w->d[g.s], g.f);
        return Run();
    }

    // The network of the stream (head scale folded in), loaded as a model is.
    std::vector<float> RenderStream(const std::vector<float>& stream) {
        engine->Load(stream.data(), 1.0f);
        return Run();
    }
};

// The largest difference of two renders past their first kSettle samples.
float Worst(const std::vector<float>& x, const std::vector<float>& y) {
    float worst = 0.0f;
    for (size_t i = kSettle; i < x.size(); ++i) worst = std::max(worst, std::fabs(x[i] - y[i]));
    return worst;
}

}  // namespace

// Waypoints on the chord at 1/4, 1/2 and 3/4 make the path the chord: every position renders what the straight
// blend renders there, which a wrong segment, a wrong fraction or waypoints out of order would not.
TEST(MorphPath, WaypointsOnTheChordAreTheStraightBlend) {
    PathRig r;
    ASSERT_NE(r.a, nullptr);
    ASSERT_NE(r.b, nullptr);
    const std::vector<std::vector<float>> chord = {r.Chord(0.25f), r.Chord(0.5f), r.Chord(0.75f)};
    for (float t : {0.0f, 0.1f, 0.25f, 0.4f, 0.5f, 0.625f, 0.75f, 0.9f, 1.0f})
        EXPECT_LE(Worst(r.Render(chord, t), r.Render({}, t)), 1e-4f) << "t " << t;
    // The chord's positions are networks apart, so the comparison above tells them from one another.
    EXPECT_GT(Worst(r.Render({}, 0.25f), r.Render({}, 0.5f)), 1e-2f);
    EXPECT_GT(Worst(r.Render({}, 0.75f), r.Render({}, 0.5f)), 1e-2f);
    EXPECT_GT(Worst(r.Render({}, 0.1f), r.Render({}, 0.4f)), 1e-2f);
    // Out of order, the same waypoints are another path.
    EXPECT_GT(Worst(r.Render({chord[2], chord[1], chord[0]}, 0.3f), r.Render({}, 0.3f)), 1e-2f);
}

// Waypoints off the chord: each knot is its waypoint's network exactly, in stored order, and the ends are the two
// models.
TEST(MorphPath, KnotsAreTheWaypoints) {
    PathRig r;
    ASSERT_NE(r.a, nullptr);
    ASSERT_NE(r.b, nullptr);
    // Points of the chord pushed off it, each by its own amount, and not in the chord's order.
    const float along[3] = {0.5f, 0.2f, 0.8f};
    std::vector<std::vector<float>> path;
    for (int k = 0; k < 3; ++k) {
        std::vector<float> s = r.Chord(along[k]);
        for (size_t i = 0; i < s.size(); ++i) s[i] *= 1.0f + 0.02f * float(k + 1) * ((i & 1) != 0 ? 1.0f : -1.0f);
        path.push_back(s);
    }
    const std::vector<float> knots[3] = {r.RenderStream(path[0]), r.RenderStream(path[1]), r.RenderStream(path[2])};
    for (int k = 0; k < 3; ++k) {
        EXPECT_LE(Worst(r.Render(path, 0.25f * float(k + 1)), knots[k]), 1e-6f) << "knot " << k;
        for (int j = 0; j < k; ++j) EXPECT_GT(Worst(knots[k], knots[j]), 1e-2f) << k << " against " << j;
    }
    EXPECT_LE(Worst(r.Render(path, 0.0f), r.RenderStream(r.na)), 1e-6f);
    EXPECT_LE(Worst(r.Render(path, 1.0f), r.RenderStream(r.nb)), 1e-4f);
    // Between two knots the network is neither: half way along the first segment is the mean of A and waypoint 0.
    std::vector<float> mid(a2::kWeightCount);
    for (size_t i = 0; i < mid.size(); ++i) mid[i] = 0.5f * (r.na[i] + path[0][i]);
    EXPECT_LE(Worst(r.Render(path, 0.125f), r.RenderStream(mid)), 1e-4f);
    EXPECT_GT(Worst(r.Render(path, 0.125f), knots[0]), 1e-2f);
}

// tests/fixtures/morph/A2_path.preset: A2 morphing into its shuffled self through three waypoints that are A2 in
// other coordinates. Every node of that path is A2's network, so the editor's image, parsed and prepared here,
// renders the reference at every knot and another network between two: the waypoints' place in the image, their
// order and their head scale agree across the two languages.
TEST(MorphPath, FixturePresetRendersA2AtEveryKnot) {
    PathRig r;
    ASSERT_NE(r.b, nullptr);
    const std::vector<uint8_t> image = ReadFile(RepoPath("tests/fixtures/morph/A2_path.preset"));
    auto p = std::make_unique<dkpreset::Preset>();
    ASSERT_EQ(dkpreset::Parse(image.data(), image.size(), p.get()), dkpreset::Status::kOk);
    ASSERT_TRUE(p->has_morph);
    ASSERT_EQ(p->waypoint_count, 3);
    EXPECT_EQ(p->morph.target, 1);
    EXPECT_EQ(p->morph.model_crc, r.b->crc);
    std::vector<float> want = ReadWav("tests/fixtures/nam/A2.input.ref.wav");
    ASSERT_GE(want.size(), kFrom + kSettle + kCompared);
    want.erase(want.begin(), want.begin() + kFrom);
    want.resize(kSettle + kCompared);

    dkmorph::Prepare(p->morph.t, p->model, *r.b, p->waypoints[0], p->waypoint_count, r.scratch.data(), r.w.get());
    auto render = [&](float t) {
        r.engine->Load(p->model.weights, p->model.head_scale);
        const dkmorph::Segment g = dkmorph::SegmentAt(t, r.w->segments);
        r.engine->Blend(r.w->a[g.s], r.w->d[g.s], g.f);
        return r.Run();
    };
    for (float t : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) EXPECT_LE(Worst(render(t), want), 1e-3f) << "t " << t;
    for (float t : {0.125f, 0.375f, 0.625f, 0.875f}) EXPECT_GT(Worst(render(t), want), 1e-2f) << "t " << t;
}
