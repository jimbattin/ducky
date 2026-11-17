// daisy/src/link.h: the wire contract host tools parse (Identity Reply, LOG, SLOT, RESULT, STATUS, DATA, PARAM, VALUES,
// PedalCommand), the GATE payload, the septet and Pack7 codecs, and the host-message reassembly.
#include "link.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace dklink;

namespace {

// The MIDI bytes of a TxBuffer's USB-MIDI packets (CIN 4: 3 bytes; CIN 5/6/7: 1/2/3 bytes, SysEx end).
std::vector<uint8_t> Unpack(const TxBuffer& t) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 4 <= t.size(); i += 4) {
        const uint8_t cin = t.data()[i] & 0x0F;
        const size_t n = cin == 4 ? 3 : cin >= 5 && cin <= 7 ? cin - 4 : 0;
        for (size_t j = 0; j < n; ++j) out.push_back(t.data()[i + 1 + j]);
    }
    return out;
}

std::vector<uint8_t> IdentityPrefix(uint8_t board) {
    return {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x7D, 0x64, 0x00, board, 0x00, 0x00, 0x00, 0x00, 0x00};
}

// The complete message, or empty if the bytes end without one.
template <size_t N> std::vector<uint8_t> Assemble(SysExAssembler<N>& a, const std::vector<uint8_t>& in) {
    for (uint8_t b : in)
        if (a.Feed(b)) return std::vector<uint8_t>(a.data(), a.data() + a.size());
    return {};
}

}  // namespace

TEST(Identity, ReplyBytes) {
    TxBuffer t;
    ASSERT_TRUE(t.AppendIdentity(1, "abc1234+dirty"));
    std::vector<uint8_t> want = IdentityPrefix(1);
    for (char c : std::string("abc1234+dirty")) want.push_back(uint8_t(c));
    want.push_back(0xF7);
    EXPECT_EQ(Unpack(t), want);
}

TEST(Identity, EmptyBuildIdIsUnknown) {
    TxBuffer t;
    ASSERT_TRUE(t.AppendIdentity(1, ""));
    std::vector<uint8_t> want = IdentityPrefix(1);
    for (char c : std::string("unknown")) want.push_back(uint8_t(c));
    want.push_back(0xF7);
    EXPECT_EQ(Unpack(t), want);
}

TEST(Identity, RequestAcceptsAnyId) {
    for (int id = 0; id <= 0x7F; ++id) {
        const uint8_t m[] = {0xF0, 0x7E, uint8_t(id), 0x06, 0x01, 0xF7};
        EXPECT_TRUE(IsIdentityRequest(m, sizeof m)) << id;
    }
    const uint8_t reply[] = {0xF0, 0x7E, 0x7F, 0x06, 0x02, 0xF7};
    EXPECT_FALSE(IsIdentityRequest(reply, sizeof reply));
}

TEST(PedalCommand, OwnFrameOnly) {
    const uint8_t own[] = {0xF0, 0x7D, 0x64, 0x01, 0x06, 0xF7};
    const uint8_t otherDevice[] = {0xF0, 0x7D, 0x62, 0x02, 0x06, 0xF7};
    const uint8_t otherVersion[] = {0xF0, 0x7D, 0x64, 0x02, 0x06, 0xF7};
    EXPECT_EQ(PedalCommand(own, sizeof own), 0x06);
    EXPECT_EQ(PedalCommand(otherDevice, sizeof otherDevice), -1);
    EXPECT_EQ(PedalCommand(otherVersion, sizeof otherVersion), -1);
}

TEST(Log, Bytes) {
    TxBuffer t;
    ASSERT_TRUE(t.AppendLog("hi"));
    EXPECT_EQ(Unpack(t), (std::vector<uint8_t>{0xF0, 0x7D, 0x64, 0x01, 0x12, 0x68, 0x69, 0xF7}));
}

TEST(Log, NonPrintableBecomesQuestionMark) {
    TxBuffer t;
    ASSERT_TRUE(t.AppendLog("a\tb\x80"));
    EXPECT_EQ(Unpack(t), (std::vector<uint8_t>{0xF0, 0x7D, 0x64, 0x01, 0x12, 'a', '?', 'b', '?', 0xF7}));
}

