// daisy/src/preset.h: the preset slot parser, the state record and slot stepping.
#include "preset.h"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "test_files.h"

using dkpreset::Parse;
using dkpreset::Preset;
using dkpreset::Status;

namespace {

std::vector<uint8_t> Fixture() {
    std::vector<uint8_t> b = ReadFile(RepoPath("tests/fixtures/preset/A2_decay.bin"));
    EXPECT_EQ(b.size(), dkpreset::kSizeWithIr);
    return b;
}

void Put16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    b[off] = uint8_t(v);
    b[off + 1] = uint8_t(v >> 8);
}
void Put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[off + i] = uint8_t(v >> (8 * i));
}
// The slot CRC over the first size - 4 bytes, at size - 4.
void FixCrc(std::vector<uint8_t>& b) { Put32(b, b.size() - 4, dkmodel::Crc32(b.data(), b.size() - 4)); }

Status ParseBytes(const std::vector<uint8_t>& b) {
    auto p = std::make_unique<Preset>();
    return Parse(b.data(), b.size(), p.get());
}

std::vector<uint8_t> State(uint32_t seq, int slot) {
    std::vector<uint8_t> r(dkpreset::kStateBytes);
    dkpreset::BuildState(r.data(), seq, slot);
    return r;
}

}  // namespace

TEST(PresetParse, Fixture) {
    const std::vector<uint8_t> b = Fixture();
    auto p = std::make_unique<Preset>();
    ASSERT_EQ(Parse(b.data(), b.size(), p.get()), Status::kOk);
    EXPECT_TRUE(p->has_ir);
    EXPECT_STREQ(p->name, "A2 + decay");
    EXPECT_STREQ(p->model.name, "A2");
    EXPECT_GT(p->ir.tap_count, 0u);
}

TEST(PresetParse, ErasedTailIsIgnored) {
    std::vector<uint8_t> b = Fixture();
    b.resize(dkpreset::kSlotBytes, 0xFF);
    EXPECT_EQ(ParseBytes(b), Status::kOk);
}

TEST(PresetParse, ErasedIsEmpty) {
    EXPECT_EQ(ParseBytes(std::vector<uint8_t>(dkpreset::kSlotBytes, 0xFF)), Status::kEmpty);
}

TEST(PresetParse, OtherMagicIsMagic) {
    std::vector<uint8_t> b = Fixture();
    b[3] = 'X';
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kMagic);
}

TEST(PresetParse, FlippedByteIsCrc) {
    std::vector<uint8_t> b = Fixture();
    b[9000] ^= 0xFF;
    EXPECT_EQ(ParseBytes(b), Status::kCrc);
}

TEST(PresetParse, WrongSizeFieldIsSize) {
    std::vector<uint8_t> b = Fixture();
    Put32(b, 8, 16025);
    b.resize(dkpreset::kSlotBytes, 0xFF);
    EXPECT_EQ(ParseBytes(b), Status::kSize);
}

TEST(PresetParse, TruncatedIsSize) {
    std::vector<uint8_t> b = Fixture();
    b.resize(b.size() - 1);
    EXPECT_EQ(ParseBytes(b), Status::kSize);
}

TEST(PresetParse, UnknownFlagIsVersion) {
    std::vector<uint8_t> b = Fixture();
    Put16(b, 6, 2);
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kVersion);
}

TEST(PresetParse, OtherVersionIsVersion) {
    std::vector<uint8_t> b = Fixture();
    Put16(b, 4, 2);
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kVersion);
}

TEST(PresetParse, ModelOnly) {
    std::vector<uint8_t> b = Fixture();
    b.resize(dkpreset::kSizeNoIr - 4);
    Put16(b, 6, 0);
    Put32(b, 8, dkpreset::kSizeNoIr);
    b.resize(dkpreset::kSizeNoIr);
    FixCrc(b);
    auto p = std::make_unique<Preset>();
    ASSERT_EQ(Parse(b.data(), b.size(), p.get()), Status::kOk);
    EXPECT_FALSE(p->has_ir);
    EXPECT_STREQ(p->name, "A2 + decay");
}

