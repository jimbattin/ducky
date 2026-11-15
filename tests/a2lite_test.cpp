// daisy/src/dsp/a2lite.h: the A2-Lite engine against NeuralAmpModelerCore's reference renders (docs/NAM_FORMAT.md).
#include "dsp/a2lite.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "model.h"
#include "test_files.h"
#include "wav.h"

using dkdsp::A2Lite;
using dkmodel::Model;

namespace {

constexpr float kMaxError = 1e-5f;

std::unique_ptr<Model> LoadModel(const std::string& path) {
    const std::vector<uint8_t> b = ReadFile(path);
    auto m = std::make_unique<Model>();
    const dkmodel::Status st = dkmodel::Parse(b.data(), b.size(), m.get());
    EXPECT_EQ(st, dkmodel::Status::kOk) << path << ": " << dkmodel::StatusName(st);
    return st == dkmodel::Status::kOk ? std::move(m) : nullptr;
}

std::vector<float> ReadWav(const std::string& path) {
    dkwav::Audio a;
    std::string err;
    EXPECT_TRUE(dkwav::Read(path, &a, &err)) << path << ": " << err;
    EXPECT_EQ(a.rate, 48000u) << path;
    return a.samples;
}

// Runs in through a fresh engine in chunks of block.
std::vector<float> Render(const Model& m, const std::vector<float>& in, size_t block) {
    auto e = std::make_unique<A2Lite>();
    e->Load(m.weights, m.head_scale);
    std::vector<float> out(in.size());
    for (size_t i = 0; i < in.size(); i += block) e->Process(in.data() + i, out.data() + i, std::min(block, in.size() - i));
    return out;
}

float MaxAbsError(const std::vector<float>& a, const std::vector<float>& b) {
    EXPECT_EQ(a.size(), b.size());
    float e = 0.0f;
    for (size_t i = 0; i < std::min(a.size(), b.size()); ++i) e = std::max(e, std::fabs(a[i] - b[i]));
    return e;
}

void ExpectMatchesReference(const std::string& bin, const std::string& stimulus, const std::string& ref) {
    const std::unique_ptr<Model> m = LoadModel(RepoPath(bin));
    ASSERT_NE(m, nullptr);
    const std::vector<float> in = ReadWav(RepoPath(stimulus));
    const std::vector<float> want = ReadWav(RepoPath(ref));
    ASSERT_EQ(in.size(), want.size());
    const float err = MaxAbsError(Render(*m, in, dkdsp::kBlockSize), want);
    std::printf("%s x %s: max abs error %g\n", bin.c_str(), stimulus.c_str(), double(err));
    EXPECT_LE(err, kMaxError);
}

const std::string kA2Bin = "tests/fixtures/nam/A2.bin";
const std::string kInput = "tests/fixtures/nam/input.wav";

}  // namespace

TEST(A2Lite, MatchesReference) {
    ASSERT_TRUE(FileExists(RepoPath(kA2Bin)));
    ASSERT_TRUE(FileExists(RepoPath(kInput)));
    ASSERT_TRUE(FileExists(RepoPath("tests/fixtures/nam/A2.input.ref.wav")));
    EXPECT_EQ(ReadWav(RepoPath(kInput)).size(), 96000u);
    ExpectMatchesReference(kA2Bin, kInput, "tests/fixtures/nam/A2.input.ref.wav");
}

// Development models and renders are git-ignored; each case skips when its files are absent.
struct LocalCase {
    const char* name;
    const char* bin;
    const char* stimulus;
    const char* ref;
};

class LocalReference : public testing::TestWithParam<LocalCase> {};

TEST_P(LocalReference, MatchesReference) {
    const LocalCase& c = GetParam();
    for (const char* f : {c.bin, c.stimulus, c.ref})
        if (!FileExists(RepoPath(f))) GTEST_SKIP() << f << " missing";
    ExpectMatchesReference(c.bin, c.stimulus, c.ref);
}

