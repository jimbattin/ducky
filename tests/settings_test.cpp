// daisy/src/settings.h: the settings record's layout, validation, the newest-record choice and decoding.
#include "settings.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <vector>

using namespace dkparam;

namespace {

Settings Sample() {
    Settings s = FactorySettings();
    s.assign[0] = kLowCutSlope;
    s.assign[1] = kBell2Freq;
    s.assign[6] = kEqEnable;
    s.assign[7] = kMorph;
    s.assign[8] = kBell1Gain;
    s.exp_channel = 10;
    s.midi_in_channel = 5;
    s.value[kEqEnable] = 0.0f;
    s.value[kLowCutFreq] = 0.25f;
    s.value[kHighCutFreq] = 0.75f;
    s.value[kLowCutSlope] = 2.0f;
    s.value[kBell1Gain] = 0.75f;
    return s;
}

std::vector<uint8_t> Record(uint32_t seq, const Settings& s) {
    std::vector<uint8_t> r(kSettingsBytes);
    BuildSettings(r.data(), seq, s);
    return r;
}

void PutF32(std::vector<uint8_t>& r, size_t off, float v) {
    uint32_t bits;
    std::memcpy(&bits, &v, sizeof bits);
    dkbin::PutLe32(r.data() + off, bits);
}

// The CRC over the bytes before it, for a record holding n parameters.
void Seal(std::vector<uint8_t>& r, size_t n) {
    const size_t body = kSettingsHeader + 4 * n;
    dkbin::PutLe32(r.data() + body, dkbin::Crc32(r.data(), body));
}

// A version 3 record (before PEDAL) with `slots` control slots (7: before the expression slot, its channel byte 0),
// built by hand: assign[slots] from byte 12, values from byte 20.
std::vector<uint8_t> RecordBeforePedal(uint32_t seq, const Settings& s, uint8_t slots) {
    std::vector<uint8_t> r(kSettingsHeaderBeforePedal + 4 * kParamCount + 4, 0);
    dkbin::PutLe32(r.data(), kSettingsMagic);
    dkbin::PutLe32(r.data() + 4, seq);
    r[8] = kSettingsVersionBeforePedal;
    r[9] = slots;
    r[10] = kParamCount;
    r[11] = slots == kSettingsSlotsBeforeExp ? 0 : s.exp_channel;
    for (int c = 0; c < slots; ++c) r[12 + c] = s.assign[c];
    for (int i = 0; i < kParamCount; ++i) PutF32(r, kSettingsHeaderBeforePedal + 4 * i, s.value[i]);
    const size_t body = kSettingsHeaderBeforePedal + 4 * kParamCount;
    dkbin::PutLe32(r.data() + body, dkbin::Crc32(r.data(), body));
    return r;
}

}  // namespace

TEST(Settings, FactoryIsTheFactoryLayoutAtTheDefaults) {
    const Settings s = FactorySettings();
    for (int c = 0; c < kControlSlots; ++c) EXPECT_EQ(s.assign[c], kFactoryAssign[c]) << c;
    for (int i = 0; i < kParamCount; ++i) EXPECT_EQ(s.value[i], kParams[i].def) << i;
}

// A record of an earlier table's layout (version 1: M9's cuts; 2: comp_enable and comp_mode apart) is ignored: its
// ids mean other parameters.
TEST(Settings, EarlierVersionsAreInvalid) {
    for (uint8_t version : {1, 2}) {
        std::vector<uint8_t> r = Record(3, Sample());
        r[8] = version;
        Seal(r, kParamCount);
        uint32_t seq = 0;
        EXPECT_FALSE(SettingsValid(r.data(), seq)) << int(version);
    }
}

TEST(Settings, RoundTrip) {
    const Settings s = Sample();
    const std::vector<uint8_t> r = Record(9, s);
    uint32_t seq = 0;
    ASSERT_TRUE(SettingsValid(r.data(), seq));
    EXPECT_EQ(seq, 9u);
    const Settings d = DecodeSettings(r.data());
    for (int c = 0; c < kControlSlots; ++c) EXPECT_EQ(d.assign[c], s.assign[c]) << c;
    for (int i = 0; i < kParamCount; ++i) EXPECT_EQ(d.value[i], s.value[i]) << i;
    EXPECT_EQ(d.exp_channel, 10);
    EXPECT_EQ(d.midi_in_channel, 5);
}

// A version 3 record written before the expression slot (7 slots, byte 11 zero) leaves exp and pedal at the factory
// layout's.
TEST(Settings, SevenSlotRecordLeavesExpAndPedalAtFactory) {
    const Settings s = Sample();
    const std::vector<uint8_t> r = RecordBeforePedal(4, s, 7);
    uint32_t seq = 0;
    ASSERT_TRUE(SettingsValid(r.data(), seq));
    EXPECT_EQ(seq, 4u);
    const Settings d = DecodeSettings(r.data());
    for (int c = 0; c < 7; ++c) EXPECT_EQ(d.assign[c], s.assign[c]) << c;
    EXPECT_EQ(d.assign[7], kFactoryAssign[7]);
    EXPECT_EQ(d.assign[8], kFactoryAssign[8]);
    for (int i = 0; i < kParamCount; ++i) EXPECT_EQ(d.value[i], s.value[i]) << i;
    EXPECT_EQ(d.exp_channel, 0);
    EXPECT_EQ(d.midi_in_channel, 0);
}

