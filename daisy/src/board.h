// The pedal host the firmware is built for, chosen at compile time (daisy/Makefile BOARD=):
//   DUCKY_BOARD_HOTHOUSE: Cleveland Music Co. HotHouse. Knobs 1-6, three on/off/on toggles SW1-SW3, FS1/FS2,
//   LED1/LED2; stereo I/O: mono in and both outputs the same, or a mono effects loop on the rings (docs/LOOP.md).
//   DUCKY_BOARD_BURNINGBUNGALOW: BurningBungalow, a HotHouse fork: the same pins for all of the above, plus an
//   expression pedal jack and a TRS MIDI IN (and OUT, unused).
// Pins on the Daisy Seed (Cleveland Music Co.'s published pinout: github.com/clevelandmusicco/HothouseExamples):
//   knobs 1-6 = D16-D21, FS1 = D25, FS2 = D26, LED1 = D22, LED2 = D23;
//   SW1 up/down = D9/D10, SW2 = D7/D8, SW3 = D5/D6 (checked on the pedal: all three levers down read 0).
// A BurningBungalow's extra pins: the expression jack D15 (seed::A0, the ADC channel after the six knobs), MIDI IN
// D14 (USART1 RX, midi_in.h), MIDI OUT D13 (USART1 TX: idle, nothing is sent).
// Every switch pin is a libDaisy Switch (momentary, inverted, pull-up): a pin is Pressed() when grounded. A toggle is
// up when its up pin is pressed, down when its down pin is, middle otherwise (an unwired toggle reads middle).
#pragma once

#include <cstddef>
#include <cstdint>

#include "daisy_seed.h"
#include "hid/ctrl.h"
#include "hid/switch.h"

#if defined(DUCKY_BOARD_HOTHOUSE) + defined(DUCKY_BOARD_BURNINGBUNGALOW) != 1
#error "define one of DUCKY_BOARD_HOTHOUSE, DUCKY_BOARD_BURNINGBUNGALOW (daisy/Makefile BOARD=)"
#endif

namespace dkboard {

// kBoardId: the Identity Reply's board byte (link.h): 1 = HotHouse, 2 = BurningBungalow.
#if defined(DUCKY_BOARD_BURNINGBUNGALOW)
constexpr uint8_t kBoardId = 2;
constexpr const char* kName = "burningbungalow";
constexpr bool kHasPedal = true;   // the expression pedal jack
constexpr bool kHasMidiIn = true;  // the TRS MIDI IN
#else
constexpr uint8_t kBoardId = 1;
constexpr const char* kName = "hothouse";
constexpr bool kHasPedal = false;
constexpr bool kHasMidiIn = false;
#endif
constexpr int kKnobs = 6;    // knobs 1-6
constexpr int kToggles = 3;  // SW1-SW3

class Board {
public:
    static constexpr daisy::Pin kLed1 = daisy::seed::D22, kLed2 = daisy::seed::D23;

    daisy::DaisySeed seed;

    // Seed at 480 MHz, audio rate and block size, knob (and expression jack) ADC and switches; the knobs and the jack
    // filter at the audio callback rate.
    void Init(size_t blockSize, daisy::SaiHandle::Config::SampleRate rate) {
        seed.Configure();
        seed.Init(true);
        seed.SetAudioSampleRate(rate);
        seed.SetAudioBlockSize(blockSize);
        static constexpr daisy::Pin kKnobPins[kKnobs] = {daisy::seed::D16, daisy::seed::D17, daisy::seed::D18,
                                                         daisy::seed::D19, daisy::seed::D20, daisy::seed::D21};
        constexpr int kAdcChannels = kKnobs + (kHasPedal ? 1 : 0);  // the knobs, then the expression jack
        daisy::AdcChannelConfig cfg[kAdcChannels];
        for (int i = 0; i < kKnobs; ++i) cfg[i].InitSingle(kKnobPins[i]);
        if constexpr (kHasPedal) cfg[kAdcChannels - 1].InitSingle(daisy::seed::A0);
        seed.adc.Init(cfg, kAdcChannels);
        for (int i = 0; i < kKnobs; ++i) knobs_[i].Init(seed.adc.GetPtr(i), seed.AudioCallbackRate());
        if constexpr (kHasPedal) pedal_.Init(seed.adc.GetPtr(kAdcChannels - 1), seed.AudioCallbackRate());
        footswitch_[0].Init(daisy::seed::D25);
        footswitch_[1].Init(daisy::seed::D26);
        for (int i = 0; i < kSwitchPins; ++i) switches_[i].Init(kSwitchPinList[i]);
    }

    // Once per audio block.
    void Process() {
        for (daisy::AnalogControl& k : knobs_) k.Process();
        if constexpr (kHasPedal) pedal_.Process();
        for (daisy::Switch& s : footswitch_) s.Debounce();
        for (daisy::Switch& s : switches_) s.Debounce();
    }

    float Knob(int k) const { return knobs_[k].Value(); }                        // k 0-5: knob 1-6, 0..1
    float Pedal() const { return kHasPedal ? pedal_.Value() : -1.0f; }         // the jack's raw reading 0..1; -1: none
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
    daisy::AnalogControl pedal_;  // the expression jack, initialised only on a board with one (kHasPedal)
    daisy::Switch footswitch_[2];
    daisy::Switch switches_[kSwitchPins];
};

}  // namespace dkboard
