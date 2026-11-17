// The live parameter values, the control slots' assignments and the arbitration between the controls and the link
// (docs/MIDI.md "Controls"). The audio callback owns it: Update once per block, Apply for each edit the main loop
// queued. Host-portable (no libDaisy, no allocation), covered by tests/param_store_test.cpp.
//
// - A control writes its parameter: a knob every block, a toggle when it changes position. The first Update after
//   Init writes every assigned parameter from its control (what you see is what you get), replacing the saved values
//   of parameters on a control.
// - A link SET holds every slot on its parameter: a held knob slot leaves the parameter alone until its knob has
//   moved more than kPickup from where it was when held; a held switch slot until its toggle changes position.
// - A toggle that changes position always writes its parameter.
// - A parameter on more than one slot follows the control moved last: a slot writing it holds the others.
// - A newly assigned slot is held: the parameter keeps its value until the control moves.
#pragma once

#include <cmath>
#include <cstdint>

#include "memory.h"
#include "params.h"
#include "settings.h"

namespace dkparam {

enum class EditKind : uint8_t { kSet, kAssign };
// kSet: a = the parameter id (< kParamCount), value. kAssign: a = the control slot, b = the id or kNoParam (a pair
// Accepts() allows).
struct Edit {
    EditKind kind;
    uint8_t a, b;
    float value;
};

class Store {
public:
    // The values and assignments of s; nothing held, no toggle seen yet.
    void Init(const Settings& s) {
        for (int i = 0; i < kParamCount; ++i) value_[i] = s.value[i];
        for (int c = 0; c < kControlSlots; ++c) assign_[c] = s.assign[c];
        held_ = 0;
        for (float& a : anchor_) a = 0.0f;
        for (uint8_t& t : last_) t = 0xFF;
        FindPeers();
    }

    // knobs: Knobs 3-6 as of now.
    DK_ITCM void Apply(const Edit& e, const float (&knobs)[kKnobSlots]) {
        if (e.kind == EditKind::kSet) {
            value_[e.a] = Quantize(kParams[e.a], e.value);
            for (int c = 0; c < kControlSlots; ++c)
                if (assign_[c] == e.a) Hold(c, knobs);
            return;
        }
        if (assign_[e.a] == e.b) return;
        assign_[e.a] = e.b;
        FindPeers();
        if (e.b != kNoParam)
            Hold(e.a, knobs);
        else
            held_ &= uint8_t(~(1u << e.a));
    }

    // Once per block: Knobs 3-6 (0..1) and SW1-SW3 (filtered: 0 down, 1 middle, 2 up).
    DK_ITCM void Update(const float (&knobs)[kKnobSlots], const uint8_t (&toggles)[kSwitchSlots]) {
        for (int t = 0; t < kSwitchSlots; ++t) {
            if (toggles[t] == last_[t]) continue;
            const int c = kKnobSlots + t;
            last_[t] = toggles[t];
            held_ &= uint8_t(~(1u << c));
            const uint8_t id = assign_[c];
            if (id != kNoParam) Write(id, FromToggle(kParams[id], toggles[t]), c, knobs);
        }
        for (int c = 0; c < kKnobSlots; ++c) {
            const uint8_t id = assign_[c];
            if (id == kNoParam) continue;
            if (held_ & (1u << c)) {
                if (std::fabs(knobs[c] - anchor_[c]) <= kPickup) continue;
                held_ &= uint8_t(~(1u << c));
            }
            // No test for a changed value: a float compare costs more here than the write (docs/M7_PERFORMANCE.md).
            Write(id, FromKnob(kParams[id], knobs[c], value_[id]), c, knobs);
        }
    }

    const float* Values() const { return value_; }
    float Value(uint8_t id) const { return value_[id]; }
    uint8_t Assignment(int slot) const { return assign_[slot]; }
    // Bit c: control slot c is held (its parameter waits for the control to move).
    uint8_t HeldBits() const { return held_; }

private:
    // Slot c held; a knob slot is anchored at its knob's position.
    DK_ITCM void Hold(int c, const float (&knobs)[kKnobSlots]) {
        held_ |= uint8_t(1u << c);
        if (c < kKnobSlots) anchor_[c] = knobs[c];
    }
    // Slot `by` writes v to id: every other slot on id is held. One held already keeps its anchor: re-anchoring it
    // at every write would let the writing knob's jitter keep it held however far its own knob moves. A knob writes
    // on most blocks (its reading wanders), so the usual case, no other slot on id, is one test.
    DK_ITCM void Write(uint8_t id, float v, int by, const float (&knobs)[kKnobSlots]) {
        value_[id] = v;
        const unsigned todo = peers_[by] & ~unsigned(held_);
        if (todo == 0) return;
        for (int c = 0; c < kControlSlots; ++c)
            if (todo & (1u << c)) Hold(c, knobs);
    }
    // peers_ from assign_.
    DK_ITCM void FindPeers() {
        for (int c = 0; c < kControlSlots; ++c) {
            uint8_t bits = 0;
            for (int o = 0; o < kControlSlots; ++o)
                if (o != c && assign_[c] != kNoParam && assign_[o] == assign_[c]) bits |= uint8_t(1u << o);
            peers_[c] = bits;
        }
    }

    float value_[kParamCount];
    uint8_t assign_[kControlSlots];  // a parameter id or kNoParam
    uint8_t peers_[kControlSlots];   // bit o: slot o (another one) drives the same parameter
    uint8_t held_;                   // bit c: slot c is held
    float anchor_[kKnobSlots];       // a held knob slot's knob position when it was held
    uint8_t last_[kSwitchSlots];     // each toggle's position at the last Update; 0xFF before the first
};

}  // namespace dkparam
