// daisy/src/params.h: the parameter table, the wire codes, and what a knob or a toggle gives a parameter.
#include "params.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

using namespace dkparam;

TEST(Table, Valid) {
    std::set<std::string> names;
    for (const ParamDef& d : kParams) EXPECT_TRUE(names.insert(d.name).second) << d.name;
    for (int s = 0; s < kControlSlots; ++s) {
        const uint8_t id = FactoryParam(s);
        ASSERT_LT(id, kParamCount) << s;
        EXPECT_TRUE(Accepts(s, id)) << s;
    }
}

TEST(Codes, RoundTrip) {
    for (const ParamDef& d : kParams) {
        if (d.taper == Taper::kChoice) {
            for (uint32_t c = 0; c <= uint32_t(d.max); ++c) EXPECT_EQ(ToCode(d, FromCode(d, c)), c) << d.name;
            EXPECT_EQ(ToCode(d, FromCode(d, uint32_t(d.max) + 1)), uint32_t(d.max)) << d.name;
            EXPECT_EQ(ToCode(d, FromCode(d, kCodeMax)), uint32_t(d.max)) << d.name;
        } else {
            for (uint32_t c : {0u, 1u, 8192u, 16383u}) EXPECT_EQ(ToCode(d, FromCode(d, c)), c) << d.name;
            EXPECT_EQ(FromCode(d, 0), 0.0f);
            EXPECT_EQ(FromCode(d, kCodeMax), 1.0f);
            EXPECT_EQ(FromCode(d, 20000), 1.0f);
        }
    }
}

TEST(Accepts, SwitchesTakeChoicesOnly) {
    EXPECT_FALSE(Accepts(4, kLowCutFreq));
    EXPECT_TRUE(Accepts(4, kMid));
    EXPECT_TRUE(Accepts(0, kMid));
    EXPECT_TRUE(Accepts(0, kLowCutFreq));
    EXPECT_TRUE(Accepts(6, kNoParam));
    EXPECT_FALSE(Accepts(7, kNoParam));
    EXPECT_FALSE(Accepts(-1, kNoParam));
    EXPECT_FALSE(Accepts(0, kParamCount));
}

TEST(FromToggle, ThreeValuesFollowThePosition) {
    const ParamDef& d = kParams[kMid];
    for (uint8_t pos = 0; pos < 3; ++pos) EXPECT_EQ(FromToggle(d, pos), float(pos));
}

TEST(FromToggle, OnOffMiddleIsOff) {
    const ParamDef d{0, "on_off", "On/off", "off,on", "Test", 0.0f, 1.0f, 0.0f, Taper::kChoice, OffEnd::kNone,
                     kNoParam};
    EXPECT_EQ(FromToggle(d, 0), 1.0f);
    EXPECT_EQ(FromToggle(d, 1), 0.0f);
    EXPECT_EQ(FromToggle(d, 2), 1.0f);
}

TEST(FromKnob, ContinuousIsTheClampedPosition) {
    const ParamDef& d = kParams[kLowCutFreq];
    EXPECT_EQ(FromKnob(d, 0.37f, 0.9f), 0.37f);
    EXPECT_EQ(FromKnob(d, -0.01f, 0.9f), 0.0f);
    EXPECT_EQ(FromKnob(d, 1.01f, 0.0f), 1.0f);
}

TEST(FromKnob, ChoiceHysteresis) {
    const ParamDef& d = kParams[kLowCutSlope];
    EXPECT_EQ(FromKnob(d, 0.26f, 0.0f), 0.0f);
    EXPECT_EQ(FromKnob(d, 0.28f, 0.0f), 1.0f);
    EXPECT_EQ(FromKnob(d, 0.23f, 1.0f), 1.0f);
    EXPECT_EQ(FromKnob(d, 0.22f, 1.0f), 0.0f);
    EXPECT_EQ(FromKnob(d, 1.0f, 0.0f), 2.0f);
}

TEST(Quantize, ClampsRoundsAndDefaults) {
    const ParamDef& f = kParams[kHighCutFreq];
    EXPECT_EQ(Quantize(f, 1.5f), 1.0f);
    EXPECT_EQ(Quantize(f, -0.5f), 0.0f);
    EXPECT_EQ(Quantize(f, NAN), f.def);
    const ParamDef& c = kParams[kMid];
    EXPECT_EQ(Quantize(c, 1.6f), 2.0f);
    EXPECT_EQ(Quantize(c, 7.0f), 2.0f);
    EXPECT_EQ(Quantize(c, INFINITY), c.def);
}
