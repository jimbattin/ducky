// daisy/src/upload.h: UPLOAD BEGIN/CHUNK staging (ordering, resends, gaps, CRC, bounds).
#include "upload.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

using namespace dklink;

namespace {

std::vector<uint8_t> BeginPayload(int slot, uint32_t size) {
    std::vector<uint8_t> p(4);
    p[0] = uint8_t(slot);
    PutSeptets(p.data() + 1, size, 3);
    return p;
}

std::vector<uint8_t> ChunkPayload(uint32_t offset, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> p(8 + Packed7Size(data.size()));
    PutSeptets(p.data(), offset, 3);
    PutSeptets(p.data() + 3, dkmodel::Crc32(data.data(), data.size()), 5);
    Pack7(data.data(), data.size(), p.data() + 8);
    return p;
}

// The image's bytes: i & 0xFF.
std::vector<uint8_t> Image(size_t n) {
    std::vector<uint8_t> b(n);
    for (size_t i = 0; i < n; ++i) b[i] = uint8_t(i & 0xFF);
    return b;
}

std::vector<uint8_t> Slice(const std::vector<uint8_t>& b, size_t off, size_t n) {
    return std::vector<uint8_t>(b.begin() + off, b.begin() + std::min(off + n, b.size()));
}

struct Fixture {
    std::vector<uint8_t> buf = std::vector<uint8_t>(dkpreset::kSizeWithIr);
    Upload up{buf.data()};
    Result Begin(int slot, uint32_t size) {
        const std::vector<uint8_t> p = BeginPayload(slot, size);
        return up.Begin(p.data(), p.size());
    }
    Result Chunk(uint32_t offset, const std::vector<uint8_t>& data) {
        const std::vector<uint8_t> p = ChunkPayload(offset, data);
        return up.Chunk(p.data(), p.size());
    }
};

}  // namespace

TEST(Upload, ChunkCrcVector) {
    const std::vector<uint8_t> p = ChunkPayload(512, Image(256));
    EXPECT_EQ(p.size(), 8 + Packed7Size(256));
    EXPECT_EQ(std::vector<uint8_t>(p.begin(), p.begin() + 3), (std::vector<uint8_t>{0x00, 0x04, 0x00}));
    EXPECT_EQ(std::vector<uint8_t>(p.begin() + 3, p.begin() + 8),
              (std::vector<uint8_t>{0x02, 0x48, 0x16, 0x18, 0x73}));
    const std::vector<uint8_t> img = Image(256);
    EXPECT_EQ(dkmodel::Crc32(img.data(), img.size()), 0x29058C73u);
}

TEST(Upload, BadBeginLeavesNoUpload) {
    Fixture f;
    ASSERT_EQ(f.Begin(0, dkpreset::kSizeWithIr), Result::kOk);
    EXPECT_EQ(f.Begin(8, dkpreset::kSizeWithIr), Result::kBadRequest);
    EXPECT_FALSE(f.up.active());
    EXPECT_EQ(f.Begin(0, 1000), Result::kBadRequest);
    EXPECT_FALSE(f.up.active());
    const uint8_t shortPayload[] = {0, 0, 0x7D};
    EXPECT_EQ(f.up.Begin(shortPayload, sizeof shortPayload), Result::kBadRequest);
    EXPECT_FALSE(f.up.active());
    EXPECT_EQ(f.up.Ready(), Result::kNoUpload);
}

TEST(Upload, InOrderCompletes) {
    for (size_t size : {dkpreset::kSizeNoIr, dkpreset::kSizeWithIr}) {
        Fixture f;
        const std::vector<uint8_t> img = Image(size);
        ASSERT_EQ(f.Begin(5, uint32_t(size)), Result::kOk);
        EXPECT_EQ(f.up.slot(), 5);
        for (size_t off = 0; off < size; off += kChunkBytes) {
            EXPECT_EQ(f.up.Ready(), Result::kIncomplete);
            ASSERT_EQ(f.Chunk(uint32_t(off), Slice(img, off, kChunkBytes)), Result::kOk) << off;
            EXPECT_EQ(f.up.next(), std::min(off + kChunkBytes, size));
        }
        EXPECT_EQ(f.up.Ready(), Result::kOk);
        EXPECT_EQ(Slice(f.buf, 0, size), img);
    }
}

TEST(Upload, ResentChunkIsIdempotent) {
    Fixture f;
    const std::vector<uint8_t> img = Image(dkpreset::kSizeNoIr);
    ASSERT_EQ(f.Begin(0, dkpreset::kSizeNoIr), Result::kOk);
    ASSERT_EQ(f.Chunk(0, Slice(img, 0, 256)), Result::kOk);
    ASSERT_EQ(f.Chunk(256, Slice(img, 256, 256)), Result::kOk);
    EXPECT_EQ(f.Chunk(256, Slice(img, 256, 256)), Result::kOk);
    EXPECT_EQ(f.up.next(), 512u);
    EXPECT_EQ(f.Chunk(0, Slice(img, 0, 256)), Result::kOk);
    EXPECT_EQ(f.up.next(), 512u);
}

TEST(Upload, GapIsOffset) {
    Fixture f;
    const std::vector<uint8_t> img = Image(dkpreset::kSizeNoIr);
    ASSERT_EQ(f.Begin(0, dkpreset::kSizeNoIr), Result::kOk);
    ASSERT_EQ(f.Chunk(0, Slice(img, 0, 256)), Result::kOk);
    EXPECT_EQ(f.Chunk(512, Slice(img, 512, 256)), Result::kOffset);
    EXPECT_EQ(f.up.next(), 256u);
}

TEST(Upload, FlippedCrcIsCrc) {
    Fixture f;
    ASSERT_EQ(f.Begin(0, dkpreset::kSizeNoIr), Result::kOk);
    std::vector<uint8_t> p = ChunkPayload(0, Image(256));
    p[7] ^= 0x01;
    EXPECT_EQ(f.up.Chunk(p.data(), p.size()), Result::kCrc);
    EXPECT_EQ(f.up.next(), 0u);
}

TEST(Upload, ChunkWithoutBegin) {
    Fixture f;
    EXPECT_EQ(f.Chunk(0, Image(256)), Result::kNoUpload);
}

TEST(Upload, ChunkPastSizeIsBadRequest) {
    Fixture f;
    const std::vector<uint8_t> img = Image(dkpreset::kSizeNoIr);
    ASSERT_EQ(f.Begin(0, dkpreset::kSizeNoIr), Result::kOk);
    uint32_t off = 0;
    for (; off + 256 <= dkpreset::kSizeNoIr; off += 256) ASSERT_EQ(f.Chunk(off, Slice(img, off, 256)), Result::kOk);
    EXPECT_EQ(f.Chunk(off, Image(256)), Result::kBadRequest);  // 7936 + 256 > 7996
    EXPECT_EQ(f.up.next(), off);
}

TEST(Upload, MalformedChunkIsBadRequest) {
    Fixture f;
    ASSERT_EQ(f.Begin(0, dkpreset::kSizeNoIr), Result::kOk);
    EXPECT_EQ(f.Chunk(0, {}), Result::kBadRequest);         // no data
    EXPECT_EQ(f.Chunk(0, Image(257)), Result::kBadRequest);  // over kChunkBytes
}
