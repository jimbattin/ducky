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
    s.assign[3] = kNoParam;
    s.value[kLowCutFreq] = 0.25f;
    s.value[kHighCutFreq] = 0.75f;
    s.value[kLowCutSlope] = 2.0f;
    s.value[kMid] = 0.0f;
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

}  // namespace

TEST(Settings, RoundTrip) {
    const Settings s = Sample();
    const std::vector<uint8_t> r = Record(9, s);
    uint32_t seq = 0;
    ASSERT_TRUE(SettingsValid(r.data(), seq));
    EXPECT_EQ(seq, 9u);
    const Settings d = DecodeSettings(r.data());
    for (int c = 0; c < kControlSlots; ++c) EXPECT_EQ(d.assign[c], s.assign[c]) << c;
    for (int i = 0; i < kParamCount; ++i) EXPECT_EQ(d.value[i], s.value[i]) << i;
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
    r[10] = 3;
    Seal(r, 3);
    uint32_t seq = 0;
    ASSERT_TRUE(SettingsValid(r.data(), seq));
    const Settings d = DecodeSettings(r.data());
    EXPECT_EQ(d.value[kLowCutFreq], 0.25f);
    EXPECT_EQ(d.value[kLowCutSlope], 2.0f);
    for (int i = 3; i < kParamCount; ++i) EXPECT_EQ(d.value[i], kParams[i].def) << i;
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
    r[12 + 4] = kLowCutFreq;
    r[12 + 5] = 0x50;  // no such parameter
    Seal(r, kParamCount);
    uint32_t seq = 0;
    ASSERT_TRUE(SettingsValid(r.data(), seq));
    const Settings d = DecodeSettings(r.data());
    EXPECT_EQ(d.assign[4], kNoParam);
    EXPECT_EQ(d.assign[5], kNoParam);
    EXPECT_EQ(d.assign[6], kMid);
}
