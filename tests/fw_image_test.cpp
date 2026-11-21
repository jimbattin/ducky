// daisy/src/fw_image.h: FIRMWARE COMMIT's image checks, each failing alone on an otherwise valid image.
#include "fw_image.h"

#include <gtest/gtest.h>

#include <vector>

using namespace dkfwimg;

namespace {

// A valid image of n bytes for board 1: SP at the top of DTCM, reset vector inside the image (Thumb), the tag at 0x400.
std::vector<uint8_t> Image(size_t n = 2048) {
    std::vector<uint8_t> b(n);
    for (size_t i = 0; i < n; ++i) b[i] = uint8_t(i * 7);
    dkbin::PutLe32(b.data(), kStackHi);
    dkbin::PutLe32(b.data() + 4, kSramBase + 0x101);
    dkbin::PutLe32(b.data() + kTagOffset, kTagMagic);
    dkbin::PutLe32(b.data() + kTagOffset + 4, kTagVersion);
    dkbin::PutLe32(b.data() + kTagOffset + 8, 1);
    dkbin::PutLe32(b.data() + kTagOffset + 12, 0);
    return b;
}

Check Checked(const std::vector<uint8_t>& b, uint32_t board = 1) {
    return CheckImage(b.data(), b.size(), dkbin::Crc32(b.data(), b.size()), board);
}

}  // namespace

TEST(FwImage, ValidImage) {
    EXPECT_EQ(Checked(Image()), Check::kOk);
    EXPECT_EQ(Checked(Image(kMinImageBytes)), Check::kOk);
}

TEST(FwImage, WrongCrc) {
    const std::vector<uint8_t> b = Image();
    EXPECT_EQ(CheckImage(b.data(), b.size(), dkbin::Crc32(b.data(), b.size()) ^ 1u, 1), Check::kCrc);
}

TEST(FwImage, StackOutsideDtcm) {
    std::vector<uint8_t> b = Image();
    dkbin::PutLe32(b.data(), 0x30000000);
    EXPECT_EQ(Checked(b), Check::kVectors);
}

TEST(FwImage, EvenResetVector) {
    std::vector<uint8_t> b = Image();
    dkbin::PutLe32(b.data() + 4, kSramBase + 0x100);
    EXPECT_EQ(Checked(b), Check::kVectors);
}

TEST(FwImage, ResetVectorPastImage) {
    std::vector<uint8_t> b = Image();
    dkbin::PutLe32(b.data() + 4, kSramBase + uint32_t(b.size()) + 1);
    EXPECT_EQ(Checked(b), Check::kVectors);
}

TEST(FwImage, BadMagic) {
    std::vector<uint8_t> b = Image();
    b[kTagOffset] ^= 0xFF;
    EXPECT_EQ(Checked(b), Check::kTag);
}

TEST(FwImage, OtherVersion) {
    std::vector<uint8_t> b = Image();
    dkbin::PutLe32(b.data() + kTagOffset + 4, 2);
    EXPECT_EQ(Checked(b), Check::kTag);
}

TEST(FwImage, OtherBoard) {
    std::vector<uint8_t> b = Image();
    dkbin::PutLe32(b.data() + kTagOffset + 8, 2);
    EXPECT_EQ(Checked(b), Check::kBoard);
    EXPECT_EQ(Checked(Image(), 2), Check::kBoard);
}
