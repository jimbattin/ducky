// The pedal host the firmware is built for, chosen at compile time (daisy/Makefile BOARD=):
//   DUCKY_BOARD_HOTHOUSE: Cleveland Music Co. HotHouse. Knobs 1-6, three on/off/on toggles SW1-SW3, FS1/FS2,
//   LED1/LED2; stereo I/O: mono in and both outputs the same, or a mono effects loop on the rings (docs/LOOP.md).
// Pins on the Daisy Seed (Cleveland Music Co.'s published pinout: github.com/clevelandmusicco/HothouseExamples):
//   knobs 1-6 = D16-D21, FS1 = D25, FS2 = D26, LED1 = D22, LED2 = D23;
//   SW1 up/down = D9/D10, SW2 = D7/D8, SW3 = D5/D6 (checked on the pedal: all three levers down read 0).
// Every switch pin is a libDaisy Switch (momentary, inverted, pull-up): a pin is Pressed() when grounded. A toggle is
// up when its up pin is pressed, down when its down pin is, middle otherwise (an unwired toggle reads middle).
#pragma once

#include <cstddef>
#include <cstdint>

#include "daisy_seed.h"
#include "hid/ctrl.h"
#include "hid/switch.h"

#if !defined(DUCKY_BOARD_HOTHOUSE)
#error "define DUCKY_BOARD_HOTHOUSE (daisy/Makefile BOARD=)"
#endif

namespace dkboard {

constexpr uint8_t kBoardId = 1;  // the Identity Reply's board byte (link.h): 1 = HotHouse
constexpr const char* kName = "hothouse";
constexpr int kKnobs = 6;    // knobs 1-6
constexpr int kToggles = 3;  // SW1-SW3

class Board {
public:
    static constexpr daisy::Pin kLed1 = daisy::seed::D22, kLed2 = daisy::seed::D23;

    daisy::DaisySeed seed;

    // Seed at 480 MHz, audio rate and block size, knob ADC and switches; the knobs filter at the audio callback rate.
    void Init(size_t blockSize, daisy::SaiHandle::Config::SampleRate rate) {
        seed.Configure();
        seed.Init(true);
        seed.SetAudioSampleRate(rate);
        seed.SetAudioBlockSize(blockSize);
        static constexpr daisy::Pin kKnobPins[kKnobs] = {daisy::seed::D16, daisy::seed::D17, daisy::seed::D18,
                                                         daisy::seed::D19, daisy::seed::D20, daisy::seed::D21};
        daisy::AdcChannelConfig cfg[kKnobs];
        for (int i = 0; i < kKnobs; ++i) cfg[i].InitSingle(kKnobPins[i]);
        seed.adc.Init(cfg, kKnobs);
        for (int i = 0; i < kKnobs; ++i) knobs_[i].Init(seed.adc.GetPtr(i), seed.AudioCallbackRate());
        footswitch_[0].Init(daisy::seed::D25);
        footswitch_[1].Init(daisy::seed::D26);
        for (int i = 0; i < kSwitchPins; ++i) switches_[i].Init(kSwitchPinList[i]);
    }

    // Once per audio block.
    void Process() {
        for (daisy::AnalogControl& k : knobs_) k.Process();
        for (daisy::Switch& s : footswitch_) s.Debounce();
        for (daisy::Switch& s : switches_) s.Debounce();
    }

    float Knob(int k) const { return knobs_[k].Value(); }                        // k 0-5: knob 1-6, 0..1
    bool FootswitchRising(int f) const { return footswitch_[f].RisingEdge(); }  // f 0/1: FS1/FS2
    bool FootswitchPressed(int f) const { return footswitch_[f].Pressed(); }
    // Toggle t (0-2: SW1-SW3), unfiltered: 2 up, 0 down, 1 middle.
    uint8_t Toggle(int t) const {
        return switches_[2 * t].Pressed() ? 2 : switches_[2 * t + 1].Pressed() ? 0 : 1;
    }

private:
    // SW1-SW3, up pin then down pin.
    static constexpr int kSwitchPins = 2 * kToggles;
    static constexpr daisy::Pin kSwitchPinList[kSwitchPins] = {daisy::seed::D9, daisy::seed::D10, daisy::seed::D7,
                                                               daisy::seed::D8, daisy::seed::D5,  daisy::seed::D6};

    daisy::AnalogControl knobs_[kKnobs];
    daisy::Switch footswitch_[2];
    daisy::Switch switches_[kSwitchPins];
};

}  // namespace dkboard
