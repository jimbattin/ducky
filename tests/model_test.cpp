// daisy/src/model.h: the format-2 model binary parser.
#include "model.h"

#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "test_files.h"

using dkbin::Crc32;
using dkmodel::Model;
using dkmodel::Parse;
using dkmodel::Status;

namespace {

std::vector<uint8_t> A2Bin() {
    std::vector<uint8_t> b = ReadFile(RepoPath("tests/fixtures/nam/A2.bin"));
    EXPECT_EQ(b.size(), dkmodel::kBinSize);
    return b;
}

void Put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(v >> (8 * i));
}

void FixCrc(std::vector<uint8_t>& b) { Put32(b, 7904, Crc32(b.data(), 7904)); }

Status ParseBytes(const std::vector<uint8_t>& b) {
    auto m = std::make_unique<Model>();
    return Parse(b.data(), b.size(), m.get());
}

}  // namespace

TEST(Crc32, CheckValue) {
    const char* s = "123456789";
    EXPECT_EQ(Crc32(reinterpret_cast<const uint8_t*>(s), std::strlen(s)), 0xCBF43926u);
}

TEST(Parse, A2Fixture) {
    const std::vector<uint8_t> b = A2Bin();
    auto m = std::make_unique<Model>();
    ASSERT_EQ(Parse(b.data(), b.size(), m.get()), Status::kOk);
    EXPECT_TRUE(m->flags & dkmodel::kFlagHasLoudness);
    EXPECT_NE(std::memchr(m->name, '\0', dkmodel::kNameSize), nullptr);
    EXPECT_GT(std::strlen(m->name), 0u);
}

TEST(Parse, TruncatedIsSize) {
    std::vector<uint8_t> b = A2Bin();
    b.resize(dkmodel::kBinSize - 1);
    EXPECT_EQ(ParseBytes(b), Status::kSize);
}

TEST(Parse, FlippedWeightByteIsCrc) {
    std::vector<uint8_t> b = A2Bin();
    b[1000] ^= 0xFF;
    EXPECT_EQ(ParseBytes(b), Status::kCrc);
}

TEST(Parse, OtherShapeIsShape) {
    std::vector<uint8_t> b = A2Bin();
    b[40] = 2;  // dilation 0, u16 LE
    b[41] = 0;
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kShape);
}

TEST(Parse, NanWeightIsWeights) {
    std::vector<uint8_t> b = A2Bin();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    uint32_t u;
    std::memcpy(&u, &nan, sizeof u);
    Put32(b, 92, u);
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kWeights);
}
