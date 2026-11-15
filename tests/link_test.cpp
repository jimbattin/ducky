// daisy/src/link.h: the wire contract host tools parse (Identity Reply, LOG, PedalCommand) and the host-message
// reassembly.
#include "link.h"

#include <gtest/gtest.h>

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
std::vector<uint8_t> Assemble(SysExAssembler<64>& a, const std::vector<uint8_t>& in) {
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
