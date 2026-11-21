// daisy/src/midi.h: CC extraction from a raw MIDI stream (running status, other messages, realtime, SysEx) and the
// expression channel match.
#include "midi.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <tuple>
#include <vector>

using namespace dkmidi;

namespace {

using CcT = std::tuple<int, int, int>;  // channel, number, value

std::vector<CcT> Parse(std::initializer_list<uint8_t> bytes) {
    CcParser p;
    std::vector<CcT> out;
    Cc c;
    for (uint8_t b : bytes)
        if (p.Feed(b, c)) out.emplace_back(c.channel, c.number, c.value);
    return out;
}

}  // namespace

TEST(CcParser, RunningStatus) {
    EXPECT_EQ(Parse({0xB2, 0x07, 0x64, 0x07, 0x65}), (std::vector<CcT>{{2, 7, 100}, {2, 7, 101}}));
}

TEST(CcParser, NoteDataIsNotCc) {
    EXPECT_EQ(Parse({0x90, 0x3C, 0x64, 0xB0, 0x0E, 0x40}), (std::vector<CcT>{{0, 14, 64}}));
}

TEST(CcParser, ProgramChangeTakesOneByte) {
    EXPECT_EQ(Parse({0xC0, 0x05, 0xB0, 0x0E, 0x40}), (std::vector<CcT>{{0, 14, 64}}));
    EXPECT_TRUE(Parse({0xC0, 0x05, 0x0E, 0x40}).empty());  // running status of a program change
}

TEST(CcParser, RealtimeInsideMessage) { EXPECT_EQ(Parse({0xB0, 0xF8, 0x0E, 0x40}), (std::vector<CcT>{{0, 14, 64}})); }

TEST(CcParser, SysExEndsRunningStatus) {
    EXPECT_EQ(Parse({0xB0, 0x0E, 0x40, 0xF0, 0x7D, 0x0E, 0x40, 0xF7, 0x0E, 0x40}), (std::vector<CcT>{{0, 14, 64}}));
}

TEST(CcParser, SystemCommonSwallowsItsData) {
    EXPECT_TRUE(Parse({0xB0, 0xF2, 0x0E, 0x40, 0x0E, 0x40}).empty());  // song position: 2 bytes, then no status
    EXPECT_TRUE(Parse({0xB0, 0xF3, 0x0E, 0x0E, 0x40}).empty());
}

TEST(ChannelMatches, OmniAndOne) {
    EXPECT_TRUE(ChannelMatches(0, 5));
    EXPECT_TRUE(ChannelMatches(1, 0));
    EXPECT_FALSE(ChannelMatches(1, 1));
    EXPECT_TRUE(ChannelMatches(16, 15));
}