TEST(PresetParse, BadEmbeddedModelIsModel) {
    std::vector<uint8_t> b = Fixture();
    b[76 + 1000] ^= 0xFF;  // the model's own CRC no longer matches
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kModel);
}

TEST(PresetParse, BadEmbeddedIrIsIr) {
    std::vector<uint8_t> b = Fixture();
    b[7728 + 1000] ^= 0xFF;
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kIr);
}

TEST(PresetState, HigherSeqWins) {
    const auto a = State(7, 2), b = State(8, 5);
    uint32_t seq = 0;
    int slot = -1;
    EXPECT_EQ(dkpreset::NewestState(a.data(), b.data(), seq, slot), 1);
    EXPECT_EQ(seq, 8u);
    EXPECT_EQ(slot, 5);
    EXPECT_EQ(dkpreset::NewestState(b.data(), a.data(), seq, slot), 0);
    EXPECT_EQ(slot, 5);
}

TEST(PresetState, ErasedLoses) {
    const auto a = State(3, 1);
    const std::vector<uint8_t> erased(dkpreset::kStateBytes, 0xFF);
    uint32_t seq = 0;
    int slot = -1;
    EXPECT_EQ(dkpreset::NewestState(erased.data(), a.data(), seq, slot), 1);
    EXPECT_EQ(seq, 3u);
    EXPECT_EQ(slot, 1);
}

TEST(PresetState, TornLoses) {
    const auto a = State(3, 1);
    auto torn = State(4, 6);
    torn[12] ^= 0xFF;  // magic intact, CRC wrong
    uint32_t seq = 0;
    int slot = -1;
    EXPECT_EQ(dkpreset::NewestState(a.data(), torn.data(), seq, slot), 0);
    EXPECT_EQ(seq, 3u);
    EXPECT_EQ(slot, 1);
}

TEST(PresetState, NeitherValid) {
    const std::vector<uint8_t> erased(dkpreset::kStateBytes, 0xFF);
    uint32_t seq = 99;
    int slot = 99;
    EXPECT_EQ(dkpreset::NewestState(erased.data(), erased.data(), seq, slot), -1);
    EXPECT_EQ(seq, 99u);
    EXPECT_EQ(slot, 99);
}

TEST(PresetState, SlotOutOfRangeIsInvalid) {
    const auto r = State(1, 8);
    uint32_t seq = 0;
    int slot = -1;
    EXPECT_FALSE(dkpreset::StateValid(r.data(), seq, slot));
}

TEST(PresetSlots, NextFilledSkipsAndWraps) {
    const bool f[8] = {false, true, false, true, false, false, false, true};
    EXPECT_EQ(dkpreset::NextFilled(f, 7), 1);
    EXPECT_EQ(dkpreset::NextFilled(f, 1), 3);
    EXPECT_EQ(dkpreset::NextFilled(f, 3), 7);
}

TEST(PresetSlots, NextFilledSingleAndNone) {
    const bool one[8] = {false, false, false, false, true, false, false, false};
    EXPECT_EQ(dkpreset::NextFilled(one, 4), 4);
    const bool none[8] = {};
    EXPECT_EQ(dkpreset::NextFilled(none, 0), -1);
}

TEST(PresetSlots, BootSlot) {
    const bool f[8] = {false, true, false, true, false, false, false, false};
    EXPECT_EQ(dkpreset::BootSlot(f, 3), 3);
    EXPECT_EQ(dkpreset::BootSlot(f, 2), 1);   // saved slot empty
    EXPECT_EQ(dkpreset::BootSlot(f, -1), 1);  // no saved slot
    const bool none[8] = {};
    EXPECT_EQ(dkpreset::BootSlot(none, 0), -1);
}