TEST(Log, TruncatedToMaxLog) {
    TxBuffer t;
    const std::string text(200, 'x');
    ASSERT_TRUE(t.AppendLog(text.c_str()));
    const std::vector<uint8_t> bytes = Unpack(t);
    ASSERT_EQ(bytes.size(), 5 + kMaxLog + 1);
    EXPECT_EQ(bytes[4], kLog);
    EXPECT_EQ(bytes.back(), 0xF7);
}

TEST(SysExAssembler, SkipsRealtimeInsideMessage) {
    SysExAssembler<64> a;
    EXPECT_EQ(Assemble(a, {0xF0, 0x7D, 0x64, 0xF8, 0x01, 0x06, 0xF7}),
              (std::vector<uint8_t>{0xF0, 0x7D, 0x64, 0x01, 0x06, 0xF7}));
}

TEST(SysExAssembler, SecondStartRestarts) {
    SysExAssembler<64> a;
    EXPECT_EQ(Assemble(a, {0xF0, 0x7D, 0x64, 0xF0, 0x7D, 0x64, 0x01, 0x01, 0xF7}),
              (std::vector<uint8_t>{0xF0, 0x7D, 0x64, 0x01, 0x01, 0xF7}));
}

TEST(SysExAssembler, OverlongMessageDropped) {
    SysExAssembler<64> a;
    std::vector<uint8_t> in = {0xF0};
    in.insert(in.end(), 63, 0x00);
    in.push_back(0xF7);  // 65 bytes
    EXPECT_TRUE(Assemble(a, in).empty());
    // The next message still assembles.
    EXPECT_EQ(Assemble(a, {0xF0, 0x7D, 0x64, 0x01, 0x06, 0xF7}).size(), 6u);
}

TEST(SysExAssembler, MaxHostFrameAssembles) {
    SysExAssembler<kMaxHostFrame> a;
    std::vector<uint8_t> in = {0xF0, 0x7D, 0x64, 0x01, kUploadChunk};
    in.insert(in.end(), kMaxHostFrame - 6, 0x11);
    in.push_back(0xF7);
    ASSERT_EQ(in.size(), 307u);
    EXPECT_EQ(Assemble(a, in), in);
}

TEST(Pack7, Vector) {
    const uint8_t in[] = {0x80, 0x01, 0xFF, 0x7F, 0x00, 0x81, 0x42, 0xC3};
    uint8_t out[16];
    const size_t m = Pack7(in, sizeof in, out);
    EXPECT_EQ(std::vector<uint8_t>(out, out + m),
              (std::vector<uint8_t>{0x25, 0x00, 0x01, 0x7F, 0x7F, 0x00, 0x01, 0x42, 0x01, 0x43}));
}

TEST(Pack7, RoundTrip) {
    for (size_t n = 0; n <= 20; ++n) {
        std::vector<uint8_t> in(n);
        for (size_t i = 0; i < n; ++i) in[i] = uint8_t(i * 37 + 0x80 * (i & 1));
        std::vector<uint8_t> packed(Packed7Size(n)), back(n + 1);
        ASSERT_EQ(Pack7(in.data(), n, packed.data()), Packed7Size(n));
        size_t got = 99;
        ASSERT_TRUE(Unpack7(packed.data(), packed.size(), back.data(), got)) << n;
        ASSERT_EQ(got, n);
        back.resize(n);
        EXPECT_EQ(back, in) << n;
    }
}

TEST(Pack7, UnpackRejects) {
    uint8_t out[16];
    size_t n = 0;
    const uint8_t lone[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x00};  // 9 bytes: header without data
    EXPECT_FALSE(Unpack7(lone, sizeof lone, out, n));
    const uint8_t high[] = {0x00, 0x80};
    EXPECT_FALSE(Unpack7(high, sizeof high, out, n));
}