// A version 3 record written before PEDAL (8 slots) keeps its slots and expression channel; PEDAL takes the factory
// layout's, MIDI IN omni.
TEST(Settings, EightSlotRecordLeavesPedalAtFactory) {
    const Settings s = Sample();
    const std::vector<uint8_t> r = RecordBeforePedal(5, s, 8);
    uint32_t seq = 0;
    ASSERT_TRUE(SettingsValid(r.data(), seq));
    const Settings d = DecodeSettings(r.data());
    for (int c = 0; c < 8; ++c) EXPECT_EQ(d.assign[c], s.assign[c]) << c;
    EXPECT_EQ(d.assign[8], kFactoryAssign[8]);
    for (int i = 0; i < kParamCount; ++i) EXPECT_EQ(d.value[i], s.value[i]) << i;
    EXPECT_EQ(d.exp_channel, 10);
    EXPECT_EQ(d.midi_in_channel, 0);
}

TEST(Settings, OtherSlotCountsAreInvalid) {
    for (uint8_t slots : {6, 7, 8, 10}) {
        std::vector<uint8_t> r = Record(4, Sample());
        r[9] = slots;
        Seal(r, kParamCount);
        uint32_t seq = 0;
        EXPECT_FALSE(SettingsValid(r.data(), seq)) << int(slots);
    }
    for (uint8_t slots : {6, 9}) {
        std::vector<uint8_t> r = RecordBeforePedal(4, Sample(), 8);
        r[9] = slots;
        const size_t body = kSettingsHeaderBeforePedal + 4 * kParamCount;
        dkbin::PutLe32(r.data() + body, dkbin::Crc32(r.data(), body));
        uint32_t seq = 0;
        EXPECT_FALSE(SettingsValid(r.data(), seq)) << "version 3, " << int(slots);
    }
}

TEST(Settings, ChannelPast16DecodesToOmni) {
    std::vector<uint8_t> r = Record(4, Sample());
    r[11] = 17;
    r[12] = 17;
    Seal(r, kParamCount);
    uint32_t seq = 0;
    ASSERT_TRUE(SettingsValid(r.data(), seq));
    const Settings d = DecodeSettings(r.data());
    EXPECT_EQ(d.exp_channel, 0);
    EXPECT_EQ(d.midi_in_channel, 0);
}

TEST(Settings, FlippedPayloadByteIsInvalid) {
    std::vector<uint8_t> r = Record(1, Sample());
    r[kSettingsHeader + 5] ^= 0x01;
    uint32_t seq = 77;
    EXPECT_FALSE(SettingsValid(r.data(), seq));
    EXPECT_EQ(seq, 77u);
}

TEST(Settings, NewestPicksHigherSeqOrTheOnlyValid) {
    const std::vector<uint8_t> a = Record(7, Sample()), b = Record(8, Sample());
    uint32_t seq = 0;
    EXPECT_EQ(NewestSettings(a.data(), b.data(), seq), 1);
    EXPECT_EQ(seq, 8u);
    EXPECT_EQ(NewestSettings(b.data(), a.data(), seq), 0);
    EXPECT_EQ(seq, 8u);
    // A torn write of the newest record leaves the previous one.
    std::vector<uint8_t> torn = b;
    torn[kSettingsBytes - 1] ^= 0xFF;
    EXPECT_EQ(NewestSettings(a.data(), torn.data(), seq), 0);
    EXPECT_EQ(seq, 7u);
    const std::vector<uint8_t> erased(kSettingsBytes, 0xFF);
    EXPECT_EQ(NewestSettings(erased.data(), b.data(), seq), 1);
    EXPECT_EQ(seq, 8u);
    seq = 5;
    EXPECT_EQ(NewestSettings(erased.data(), torn.data(), seq), -1);
    EXPECT_EQ(seq, 5u);
}

TEST(Settings, FewerParametersKeepTheLaterDefaults) {
    std::vector<uint8_t> r = Record(2, Sample());
    r[10] = 4;
    Seal(r, 4);
    uint32_t seq = 0;
    ASSERT_TRUE(SettingsValid(r.data(), seq));
    const Settings d = DecodeSettings(r.data());
    EXPECT_EQ(d.value[kEqEnable], 0.0f);
    EXPECT_EQ(d.value[kLowCutFreq], 0.25f);
    EXPECT_EQ(d.value[kLowCutSlope], 2.0f);
    for (int i = 4; i < kParamCount; ++i) EXPECT_EQ(d.value[i], kParams[i].def) << i;
}

TEST(Settings, NanValueDecodesToTheDefault) {
    std::vector<uint8_t> r = Record(2, Sample());
    PutF32(r, kSettingsHeader + 4 * kLowCutRes, NAN);
    Seal(r, kParamCount);
    uint32_t seq = 0;
    ASSERT_TRUE(SettingsValid(r.data(), seq));
    EXPECT_EQ(DecodeSettings(r.data()).value[kLowCutRes], kParams[kLowCutRes].def);
}

TEST(Settings, ContinuousParameterOnASwitchDecodesToNone) {
    std::vector<uint8_t> r = Record(2, Sample());
    r[13 + 4] = kLowCutFreq;
    r[13 + 5] = 0x50;  // no such parameter
    Seal(r, kParamCount);
    uint32_t seq = 0;
    ASSERT_TRUE(SettingsValid(r.data(), seq));
    const Settings d = DecodeSettings(r.data());
    EXPECT_EQ(d.assign[4], kNoParam);
    EXPECT_EQ(d.assign[5], kNoParam);
    EXPECT_EQ(d.assign[6], kEqEnable);
}
