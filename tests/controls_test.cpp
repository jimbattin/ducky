// daisy/src/controls.h: the toggle hysteresis and LED2's blink pattern.
#include "controls.h"

#include <gtest/gtest.h>

#include <vector>

using namespace dkctl;

TEST(ToggleFilter, AcceptsExactlyAtStableCount) {
    ToggleFilter f;
    f.Init(0);
    for (int i = 0; i < kToggleStableBlocks - 1; ++i) EXPECT_EQ(f.Process(1), 0);
    EXPECT_EQ(f.Process(1), 1);
}

TEST(ToggleFilter, UpToDownNeverReportsMiddle) {
    ToggleFilter f;
    f.Init(2);
    for (int i = 0; i < 10; ++i) EXPECT_EQ(f.Process(1), 2);
    for (int i = 0; i < kToggleStableBlocks - 1; ++i) EXPECT_EQ(f.Process(0), 2);
    EXPECT_EQ(f.Process(0), 0);
}

TEST(ToggleFilter, BounceRestartsCount) {
    ToggleFilter f;
    f.Init(2);
    for (int i = 0; i < kToggleStableBlocks - 1; ++i) f.Process(0);
    EXPECT_EQ(f.Process(2), 2);
    for (int i = 0; i < kToggleStableBlocks - 1; ++i) EXPECT_EQ(f.Process(0), 2);
    EXPECT_EQ(f.Process(0), 0);
}

namespace {

// The lengths of the alternating runs of Tick() over ticks blocks, starting with an on run (0 if it starts off).
std::vector<int> Runs(Blinker& b, int ticks) {
    std::vector<int> runs{0};
    bool on = true;
    for (int i = 0; i < ticks; ++i) {
        if (b.Tick() != on) {
            on = !on;
            runs.push_back(0);
        }
        ++runs.back();
    }
    return runs;
}

}  // namespace

TEST(Blinker, ThreeBlinksThenPauseThenRepeat) {
    Blinker b;
    b.Start(3);
    const std::vector<int> want{kBlinkOnBlocks, kBlinkOffBlocks, kBlinkOnBlocks, kBlinkOffBlocks,
                                kBlinkOnBlocks, kBlinkOffBlocks + kBlinkPauseBlocks,
                                kBlinkOnBlocks, kBlinkOffBlocks, kBlinkOnBlocks, kBlinkOffBlocks,
                                kBlinkOnBlocks, kBlinkOffBlocks + kBlinkPauseBlocks,
                                kBlinkOnBlocks};
    const int cycle = 3 * (kBlinkOnBlocks + kBlinkOffBlocks) + kBlinkPauseBlocks;
    EXPECT_EQ(Runs(b, 2 * cycle + kBlinkOnBlocks), want);
}

TEST(Blinker, StartRestartsMidPattern) {
    Blinker b;
    b.Start(3);
    for (int i = 0; i < kBlinkOnBlocks + kBlinkOffBlocks + 40; ++i) b.Tick();  // into the second blink
    b.Start(2);
    const std::vector<int> want{kBlinkOnBlocks, kBlinkOffBlocks, kBlinkOnBlocks, kBlinkOffBlocks + 500};
    EXPECT_EQ(Runs(b, 2 * (kBlinkOnBlocks + kBlinkOffBlocks) + 500), want);
}

TEST(Blinker, IdleAndZeroAreOff) {
    Blinker b;
    EXPECT_FALSE(b.Tick());
    b.Start(0);
    EXPECT_FALSE(b.Tick());
}

TEST(Blinker, ZeroStopsRepeats) {
    Blinker b;
    b.Start(1);
    b.Tick();
    b.Start(0);
    for (int i = 0; i < 2 * (kBlinkOnBlocks + kBlinkOffBlocks + kBlinkPauseBlocks); ++i) EXPECT_FALSE(b.Tick());
}
