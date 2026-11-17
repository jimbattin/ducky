// daisy/src/ir.h: the format-1 IR binary parser.
#include "ir.h"

#include <gtest/gtest.h>

#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "test_files.h"

using dkir::Ir;
using dkir::Parse;
using dkir::Status;

namespace {

std::vector<uint8_t> DecayBin() {
    std::vector<uint8_t> b = ReadFile(RepoPath("tests/fixtures/ir/decay.bin"));
    EXPECT_EQ(b.size(), dkir::kBinSize);
    return b;
}

void Put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(v >> (8 * i));
}

void FixCrc(std::vector<uint8_t>& b) { Put32(b, 8288, dkbin::Crc32(b.data(), 8288)); }

Status ParseBytes(const std::vector<uint8_t>& b) {
    auto ir = std::make_unique<Ir>();
    return Parse(b.data(), b.size(), ir.get());
}

}  // namespace

TEST(IrParse, DecayFixture) {
    const std::vector<uint8_t> b = DecayBin();
    auto ir = std::make_unique<Ir>();
    ASSERT_EQ(Parse(b.data(), b.size(), ir.get()), Status::kOk);
    EXPECT_EQ(ir->tap_count, 2048u);
    EXPECT_EQ(ir->source_rate, 44100u);
    EXPECT_EQ(ir->source_frames, 4410u);
    EXPECT_EQ(ir->source_channels, 1u);
    EXPECT_STREQ(ir->name, "decay");
    EXPECT_EQ(ir->taps[2047], 0.0f);
}

TEST(IrParse, TruncatedIsSize) {
    std::vector<uint8_t> b = DecayBin();
    b.resize(8291);
    EXPECT_EQ(ParseBytes(b), Status::kSize);
}

TEST(IrParse, OtherMagicIsMagic) {
    std::vector<uint8_t> b = DecayBin();
    b[3] = 'M';
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kMagic);
}

TEST(IrParse, Version2IsVersion) {
    std::vector<uint8_t> b = DecayBin();
    b[4] = 2;
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kVersion);
}

TEST(IrParse, FlippedTapByteIsCrc) {
    std::vector<uint8_t> b = DecayBin();
    b[1000] ^= 0xFF;
    EXPECT_EQ(ParseBytes(b), Status::kCrc);
}

TEST(IrParse, TapCountOutOfRangeIsTaps) {
    for (uint32_t count : {0u, 2049u}) {
        std::vector<uint8_t> b = DecayBin();
        Put32(b, 12, count);
        FixCrc(b);
        EXPECT_EQ(ParseBytes(b), Status::kTaps) << count;
    }
}

TEST(IrParse, NonZeroTapPastCountIsTaps) {
    std::vector<uint8_t> b = DecayBin();
    Put32(b, 12, 2000);
    Put32(b, 96 + 4 * 2047, 0x3F800000);  // 1.0f
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kTaps);
}

TEST(IrParse, NanTapIsSamples) {
    std::vector<uint8_t> b = DecayBin();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    uint32_t u;
    std::memcpy(&u, &nan, sizeof u);
    Put32(b, 96 + 4 * 10, u);
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kSamples);
}
