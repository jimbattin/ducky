// daisy/src/param_store.h: the arbitration between the controls and the link's SET and ASSIGN.
#include "param_store.h"

#include <gtest/gtest.h>

using namespace dkparam;

namespace {

// The cuts on Knobs 3-6 and SW1-SW2, eq_enable on SW3.
Settings Layout() {
    Settings s = FactorySettings();
    const uint8_t assign[kControlSlots] = {kHighCutRes,  kHighCutFreq,  kLowCutFreq, kLowCutRes,
                                           kLowCutSlope, kHighCutSlope, kEqEnable};
    for (int c = 0; c < kControlSlots; ++c) s.assign[c] = assign[c];
    return s;
}

// A Layout() store whose controls sit at noon and in the middle, after its first Update.
struct Rig {
    Store store;
    float knobs[kKnobSlots] = {0.5f, 0.5f, 0.5f, 0.5f};
    uint8_t toggles[kSwitchSlots] = {1, 1, 1};

    Rig() {
        store.Init(Layout());
        Update();
    }
    void Update() { store.Update(knobs, toggles); }
    void Set(uint8_t id, float v) { store.Apply({EditKind::kSet, id, 0, v}, knobs); }
    void Assign(uint8_t slot, uint8_t id) { store.Apply({EditKind::kAssign, slot, id, 0.0f}, knobs); }
    bool Held(int slot) const { return (store.HeldBits() >> slot) & 1; }
};

}  // namespace

TEST(Store, FirstUpdateFollowsControls) {
    Store s;
    s.Init(Layout());
    const float knobs[kKnobSlots] = {0.1f, 0.2f, 0.3f, 0.4f};
    const uint8_t toggles[kSwitchSlots] = {2, 0, 1};
    s.Update(knobs, toggles);
    EXPECT_EQ(s.Value(kHighCutRes), 0.1f);
    EXPECT_EQ(s.Value(kHighCutFreq), 0.2f);
    EXPECT_EQ(s.Value(kLowCutFreq), 0.3f);
    EXPECT_EQ(s.Value(kLowCutRes), 0.4f);
    EXPECT_EQ(s.Value(kLowCutSlope), 2.0f);
    EXPECT_EQ(s.Value(kHighCutSlope), 0.0f);
    EXPECT_EQ(s.Value(kEqEnable), 0.0f);
    EXPECT_EQ(s.HeldBits(), 0);
}

TEST(Store, SetHoldsKnobUntilPickup) {
    Rig r;
    r.Set(kLowCutFreq, 0.8f);
    r.Update();
    EXPECT_EQ(r.store.Value(kLowCutFreq), 0.8f);
    EXPECT_EQ(r.store.HeldBits(), 1 << 2);
    r.knobs[2] = 0.52f;
    r.Update();
    EXPECT_EQ(r.store.Value(kLowCutFreq), 0.8f);
    EXPECT_TRUE(r.Held(2));
    r.knobs[2] = 0.54f;
    r.Update();
    EXPECT_EQ(r.store.Value(kLowCutFreq), 0.54f);
    EXPECT_EQ(r.store.HeldBits(), 0);
}

TEST(Store, SetIsQuantized) {
    Rig r;
    r.Set(kHighCutFreq, 1.5f);
    EXPECT_EQ(r.store.Value(kHighCutFreq), 1.0f);
    r.Set(kHighCutSlope, 1.6f);
    EXPECT_EQ(r.store.Value(kHighCutSlope), 2.0f);
}

TEST(Store, ToggleChangeBeatsSet) {
    Rig r;
    r.Set(kLowCutSlope, 0.0f);
    r.Update();
    EXPECT_EQ(r.store.Value(kLowCutSlope), 0.0f);
    EXPECT_TRUE(r.Held(4));
    r.toggles[0] = 2;
    r.Update();
    EXPECT_EQ(r.store.Value(kLowCutSlope), 2.0f);
    EXPECT_FALSE(r.Held(4));
}

TEST(Store, AssignKeepsValueUntilMove) {
    Rig r;
    r.Assign(0, kLowCutSlope);
    r.Update();
    EXPECT_EQ(r.store.Assignment(0), kLowCutSlope);
    EXPECT_EQ(r.store.Value(kLowCutSlope), 1.0f);
    EXPECT_TRUE(r.Held(0));
    r.knobs[0] = 0.9f;
    r.Update();
    EXPECT_EQ(r.store.Value(kLowCutSlope), 2.0f);
    EXPECT_FALSE(r.Held(0));
    EXPECT_TRUE(r.Held(4));  // SW1 is on it too: the knob moved last
    EXPECT_EQ(r.store.Value(kHighCutRes), 0.5f);
}

TEST(Store, AssignSameParameterChangesNothing) {
    Rig r;
    r.Assign(2, kLowCutFreq);
    EXPECT_EQ(r.store.HeldBits(), 0);
}

TEST(Store, LastMovedKnobWins) {
    Rig r;
    r.Assign(0, kLowCutFreq);
    r.Assign(1, kLowCutFreq);
    r.knobs[0] = 0.7f;
    r.Update();
    EXPECT_EQ(r.store.Value(kLowCutFreq), 0.7f);
    EXPECT_FALSE(r.Held(0));
    EXPECT_TRUE(r.Held(1));
    EXPECT_TRUE(r.Held(2));
    r.knobs[1] = 0.54f;
    r.Update();
    EXPECT_EQ(r.store.Value(kLowCutFreq), 0.54f);
    EXPECT_TRUE(r.Held(0));
    EXPECT_FALSE(r.Held(1));
}

// The writing knob's jitter does not re-anchor a held knob: it takes over once it has moved kPickup in all.
TEST(Store, HeldKnobTakesOverWhileTheOtherJitters) {
    Rig r;
    r.Assign(0, kLowCutFreq);
    r.knobs[0] = 0.7f;
    r.Update();
    for (int i = 1; i <= 4; ++i) {
        r.knobs[0] = 0.7f + 0.001f * float(i % 2);
        r.knobs[2] = 0.5f + 0.01f * float(i);
        r.Update();
    }
    EXPECT_EQ(r.store.Value(kLowCutFreq), r.knobs[2]);
    EXPECT_FALSE(r.Held(2));
    EXPECT_TRUE(r.Held(0));
}

TEST(Store, NoneSlotDrivesNothing) {
    Rig r;
    float before[kParamCount];
    for (int i = 0; i < kParamCount; ++i) before[i] = r.store.Value(uint8_t(i));
    r.Assign(2, kNoParam);
    r.knobs[2] = 0.9f;
    r.Update();
    EXPECT_EQ(r.store.Assignment(2), kNoParam);
    EXPECT_EQ(r.store.HeldBits(), 0);
    for (int i = 0; i < kParamCount; ++i) EXPECT_EQ(r.store.Value(uint8_t(i)), before[i]) << i;
}

TEST(Store, ChoiceOnKnobAndSwitchFollowsTheLastMoved) {
    Rig r;
    r.Assign(0, kHighCutSlope);
    r.knobs[0] = 0.0f;
    r.Update();
    EXPECT_EQ(r.store.Value(kHighCutSlope), 0.0f);
    EXPECT_TRUE(r.Held(5));
    r.toggles[1] = 2;
    r.Update();
    EXPECT_EQ(r.store.Value(kHighCutSlope), 2.0f);
    EXPECT_TRUE(r.Held(0));
    EXPECT_FALSE(r.Held(5));
}