TEST(Septets, Vectors) {
    uint8_t b[5];
    PutSeptets(b, 0xDEADBEEF, 5);
    EXPECT_EQ(std::vector<uint8_t>(b, b + 5), (std::vector<uint8_t>{0x0D, 0x75, 0x36, 0x7D, 0x6F}));
    EXPECT_EQ(GetSeptets(b, 5), 0xDEADBEEFu);
    PutSeptets(b, 16024, 3);
    EXPECT_EQ(std::vector<uint8_t>(b, b + 3), (std::vector<uint8_t>{0x00, 0x7D, 0x18}));
    EXPECT_EQ(GetSeptets(b, 3), 16024u);
}

TEST(Result, Bytes) {
    TxBuffer t;
    ASSERT_TRUE(t.AppendResult(kUploadChunk, Result::kOk, 512));
    EXPECT_EQ(Unpack(t),
              (std::vector<uint8_t>{0xF0, 0x7D, 0x64, 0x01, 0x14, 0x09, 0x00, 0x00, 0x04, 0x00, 0xF7}));
}

TEST(Status, Bytes) {
    TxBuffer t;
    StatusFrame s{500, 503, 3, 3, false, {0, 250, 500, 750, 1000, 123}, {2, 1, 0}, 0b10, 632, 245, 37};
    ASSERT_TRUE(t.AppendStatus(s));
    EXPECT_EQ(Unpack(t), (std::vector<uint8_t>{0xF0, 0x7D, 0x64, 0x01, 0x15, 0x03, 0x74, 0x03, 0x77, 0x00, 0x00,
                                               0x00, 0x03, 0x03, 0x00, 0x00, 0x00, 0x01, 0x7A, 0x03, 0x74, 0x05,
                                               0x6E, 0x07, 0x68, 0x00, 0x7B, 0x02, 0x01, 0x00, 0x02, 0x04, 0x78,
                                               0x01, 0x75, 0x00, 0x25, 0xF7}));
}

TEST(Gate, ParsePayload) {
    const uint8_t p[] = {0x01, 0x37, 0x0F, 0x1E, 0x01, 0x7A, 0x3C};
    dkdsp::GateSettings g{};
    ASSERT_TRUE(dklink::ParseGate(p, sizeof p, g));
    EXPECT_TRUE(g.enabled);
    EXPECT_EQ(g.threshold, 55);
    EXPECT_EQ(g.attack, 15);
    EXPECT_EQ(g.hold, 30);
    EXPECT_EQ(g.release, 250);
    EXPECT_EQ(g.ratio, 60);
    EXPECT_FALSE(dklink::ParseGate(p, 6, g));
    uint8_t q[7];
    std::copy(p, p + 7, q);
    q[1] = 29;
    EXPECT_FALSE(dklink::ParseGate(q, 7, g));
    std::copy(p, p + 7, q);
    q[0] = 2;
    EXPECT_FALSE(dklink::ParseGate(q, 7, g));
}

TEST(Data, Bytes) {
    TxBuffer t;
    const uint8_t d[] = {0x80, 0x01};
    ASSERT_TRUE(t.AppendData(3, 300, 0xDEADBEEF, d, 2));
    EXPECT_EQ(Unpack(t), (std::vector<uint8_t>{0xF0, 0x7D, 0x64, 0x01, 0x16, 0x03, 0x00, 0x02, 0x2C, 0x0D, 0x75,
                                               0x36, 0x7D, 0x6F, 0x01, 0x00, 0x01, 0xF7}));
}

TEST(Data, LengthLimits) {
    TxBuffer t;
    uint8_t d[kReadBytes + 1] = {};
    EXPECT_FALSE(t.AppendData(0, 0, 0, d, 0));
    EXPECT_FALSE(t.AppendData(0, 0, 0, d, kReadBytes + 1));
    EXPECT_EQ(t.size(), 0u);
}

TEST(Slot, NamesSeparatedCutAndPrintable) {
    TxBuffer t;
    const std::string longName(70, 'n');
    ASSERT_TRUE(t.AppendSlot(2, 0, 3, longName.c_str(), "a\tb", nullptr));
    std::vector<uint8_t> want = {0xF0, 0x7D, 0x64, 0x01, kSlot, 2, 0, 3};
    want.insert(want.end(), kMaxName, 'n');
    want.insert(want.end(), {0x00, 'a', '?', 'b', 0x00, 0xF7});
    EXPECT_EQ(Unpack(t), want);
}

