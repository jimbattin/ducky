// daisy/src/preset.h: the preset slot parser, its gate block, the state record and slot stepping.
#include "preset.h"

#include <gtest/gtest.h>

#include <algorithm>
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
void FixCrc(std::vector<uint8_t>& b) { Put32(b, b.size() - 4, dkbin::Crc32(b.data(), b.size() - 4)); }

Status ParseBytes(const std::vector<uint8_t>& b) {
    auto p = std::make_unique<Preset>();
    return Parse(b.data(), b.size(), p.get());
}

std::vector<uint8_t> State(uint32_t seq, int slot) {
    std::vector<uint8_t> r(dkpreset::kStateBytes);
    dkpreset::BuildState(r.data(), seq, slot);
    return r;
}

std::vector<uint8_t> MorphBlock() {
    std::vector<uint8_t> b = ReadFile(RepoPath("tests/fixtures/morph/A2_shuffled.morph"));
    EXPECT_EQ(b.size(), dkmorph::kBlockBytes);
    return b;
}

// image with its morph block set to block (null: removed).
std::vector<uint8_t> WithMorph(const std::vector<uint8_t>& image, const uint8_t* block) {
    std::vector<uint8_t> out(dkpreset::kSizeMax);
    out.resize(dkpreset::SetMorph(image.data(), image.size(), block, out.data()));
    return out;
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
    const dkdsp::GateSettings off = dkdsp::kGateOff;
    EXPECT_EQ(p->gate.enabled, off.enabled);
    EXPECT_EQ(p->gate.threshold, off.threshold);
    EXPECT_EQ(p->gate.attack, off.attack);
    EXPECT_EQ(p->gate.hold, off.hold);
    EXPECT_EQ(p->gate.release, off.release);
    EXPECT_EQ(p->gate.ratio, off.ratio);
}

TEST(PresetRename, FixtureParsesWithNewName) {
    std::vector<uint8_t> b = Fixture();
    const char name[] = "Renamed";
    dkpreset::Rename(b.data(), b.size(), name, sizeof name - 1);
    auto p = std::make_unique<Preset>();
    ASSERT_EQ(Parse(b.data(), b.size(), p.get()), Status::kOk);
    EXPECT_STREQ(p->name, "Renamed");
    EXPECT_STREQ(p->model.name, "A2");
}

TEST(PresetGate, SetGateRoundTrip) {
    std::vector<uint8_t> b = Fixture();
    dkpreset::SetGate(b.data(), b.size(), {true, 55, 15, 30, 250, 60});
    const std::vector<uint8_t> block(b.begin() + 76, b.begin() + 84);
    EXPECT_EQ(block, (std::vector<uint8_t>{0x01, 0x37, 0x0F, 0x1E, 0xFA, 0x00, 0x3C, 0x00}));
    auto p = std::make_unique<Preset>();
    ASSERT_EQ(Parse(b.data(), b.size(), p.get()), Status::kOk);
    EXPECT_TRUE(p->gate.enabled);
    EXPECT_EQ(p->gate.threshold, 55);
    EXPECT_EQ(p->gate.attack, 15);
    EXPECT_EQ(p->gate.hold, 30);
    EXPECT_EQ(p->gate.release, 250);
    EXPECT_EQ(p->gate.ratio, 60);
    EXPECT_STREQ(p->name, "A2 + decay");
    EXPECT_STREQ(p->model.name, "A2");
}

TEST(PresetGate, OutOfRangeIsClamped) {
    std::vector<uint8_t> b = Fixture();
    const uint8_t block[] = {0xFF, 0x00, 0xFF, 0x00, 0xFF, 0xFF, 0xFF, 0x00};
    std::copy(block, block + 8, b.begin() + 76);
    FixCrc(b);
    auto p = std::make_unique<Preset>();
    ASSERT_EQ(Parse(b.data(), b.size(), p.get()), Status::kOk);
    EXPECT_TRUE(p->gate.enabled);
    EXPECT_EQ(p->gate.threshold, 30);
    EXPECT_EQ(p->gate.attack, 100);
    EXPECT_EQ(p->gate.hold, 10);
    EXPECT_EQ(p->gate.release, 1000);
    EXPECT_EQ(p->gate.ratio, 100);
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
    Put16(b, 6, 4);
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
    b[84 + 1000] ^= 0xFF;  // the model's own CRC no longer matches
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kModel);
}