INSTANTIATE_TEST_SUITE_P(
    Renders, LocalReference,
    testing::Values(
        LocalCase{"A2_guitar_di", "tests/fixtures/nam/A2.bin", "renders/stimuli/guitar_di.wav",
                  "renders/ref/A2.guitar_di.ref.wav"},
        LocalCase{"fender_input", "models/fender_super_eob_with_cab.bin", "tests/fixtures/nam/input.wav",
                  "renders/ref/fender_super_eob_with_cab.input.ref.wav"},
        LocalCase{"fender_guitar_di", "models/fender_super_eob_with_cab.bin", "renders/stimuli/guitar_di.wav",
                  "renders/ref/fender_super_eob_with_cab.guitar_di.ref.wav"},
        LocalCase{"sss_input", "models/sss_with_cab_clean.bin", "tests/fixtures/nam/input.wav",
                  "renders/ref/sss_with_cab_clean.input.ref.wav"},
        LocalCase{"sss_guitar_di", "models/sss_with_cab_clean.bin", "renders/stimuli/guitar_di.wav",
                  "renders/ref/sss_with_cab_clean.guitar_di.ref.wav"}),
    [](const testing::TestParamInfo<LocalCase>& info) { return std::string(info.param.name); });

TEST(A2Lite, BlockSizeIndependent) {
    const std::unique_ptr<Model> m = LoadModel(RepoPath(kA2Bin));
    ASSERT_NE(m, nullptr);
    std::vector<float> in = ReadWav(RepoPath(kInput));
    in.resize(24000);
    const std::vector<float> ref = Render(*m, in, 48);
    for (size_t block : {size_t(1), size_t(7), size_t(100)}) {
        const std::vector<float> out = Render(*m, in, block);
        for (size_t i = 0; i < in.size(); ++i) ASSERT_EQ(out[i], ref[i]) << "block " << block << " sample " << i;
    }
    // An irregular schedule.
    auto e = std::make_unique<A2Lite>();
    e->Load(m->weights, m->head_scale);
    std::vector<float> out(in.size());
    constexpr size_t kSchedule[] = {1, 5, 48, 13, 31};
    for (size_t i = 0, s = 0; i < in.size(); s = (s + 1) % 5) {
        const size_t n = std::min(kSchedule[s], in.size() - i);
        e->Process(in.data() + i, out.data() + i, n);
        i += n;
    }
    for (size_t i = 0; i < in.size(); ++i) ASSERT_EQ(out[i], ref[i]) << "irregular, sample " << i;
}

TEST(A2Lite, ResetRestoresFreshState) {
    const std::unique_ptr<Model> m = LoadModel(RepoPath(kA2Bin));
    ASSERT_NE(m, nullptr);
    const std::vector<float> in = ReadWav(RepoPath(kInput));
    const std::vector<float> fresh = Render(*m, in, 48);
    auto e = std::make_unique<A2Lite>();
    e->Load(m->weights, m->head_scale);
    std::vector<float> out(in.size());
    e->Process(in.data(), out.data(), in.size());
    e->Reset();
    e->Process(in.data(), out.data(), in.size());
    for (size_t i = 0; i < in.size(); ++i) ASSERT_EQ(out[i], fresh[i]) << i;
}

TEST(A2Lite, SilenceIsSettled) {
    const std::unique_ptr<Model> m = LoadModel(RepoPath(kA2Bin));
    ASSERT_NE(m, nullptr);
    auto e = std::make_unique<A2Lite>();
    e->Load(m->weights, m->head_scale);
    std::vector<float> zeros(7000, 0.0f), out(zeros.size());
    e->Process(zeros.data(), out.data(), 4800);
    const float settled = out[0];
    for (size_t i = 0; i < 4800; ++i) ASSERT_EQ(out[i], settled) << i;

    static_assert(dkdsp::a2::kReceptiveField < 7000, "the zeros must flush the whole receptive field");
    std::vector<float> in = ReadWav(RepoPath(kInput));
    std::vector<float> in_out(in.size());
    e->Process(in.data(), in_out.data(), in.size());
    e->Process(zeros.data(), out.data(), zeros.size());
    EXPECT_EQ(out.back(), settled);
}

TEST(A2Lite, NoNanOrDenormal) {
    const std::unique_ptr<Model> m = LoadModel(RepoPath(kA2Bin));
    ASSERT_NE(m, nullptr);
    std::vector<float> in = ReadWav(RepoPath(kInput));
    in.resize(in.size() + 480000, 0.0f);
    for (int i = 0; i < 48000; ++i) in.push_back(i % 2 ? -1e-40f : 1e-40f);
    auto e = std::make_unique<A2Lite>();
    e->Load(m->weights, m->head_scale);
    std::vector<float> out(in.size());
    e->Process(in.data(), out.data(), in.size());
    for (size_t i = 0; i < out.size(); ++i) {
        ASSERT_TRUE(std::isfinite(out[i])) << i;
        ASSERT_NE(std::fpclassify(out[i]), FP_SUBNORMAL) << i;
    }
    EXPECT_TRUE(e->StateIsNormal());
}