// The same bytes parse in editor/src/link/protocol.test.ts ("PARAM parses").
TEST(Param, Bytes) {
    TxBuffer t;
    ASSERT_TRUE(t.AppendParam(dkparam::kParams[dkparam::kLowCutFreq]));
    // id 1, log, off at min, default code 0, min 20.0f (41A00000), max 500.0f (43FA0000).
    std::vector<uint8_t> want = {0xF0, 0x7D, 0x64, 0x01, kParam, 0x01, 0x00, 0x01, 0x00, 0x00,
                                 0x04, 0x0D, 0x00, 0x00, 0x00, 0x04, 0x1F, 0x68, 0x00, 0x00};
    for (char c : std::string("low_cut_freq")) want.push_back(uint8_t(c));
    want.push_back(0x00);
    for (char c : std::string("Low-cut frequency")) want.push_back(uint8_t(c));
    want.push_back(0x00);
    for (char c : std::string("Hz")) want.push_back(uint8_t(c));
    want.push_back(0x00);
    for (char c : std::string("EQ")) want.push_back(uint8_t(c));
    want.push_back(0xF7);
    EXPECT_EQ(Unpack(t), want);
}

TEST(Param, ChoiceDefaultIsItsIndex) {
    TxBuffer t;
    ASSERT_TRUE(t.AppendParam(dkparam::kParams[dkparam::kHighCutSlope]));
    const std::vector<uint8_t> got = Unpack(t);
    // id 15, choice, no off end, default code 1, min 0.0f, max 2.0f (40000000).
    const std::vector<uint8_t> head = {0x0F, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,
                                       0x00, 0x04, 0x00, 0x00, 0x00, 0x00};
    ASSERT_GE(got.size(), 5 + head.size());
    EXPECT_EQ(std::vector<uint8_t>(got.begin() + 5, got.begin() + 5 + head.size()), head);
}

// The same bytes parse in editor/src/link/protocol.test.ts ("PARAM parses a linear row").
TEST(Param, LinearRow) {
    TxBuffer t;
    ASSERT_TRUE(t.AppendParam(dkparam::kParams[dkparam::kBell1Gain]));
    const std::vector<uint8_t> got = Unpack(t);
    // id 4, linear, no off end, default code 8192 (0 dB), min -12.0f (C1400000), max 12.0f (41400000).
    const std::vector<uint8_t> head = {0x04, 0x02, 0x00, 0x40, 0x00, 0x0C, 0x0A, 0x00, 0x00,
                                       0x00, 0x04, 0x0A, 0x00, 0x00, 0x00};
    ASSERT_GE(got.size(), 5 + head.size());
    EXPECT_EQ(std::vector<uint8_t>(got.begin() + 5, got.begin() + 5 + head.size()), head);
}

// The same bytes parse in editor/src/link/protocol.test.ts ("VALUES parses").
TEST(Values, Bytes) {
    ValuesFrame v{};
    for (int c = 0; c < dkparam::kControlSlots; ++c) v.assign[c] = dkparam::kNoParam;
    v.held = 0b0000101;
    const uint16_t codes[dkparam::kParamCount] = {1, 0, 8192, 1, 8192, 16383, 0,     8192, 8192, 0,    8192, 8192,
                                                  0, 16383, 8192, 1, 0, 12287, 1,    16383, 0,    8192, 8192};
    std::copy(codes, codes + dkparam::kParamCount, v.code);
    TxBuffer t;
    ASSERT_TRUE(t.AppendValues(v));
    EXPECT_EQ(Unpack(t),
              (std::vector<uint8_t>{0xF0, 0x7D, 0x64, 0x01, kValues, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F, 0x7F,
                                    0x05, 0x17, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00, 0x00, 0x01, 0x40, 0x00,
                                    0x7F, 0x7F, 0x00, 0x00, 0x40, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40, 0x00,
                                    0x40, 0x00, 0x00, 0x00, 0x7F, 0x7F, 0x40, 0x00, 0x00, 0x01, 0x00, 0x00,
                                    0x5F, 0x7F, 0x00, 0x01, 0x7F, 0x7F, 0x00, 0x00, 0x40, 0x00, 0x40, 0x00,
                                    0xF7}));
}