TEST(PresetParse, BadEmbeddedIrIsIr) {
    std::vector<uint8_t> b = Fixture();
    b[7992 + 1000] ^= 0xFF;
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kIr);
}

TEST(PresetParse, BadMorphBlockIsMorph) {
    std::vector<uint8_t> b = WithMorph(Fixture(), MorphBlock().data());
    ASSERT_EQ(ParseBytes(b), Status::kOk);
    const size_t perm = b.size() - 4 - dkmorph::kBlockBytes + 1;
    b[perm] = 0;
    b[perm + 1] = 0;
    b[perm + 2] = 1;
    FixCrc(b);
    EXPECT_EQ(ParseBytes(b), Status::kMorph);
}

TEST(PresetMorph, SetMorphRoundTrip) {
    const std::vector<uint8_t> plain = Fixture(), block = MorphBlock();
    auto p = std::make_unique<Preset>();
    ASSERT_EQ(Parse(plain.data(), plain.size(), p.get()), Status::kOk);
    EXPECT_FALSE(p->has_morph);

    const std::vector<uint8_t> morphed = WithMorph(plain, block.data());
    EXPECT_EQ(morphed.size(), dkpreset::kSizeMax);
    ASSERT_EQ(Parse(morphed.data(), morphed.size(), p.get()), Status::kOk);
    EXPECT_TRUE(p->has_morph);
    EXPECT_EQ(p->morph.target, 0);
    EXPECT_EQ(p->morph.model_crc, 0xCB02C66Fu);
    EXPECT_EQ(p->morph.t.perm[0], 1);
    EXPECT_EQ(p->morph.t.perm[1], 2);
    EXPECT_EQ(p->morph.t.perm[2], 0);
    EXPECT_TRUE(p->has_ir);
    EXPECT_GT(p->ir.tap_count, 0u);
    EXPECT_STREQ(p->name, "A2 + decay");

    EXPECT_EQ(WithMorph(morphed, block.data()), morphed) << "a second block replaces the first";
    EXPECT_EQ(WithMorph(morphed, nullptr), plain);
}

TEST(PresetMorph, ResolveMorph) {
    std::vector<Preset> presets(dkpreset::kSlots);
    bool filled[dkpreset::kSlots] = {};
    auto resolve = [&](int a) { return dkpreset::ResolveMorph(presets.data(), filled, a); };
    filled[1] = filled[4] = true;
    presets[1].model.crc = 0x1111;
    presets[4].model.crc = 0x4444;
    presets[4].has_morph = true;
    presets[4].morph.target = 1;
    presets[4].morph.model_crc = 0x1111;
    EXPECT_EQ(resolve(4), 1);
    EXPECT_EQ(resolve(1), -1) << "no block";

    // The target's model moved to slot 6 (a swap): found there, whatever slot 1 holds now.
    filled[6] = true;
    presets[6].model.crc = 0x1111;
    presets[1].model.crc = 0x2222;
    EXPECT_EQ(resolve(4), 6);
    filled[1] = false;
    EXPECT_EQ(resolve(4), 6);

    presets[6].model.crc = 0x3333;
    EXPECT_EQ(resolve(4), -1) << "no slot holds the model the block was fitted to";
    filled[6] = false;
    presets[6].model.crc = 0x1111;
    EXPECT_EQ(resolve(4), -1) << "an empty slot's stale preset does not count";

    presets[4].morph.target = 4;
    presets[4].morph.model_crc = 0x4444;
    EXPECT_EQ(resolve(4), -1) << "a preset does not morph with itself";
    filled[4] = false;
    EXPECT_EQ(resolve(4), -1);
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
