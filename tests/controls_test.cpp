// daisy/src/controls.h: the toggle hysteresis.
#include "controls.h"

#include <gtest/gtest.h>

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
