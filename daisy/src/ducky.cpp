// Ducky pedal firmware (Cleveland Music Co. HotHouse, board.h). M7: the left input goes through Knob 1's input trim,
// the current preset's A2-Lite model (dsp/a2lite.h) and cab IR when it has one (dsp/convolver.h), the EQ
// (dsp/filters.h: low-cut on Knobs 5-6 and SW1, the SW3 mid, high-cut on Knobs 4-3 and SW2), and Knob 2's output
// volume times the model's level match and the preset's noise gate (dsp/gain.h, dsp/noise_gate.h: the gate listens to
// the input before the trim), then FS1's mute ramp and the preset switch's fade, and is copied to both outputs (dual
// mono). Presets live in QSPI (preset.h: 8 slots, written by tools/preset_bank.ts over DFU, or slot by slot over the
// link: docs/MIDI.md) and are parsed into SDRAM at boot and after every slot change; corrupt or empty slots are
// skipped. With no valid slot the pedal plays dry (trim, EQ, volume, mute; no gate). If the IR convolver's FFT does
// not initialise, audio never starts: the pedal serves USB (DFU included) and logs "ir fft" once a second.
//
// Two threads of control:
// - AudioCallback (the audio DMA interrupt, 48 kHz, 24-sample blocks): reads the controls (30 ms toggle hysteresis),
//   toggles the mute on FS1, counts FS2 presses, smooths knobs 1-2, runs the gate's detector, the trim, the engine, the
//   IR, the EQ and the output gain, runs the mute ramp and the preset fade, drives LED1 and LED2's blinks and measures
//   the input and output peaks, the gate's level and cut, the load and the underruns.
// - The main loop: USB (usb_link.h: Identity, HELLO, DFU; the slot commands in onCommand, their replies and the
//   STATUS stream in serveReplies), the preset switch (servePresets), the state record (writeState), the slot writes
//   (writeSlot) and the once-a-second log lines.
// A preset switch: the main loop raises gSwapReq; the callback fades out (gSwapFade), then parks (writes silence,
// touches neither the engine nor the IR) and acks with gSwapParked; the main loop loads the next preset into the engine
// and the IR and publishes its gains with gSwapLoaded; the callback takes them and fades in.
// They share only atomics, each with one writer:
// - callback -> main loop (relaxed): gKnob, gToggle, gFsHeld, gMutedPub, gUnderruns, gPeak, gOutPeak, gLoadAvg,
//   gLoadMax, gFs2Presses, gGateLevel, gGateGain; gSwapParked (release/acquire);
// - main loop -> callback (relaxed): gPeakReset, gLoadReset, gBlinkCount, gGateWord (publishGate), gGateMeterReset;
//   gSwapReq, gSwapLoaded (publishing gNextLevel, gNextInCal, gNextIrOn, gNextModelOn) and gBlinkReq
//   (release/acquire).
// Memory (memory.h): AudioCallback runs from ITCM; the engine, the EQ and the callback's own state live in DTCM, the
// IR convolver (66 KB of spectra, streamed through the D-cache every block) in AXI SRAM, which the bench measured 24%
// faster for it than D2 SRAM (docs/FILTERS.md), the parsed presets in SDRAM. FPSCR.FZ is on (denormals flush to zero).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "board.h"
#include "controls.h"
#include "dsp/a2lite.h"
#include "dsp/blocks.h"
#include "dsp/convolver.h"
#include "dsp/filters.h"
#include "dsp/gain.h"
#include "dsp/mute.h"
#include "dsp/noise_gate.h"
#include "firmware.h"
#include "ir.h"
#include "memory.h"
#include "model.h"
#include "preset.h"
#include "upload.h"
#include "usb_link.h"

using namespace daisy;

namespace {

namespace ps = dkpreset;

dkboard::Board hw;

// Audio callback only (after boot).
DK_DTCM Led gLed1, gLed2;  // Update once per block (Init at the block rate)
DK_DTCM CpuLoadMeter gLoad;
DK_DTCM uint32_t gBlockTicks;  // System::GetTick() ticks in one block period (set before StartAudio)
DK_DTCM dkctl::ToggleFilter gToggleFilter[3];
DK_DTCM dkdsp::MuteRamp gMute;
DK_DTCM_BSS dkdsp::A2Lite gEngine;  // the main loop loads it while the callback is parked (and before StartAudio)
DK_DTCM float gLevel = 1.0f;  // the current model's level match
DK_DTCM float gInCal = 1.0f;  // the current model's input calibration
DK_DTCM dkdsp::Smoother gInKnob, gOutKnob;  // knobs 1 and 2
DK_DTCM dkdsp::GainRamp gInGain, gOutGain;  // Knob 1's trim times gInCal; Knob 2's volume times gLevel
DK_DTCM float gPeakAcc;        // input peak since the main loop last read gPeak
DK_DTCM float gOutPeakAcc;     // output peak since the main loop last read gOutPeak
DK_DTCM bool gMuted = false;   // FS1's state; not persisted
dkdsp::IrConvolver gIr;        // AXI SRAM; Init before StartAudio; the main loop loads it as it does gEngine
DK_DTCM bool gIrOn = false;    // the current preset has an IR
DK_DTCM bool gModelOn = false;  // a preset is loaded: false plays dry (taken from gNextModelOn when fading in)
DK_DTCM dkdsp::Eq gEq;
// The preset switch: kPlaying -> (gSwapReq) kFadingOut -> (gSwapFade silent) kParked -> (gSwapLoaded) kPlaying.
enum class Swap : uint8_t { kPlaying, kFadingOut, kParked };
DK_DTCM Swap gSwap = Swap::kPlaying;
DK_DTCM dkdsp::MuteRamp gSwapFade;
DK_DTCM uint32_t gSwapSeen = 0;   // the last gSwapReq taken
DK_DTCM uint32_t gBlinkSeen = 0;  // the last gBlinkReq taken
DK_DTCM dkctl::Blinker gBlinker;
DK_DTCM dkdsp::NoiseGate gGate;                             // the playing preset's gate (gGateWord)
DK_DTCM uint32_t gGateSeen[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};  // the last gGateWord pair taken
DK_DTCM float gGateLevelAcc = 0.0f, gGateGainAcc = 1.0f;    // the gate meter since the last STATUS

// Shared with the main loop: in SRAM (AXI).
// Callback -> main loop.
std::atomic<float> gKnob[6];               // knobs 1-6, 0..1
std::atomic<uint8_t> gToggle[3]{1, 1, 1};  // SW1-SW3 filtered: 0 down, 1 middle, 2 up
std::atomic<uint8_t> gFsHeld{0};           // bit 0 FS1, bit 1 FS2
std::atomic<bool> gMutedPub{false};        // gMuted
std::atomic<uint32_t> gUnderruns{0};       // blocks since boot whose callback ran longer than one block period
std::atomic<float> gPeak{0.0f};            // gPeakAcc, published every block
std::atomic<float> gOutPeak{0.0f};         // gOutPeakAcc, published every block
std::atomic<float> gLoadAvg{0.0f}, gLoadMax{0.0f};  // gLoad's, published every block
std::atomic<float> gGateLevel{0.0f};       // gGateLevelAcc, published every block
std::atomic<float> gGateGain{1.0f};        // gGateGainAcc, published every block
std::atomic<uint32_t> gFs2Presses{0};      // FS2 presses since boot
std::atomic<uint32_t> gSwapParked{0};      // the gSwapReq the callback has parked for (release)
// Main loop -> callback.
std::atomic<bool> gPeakReset{false};  // restart gPeakAcc and gOutPeakAcc
std::atomic<bool> gLoadReset{false};  // restart gLoad
std::atomic<uint32_t> gSwapReq{0};     // a new value asks the callback to fade out and park (release)
std::atomic<uint32_t> gSwapLoaded{0};  // gSwapReq's value once the next preset is loaded (release): fade in
std::atomic<uint32_t> gBlinkReq{0};    // a new value starts gBlinkCount blinks on LED2 (release)
std::atomic<uint8_t> gBlinkCount{0};
// The gate's settings as preset.h's 8-byte block, two little-endian words (publishGate). A torn pair mixes two valid
// settings for one block; the reserved byte is 0, so no published pair equals gGateSeen's initial value.
std::atomic<uint32_t> gGateWord[2];
std::atomic<bool> gGateMeterReset{false};  // restart gGateLevelAcc and gGateGainAcc
float gNextLevel = 1.0f, gNextInCal = 1.0f;  // the loaded preset's gains, IR and model, published by gSwapLoaded
bool gNextIrOn = false, gNextModelOn = false;

// Main loop only.
DK_SDRAM ps::Preset gPresets[ps::kSlots];  // valid where gFilled
ps::Status gSlotStatus[ps::kSlots];
bool gFilled[ps::kSlots];
int gCur = -1, gTarget = -1;  // the loaded and the wanted slot; -1 = dry
bool gSwapping = false;       // gSwapReq raised, the load not yet done
uint32_t gReq = 0, gFs2Seen = 0, gBlinkSeq = 0;
int gStateIndex = -1, gStateSlot = -1;  // the state sector holding the newest record and its slot; -1 = none
uint32_t gStateSeq = 0;
bool gPersistPending = false;  // a switch not yet written to the state record
uint32_t gSwitchedAt = 0;      // System::GetNow() at the last switch
constexpr uint32_t kPersistDelayMs = 2000;  // the state record is written once switching has settled this long
bool gReload = false;          // reload the target slot (its flash changed) even when it is already loaded

// The link's slot commands (onCommand, serveReplies). Main loop only.
DK_SDRAM uint8_t gUploadBuf[ps::kSizeWithIr];
dklink::Upload gUpload(gUploadBuf);
DK_SDRAM uint8_t gScratch[2][ps::kSizeWithIr];  // RENAME's and SWAP's copies of slot images
DK_SDRAM ps::Preset gCheck;                     // COMMIT's validation target
int gListNext = -1;                             // the next SLOT to send for LIST; 8: its RESULT; -1: none
bool gResultDue = false;                        // a RESULT waiting for room in Tx()
uint8_t gResultCmd = 0;
dklink::Result gResultCode = dklink::Result::kOk;
uint32_t gResultValue = 0;
uint8_t gReadBuf[dklink::kReadBytes];  // READ's bytes, sent as DATA before its RESULT
bool gReadDue = false;
uint8_t gReadSlot = 0;
uint32_t gReadOff = 0;
size_t gReadLen = 0;
bool gWatch = false;  // STATUS every kWatchPeriodMs
uint32_t gWatchAt = 0;
uint16_t gLoadAvgPm = 0, gLoadMaxPm = 0;  // the last `load` line's values

const uint8_t* mapped(uint32_t off) { return reinterpret_cast<const uint8_t*>(ps::kQspiBase + off); }
uint32_t slotOff(int k) { return ps::kBankOffset + uint32_t(k) * ps::kSlotBytes; }

float levelOf(const dkmodel::Model& m) {
    return dkdsp::LevelMatchGain((m.flags & dkmodel::kFlagHasLoudness) != 0, m.loudness);
}
float calOf(const dkmodel::Model& m) {
    return dkdsp::InputCalibrationGain((m.flags & dkmodel::kFlagHasInputLevel) != 0, m.input_level_dbu);
}

// Hands g to the callback, which applies it at its next block. Main loop (or before StartAudio).
void publishGate(const dkdsp::GateSettings& g) {
    uint8_t b[ps::kGateSize];
    ps::PutGate(b, g);
    for (int w = 0; w < 2; ++w) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(b[4 * w + i]) << (8 * i);
        gGateWord[w].store(v, std::memory_order_relaxed);
    }
}

// The engine and the IR from gPresets[slot], its gains, IR and model state into gNext*; slot < 0 (dry) only sets
// gNext* to no model, no IR, unity gains. Only while the callback is parked or before StartAudio.
void loadPreset(int slot) {
    publishGate(slot < 0 ? dkdsp::kGateOff : gPresets[slot].gate);
    if (slot < 0) {
        gNextModelOn = gNextIrOn = false;
        gNextLevel = gNextInCal = 1.0f;
        return;
    }
    const ps::Preset& p = gPresets[slot];
    gEngine.Load(p.model.weights, p.model.head_scale);
    if (p.has_ir) gIr.Load(p.ir.taps);
    gNextModelOn = true;
    gNextIrOn = p.has_ir;
    gNextLevel = levelOf(p.model);
    gNextInCal = calOf(p.model);
}

// The codec's sample word in bits.
unsigned bitDepth(SaiHandle::Config::BitDepth d) {
    switch (d) {
        case SaiHandle::Config::BitDepth::SAI_16BIT: return 16;
        case SaiHandle::Config::BitDepth::SAI_24BIT: return 24;
        case SaiHandle::Config::BitDepth::SAI_32BIT: return 32;
    }
    return 0;
}

const char* SeedName(DaisySeed::BoardVersion v) {
    switch (v) {
        case DaisySeed::BoardVersion::DAISY_SEED: return "rev4-ak4556";
        case DaisySeed::BoardVersion::DAISY_SEED_1_1: return "1.1-wm8731";
        case DaisySeed::BoardVersion::DAISY_SEED_2_DFM: return "2dfm-pcm3060";
        case DaisySeed::BoardVersion::DAISY_SEED_1_2: return "1.2-pcm3060";
        case DaisySeed::BoardVersion::DAISY_SEED_3: return "3-tac5242";
    }
    return "unknown";
}

// "+D.d" / "-D.d", clamped to +-999.9: newlib-nano's printf has no %f.
void tenthsDb(char (&buf)[12], float db) {
    const long t = std::clamp(lroundf(db * 10.0f), -9999L, 9999L);
    const unsigned a = unsigned(t < 0 ? -t : t);
    snprintf(buf, sizeof buf, "%c%u.%u", t < 0 ? '-' : '+', a / 10, a % 10);
}

// A model's loudness, level match, input level, input calibration and name; "model none" for nullptr. The name is
// last: Log truncates at 120 characters and a name can take 63.
void logModel(const dkmodel::Model* m) {
    if (m == nullptr) {
        dkusb::Log("model none");
        return;
    }
    const bool hasLoudness = (m->flags & dkmodel::kFlagHasLoudness) != 0;
    const bool hasInput = (m->flags & dkmodel::kFlagHasInputLevel) != 0;
    char loudness[12] = "none", level[12], input[12] = "none", cal[12];
    if (hasLoudness) tenthsDb(loudness, m->loudness);
    if (hasInput) tenthsDb(input, m->input_level_dbu);
    tenthsDb(level, 20.0f * log10f(levelOf(*m)));
    tenthsDb(cal, 20.0f * log10f(calOf(*m)));
    dkusb::Log("model loudness %s%s level %s dB input %s%s cal %s dB \"%s\"", loudness, hasLoudness ? " dB" : "",
               level, input, hasInput ? " dBu" : "", cal, m->name);
}

// An IR's tap count, source rate, normalisation gain and name; "ir none" for nullptr. The name is last, as in logModel.
void logIr(const dkir::Ir* ir) {
    if (ir == nullptr) {
        dkusb::Log("ir none");
        return;
    }
    char gain[12];
    tenthsDb(gain, 20.0f * log10f(ir->gain));
    dkusb::Log("ir taps %u src %u Hz gain %s dB \"%s\"", unsigned(ir->tap_count), unsigned(ir->source_rate), gain,
               ir->name);
}

// A gate's settings; "gate off" for nullptr (dry) or a disabled gate.
void logNoiseGate(const dkdsp::GateSettings* g) {
    if (g == nullptr || !g->enabled) {
        dkusb::Log("gate off");
        return;
    }
    dkusb::Log("gate on thr -%u dBFS atk %u.%u ms hold %u ms rel %u ms ratio %u.%u", unsigned(g->threshold),
               unsigned(g->attack / 10), unsigned(g->attack % 10), unsigned(g->hold), unsigned(g->release),
               unsigned(g->ratio / 10), unsigned(g->ratio % 10));
}

// The current preset (1-based), with its load time when loadUs > 0, then its model, IR and gate; "preset none" when
// dry.
void logPreset(uint32_t loadUs) {
    if (gCur < 0) {
        dkusb::Log("preset none");
        logModel(nullptr);
        logIr(nullptr);
        logNoiseGate(nullptr);
        return;
    }
    const ps::Preset& p = gPresets[gCur];
    if (loadUs > 0)
        dkusb::Log("preset %d \"%s\" load %lu us", gCur + 1, p.name, (unsigned long)loadUs);
    else
        dkusb::Log("preset %d \"%s\"", gCur + 1, p.name);
    logModel(&p.model);
    logIr(p.has_ir ? &p.ir : nullptr);
    logNoiseGate(&p.gate);
}

void logBootLine() {
    dkusb::Log("ducky %s seed %s sysclk %u MHz audio %u Hz %u bit block %u %s", dkboard::kName,
               SeedName(hw.seed.CheckBoardVersion()), unsigned(System::GetSysClkFreq() / 1000000),
               unsigned(hw.seed.AudioSampleRate()), bitDepth(hw.seed.AudioSaiHandle().GetConfig().bit_depth),
               unsigned(hw.seed.AudioBlockSize()), dkfw::kBuildId);
}

// Each slot's status (1 to 8).
void logSlots() {
    const char* s[ps::kSlots];
    for (int k = 0; k < ps::kSlots; ++k) s[k] = ps::StatusName(gSlotStatus[k]);
    dkusb::Log("slots %s %s %s %s %s %s %s %s", s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
}

// At boot and on HELLO: the boot line, each slot's status, the current preset.
void logBoot() {
    logBootLine();
    logSlots();
    logPreset(0);
}

// The no-audio loop's boot and HELLO lines.
void logFatal() {
    logBootLine();
    dkusb::Log("ir fft");
}

DK_ITCM void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    gLoad.OnBlockStart();
    const uint32_t start = System::GetTick();
    hw.Process();
    if (hw.FootswitchRising(0)) {
        gMuted = !gMuted;
        gMute.SetMuted(gMuted);
    }
    if (hw.FootswitchRising(1)) gFs2Presses.fetch_add(1, std::memory_order_relaxed);

    // The preset switch's handshake (servePresets).
    if (gSwap == Swap::kPlaying) {
        const uint32_t req = gSwapReq.load(std::memory_order_acquire);
        if (req != gSwapSeen) {
            gSwapSeen = req;
            gSwapFade.SetMuted(true);
            gSwap = Swap::kFadingOut;
        }
    } else if (gSwap == Swap::kParked && gSwapLoaded.load(std::memory_order_acquire) == gSwapSeen) {
        gLevel = gNextLevel;
        gInCal = gNextInCal;
        gIrOn = gNextIrOn;
        gModelOn = gNextModelOn;
        gSwapFade.SetMuted(false);
        gSwap = Swap::kPlaying;
    }

    for (int k = 0; k < 6; ++k) gKnob[k].store(hw.Knob(k), std::memory_order_relaxed);
    uint8_t tog[3];
    for (int t = 0; t < 3; ++t) {
        tog[t] = gToggleFilter[t].Process(hw.Toggle(t));
        gToggle[t].store(tog[t], std::memory_order_relaxed);
    }
    gFsHeld.store(uint8_t((hw.FootswitchPressed(0) ? 1 : 0) | (hw.FootswitchPressed(1) ? 2 : 0)),
                  std::memory_order_relaxed);

    float peak = 0.0f;
    for (size_t i = 0; i < size; i++) peak = std::fmax(peak, std::fabs(in[0][i]));
    // The gate's settings (publishGate), then its detector on the raw input peak: its gain joins the output ramp.
    const uint32_t gw0 = gGateWord[0].load(std::memory_order_relaxed);
    const uint32_t gw1 = gGateWord[1].load(std::memory_order_relaxed);
    if (gw0 != gGateSeen[0] || gw1 != gGateSeen[1]) {
        gGateSeen[0] = gw0;
        gGateSeen[1] = gw1;
        uint8_t b[ps::kGateSize];
        for (int i = 0; i < 4; ++i) {
            b[i] = uint8_t(gw0 >> (8 * i));
            b[4 + i] = uint8_t(gw1 >> (8 * i));
        }
        gGate.Set(ps::GetGate(b));
    }
    const float gate = gGate.Process(peak);
    // out[0] is the working buffer; the engine accepts in == out.
    const float inTarget = gInCal * dkdsp::InputGain(gInKnob.Process(hw.Knob(0)));
    gInGain.Process(in[0], out[0], size, inTarget);
    if (gSwap == Swap::kParked) {
        std::fill(out[0], out[0] + size, 0.0f);  // the main loop owns the engine and the IR
    } else if (gModelOn) {
        gEngine.Process(out[0], out[0], size);
        if (gIrOn) gIr.Process(out[0], out[0]);  // size is kBlockSize: dkfw::Start sets the block size
    }
    // Knob 5 low-cut, Knob 6 its resonance, Knob 4 high-cut, Knob 3 its resonance.
    gEq.Process(out[0], size, {hw.Knob(4), hw.Knob(5), hw.Knob(3), hw.Knob(2), tog[0], tog[1], tog[2]});
    const float outTarget = gLevel * dkdsp::OutputGain(gOutKnob.Process(hw.Knob(1))) * gate;
    gOutGain.Process(out[0], out[0], size, outTarget);
    gMute.Process(out[0], out[0], size);
    gSwapFade.Process(out[0], out[0], size);
    if (gSwap == Swap::kFadingOut && gSwapFade.Silent()) {
        gSwap = Swap::kParked;
        gSwapParked.store(gSwapSeen, std::memory_order_release);
    }
    std::copy(out[0], out[0] + size, out[1]);
    float outPeak = 0.0f;
    for (size_t i = 0; i < size; i++) outPeak = std::fmax(outPeak, std::fabs(out[0][i]));

    gLed1.Set(gMuted ? 0.0f : 1.0f);
    gLed1.Update();
    const uint32_t blink = gBlinkReq.load(std::memory_order_acquire);
    if (blink != gBlinkSeen) {
        gBlinkSeen = blink;
        gBlinker.Start(gBlinkCount.load(std::memory_order_relaxed));
    }
    gLed2.Set(gBlinker.Tick() ? 1.0f : 0.0f);
    gLed2.Update();

    gMutedPub.store(gMuted, std::memory_order_relaxed);
    if (gPeakReset.exchange(false, std::memory_order_relaxed)) {
        gPeakAcc = 0.0f;
        gOutPeakAcc = 0.0f;
    }
    gPeakAcc = std::fmax(gPeakAcc, peak);
    gPeak.store(gPeakAcc, std::memory_order_relaxed);
    gOutPeakAcc = std::fmax(gOutPeakAcc, outPeak);
    gOutPeak.store(gOutPeakAcc, std::memory_order_relaxed);
    if (gGateMeterReset.exchange(false, std::memory_order_relaxed)) {
        gGateLevelAcc = 0.0f;
        gGateGainAcc = 1.0f;
    }
    gGateLevelAcc = std::fmax(gGateLevelAcc, gGate.level());
    gGateLevel.store(gGateLevelAcc, std::memory_order_relaxed);
    gGateGainAcc = std::fmin(gGateGainAcc, gate);
    gGateGain.store(gGateGainAcc, std::memory_order_relaxed);

    if (System::GetTick() - start > gBlockTicks) gUnderruns.fetch_add(1, std::memory_order_relaxed);
    // A Reset just before OnBlockEnd takes its first-cycle path: the published loads are never NaN.
    if (gLoadReset.exchange(false, std::memory_order_relaxed)) gLoad.Reset();
    gLoad.OnBlockEnd();
    gLoadAvg.store(gLoad.GetAvgCpuLoad(), std::memory_order_relaxed);
    gLoadMax.store(gLoad.GetMaxCpuLoad(), std::memory_order_relaxed);
}

int pct(float v) { return int(lroundf(v * 100.0f)); }

int dbfs(float peak) { return peak > 0.0f ? std::max(int(lroundf(20.0f * log10f(peak))), -120) : -120; }

// A linear level as tenths of a dB below 1, 0..1200 (1200 for 0 and below -120 dB).
uint16_t tenthsBelow(float lin) {
    return lin > 0.0f ? uint16_t(std::clamp(lroundf(-200.0f * log10f(lin)), 0L, 1200L)) : 1200;
}

void logControls() {
    int k[6];
    for (int i = 0; i < 6; ++i) k[i] = pct(gKnob[i].load(std::memory_order_relaxed));
    const uint8_t fs = gFsHeld.load(std::memory_order_relaxed);
    dkusb::Log("ctl k %d %d %d %d %d %d sw %u %u %u fs %d %d", k[0], k[1], k[2], k[3], k[4], k[5],
               unsigned(gToggle[0].load(std::memory_order_relaxed)),
               unsigned(gToggle[1].load(std::memory_order_relaxed)),
               unsigned(gToggle[2].load(std::memory_order_relaxed)), fs & 1, (fs >> 1) & 1);
}

void logStatus() {
    const float peak = gPeak.load(std::memory_order_relaxed), outPeak = gOutPeak.load(std::memory_order_relaxed);
    gPeakReset.store(true, std::memory_order_relaxed);
    const float avg = gLoadAvg.load(std::memory_order_relaxed), max = gLoadMax.load(std::memory_order_relaxed);
    gLoadReset.store(true, std::memory_order_relaxed);
    gLoadAvgPm = uint16_t(std::clamp(int(avg * 1000), 0, 16383));
    gLoadMaxPm = uint16_t(std::clamp(int(max * 1000), 0, 16383));
    dkusb::Log("load avg %d max %d permille underruns %lu mute %s in %d dBFS out %d dBFS", int(avg * 1000),
               int(max * 1000), (unsigned long)gUnderruns.load(std::memory_order_relaxed),
               gMutedPub.load(std::memory_order_relaxed) ? "on" : "off", dbfs(peak), dbfs(outPeak));
}

// Writes slot as the newest state record, into the state sector not holding the newest one, and verifies it through
// the memory-mapped QSPI. On a failure the previous record stays the newest; the next switch tries again. The callback
// reads nothing from QSPI, so the blocking erase and write do not stall audio.
void writeState(int slot) {
    const uint32_t t0 = System::GetNow();
    const int idx = gStateIndex == 0 ? 1 : 0;
    const uint32_t off = ps::kStateOffset[idx];
    uint8_t buf[ps::kStateBytes];
    ps::BuildState(buf, gStateSeq + 1, slot);
    bool ok = hw.seed.qspi.Erase(off, off + ps::kStateSectorBytes) == QSPIHandle::Result::OK &&
              hw.seed.qspi.Write(off, ps::kStateBytes, buf) == QSPIHandle::Result::OK;
    SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t*>(ps::kQspiBase + off), int32_t(ps::kStateSectorBytes));
    uint32_t seq = 0;
    int written = -1;
    ok = ok && ps::StateValid(mapped(off), seq, written) && seq == gStateSeq + 1 && written == slot &&
         std::memcmp(mapped(off), buf, ps::kStateBytes) == 0;
    if (!ok) {
        dkusb::Log("state write failed");
        return;
    }
    gStateIndex = idx;
    gStateSeq = seq;
    gStateSlot = slot;
    dkusb::Log("state slot %d seq %lu %lu ms", slot + 1, (unsigned long)seq, (unsigned long)(System::GetNow() - t0));
}

// Sets the wanted slot and restarts LED2's repeating blinks for it; dry turns them off.
void retarget(int s) {
    gTarget = s;
    gBlinkCount.store(uint8_t(s + 1), std::memory_order_relaxed);
    gBlinkReq.store(++gBlinkSeq, std::memory_order_release);
}

// FS2 steps gTarget through the filled slots (presses while dry are dropped) and starts LED2's blinks; a target other
// than the loaded slot, or gReload, runs a switch (the callback fades out and parks, this loads, the callback fades
// in); presses during a switch coalesce into the next one, and a reload asked for during a switch is served by its
// load. kPersistDelayMs after the last switch the slot goes to the state record (dry is not recorded).
void servePresets(uint32_t now) {
    const uint32_t presses = gFs2Presses.load(std::memory_order_relaxed);
    if (presses != gFs2Seen && gTarget >= 0) {
        int t = gTarget;
        for (uint32_t n = presses - gFs2Seen; n > 0; --n) t = ps::NextFilled(gFilled, t);
        retarget(t);
    }
    gFs2Seen = presses;

    if (!gSwapping && (gTarget != gCur || gReload)) {
        gSwapReq.store(++gReq, std::memory_order_release);
        gSwapping = true;
    }
    if (gSwapping && gSwapParked.load(std::memory_order_acquire) == gReq) {
        const uint32_t t0 = System::GetUs();
        const int slot = gTarget;
        loadPreset(slot);
        gReload = false;
        gSwapLoaded.store(gReq, std::memory_order_release);
        const uint32_t loadUs = System::GetUs() - t0;
        gSwapping = false;
        gCur = slot;
        logPreset(loadUs > 0 ? loadUs : 1);
        gPersistPending = true;
        gSwitchedAt = now;
    }
    if (gPersistPending && !gSwapping && now - gSwitchedAt >= kPersistDelayMs) {
        gPersistPending = false;
        if (gCur >= 0 && gCur != gStateSlot) writeState(gCur);
    }
}

// Re-parses slot k from flash into gPresets[k], gSlotStatus[k] and gFilled[k].
void rescan(int k) {
    gSlotStatus[k] = ps::Parse(mapped(slotOff(k)), ps::kSlotBytes, &gPresets[k]);
    gFilled[k] = gSlotStatus[k] == ps::Status::kOk;
}

// A target slot that is no longer filled moves on to the next filled one (dry when none is).
void settleTarget() {
    if (gTarget >= 0 && !gFilled[gTarget]) retarget(ps::NextFilled(gFilled, gTarget));
}

// A filled slot's image size.
size_t imageSize(int k) { return gPresets[k].has_ir ? ps::kSizeWithIr : ps::kSizeNoIr; }

// Erases slot k's 64 KB block, writes data[0, size) there unless data is nullptr, and verifies the result through the
// memory-mapped QSPI (the image, or an all-0xFF block for an erase). As in writeState, the callback reads nothing from
// QSPI, so the blocking erase and write do not stall audio. The caller rescans the slot.
// libDaisy's 64 KB block erase leaves the QSPI in indirect mode, where reading the mapped region stops the pedal (M7
// bench: an ERASE without this never answered); its Write and its 4 KB sector erase return to memory-mapped mode. So
// an erase without data ends with a sector erase of the slot's (already erased) first 4 KB.
bool writeSlot(int k, uint8_t* data, size_t size) {
    const uint32_t off = slotOff(k);
    bool ok = hw.seed.qspi.Erase(off, off + ps::kSlotBytes) == QSPIHandle::Result::OK;
    if (ok)
        ok = data != nullptr ? hw.seed.qspi.Write(off, uint32_t(size), data) == QSPIHandle::Result::OK
                             : hw.seed.qspi.EraseSector(off) == QSPIHandle::Result::OK;
    SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t*>(ps::kQspiBase + off), int32_t(ps::kSlotBytes));
    if (!ok) return false;
    if (data != nullptr) return std::memcmp(mapped(off), data, size) == 0;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(mapped(off));
    for (size_t i = 0; i < ps::kSlotBytes / 4; ++i)
        if (w[i] != 0xFFFFFFFFu) return false;
    return true;
}

// The RESULT for cmd, sent by serveReplies.
void result(uint8_t cmd, dklink::Result code, uint32_t value) {
    gResultDue = true;
    gResultCmd = cmd;
    gResultCode = code;
    gResultValue = value;
}

// After a slot write and its log line: the `slots` line and the RESULT (the flash time on success, `flash` else).
void finishWrite(uint8_t cmd, bool ok, uint32_t ms) {
    using dklink::Result;
    result(cmd, ok ? Result::kOk : Result::kFlash, ok ? ms : 0);
    logSlots();
}

// The link's slot commands (docs/MIDI.md); false for a command this does not know. Each sets the RESULT; slot changes
// log their line and the `slots` line.
bool onCommand(uint8_t cmd, const uint8_t* p, size_t n) {
    using dklink::Result;
    switch (cmd) {
        case dklink::kList:
            if (n != 0) {
                result(cmd, Result::kBadRequest, 0);
            } else {
                gListNext = 0;  // serveReplies sends the SLOTs, then the RESULT
            }
            return true;
        case dklink::kWatch:
            if (n != 1 || p[0] > 1) {
                result(cmd, Result::kBadRequest, 0);
                return true;
            }
            gWatch = p[0] == 1;
            // The first STATUS comes one period later and covers only that period: the gate meter restarts now.
            gWatchAt = System::GetNow();
            gGateMeterReset.store(true, std::memory_order_relaxed);
            result(cmd, Result::kOk, 0);
            return true;
        case dklink::kUploadBegin: {
            const Result r = gUpload.Begin(p, n);
            result(cmd, r, gUpload.next());
            return true;
        }
        case dklink::kUploadChunk: {
            const Result r = gUpload.Chunk(p, n);
            result(cmd, r, gUpload.next());
            return true;
        }
        case dklink::kUploadCommit: {
            if (n != 0) {
                result(cmd, Result::kBadRequest, 0);
                return true;
            }
            const Result r = gUpload.Ready();
            if (r != Result::kOk) {
                result(cmd, r, 0);
                return true;
            }
            const ps::Status st = ps::Parse(gUpload.data(), gUpload.size(), &gCheck);
            if (st != ps::Status::kOk) {
                gUpload.Clear();
                result(cmd, Result::kInvalid, uint8_t(st));
                return true;
            }
            const int s = gUpload.slot();
            const uint32_t t0 = System::GetNow();
            const bool ok = writeSlot(s, gUpload.data(), gUpload.size());
            const uint32_t ms = System::GetNow() - t0;
            gUpload.Clear();
            rescan(s);
            if (ok) {
                if (gTarget == s)
                    gReload = true;
                else if (gTarget < 0)
                    retarget(s);
                dkusb::Log("upload slot %d \"%s\" %lu ms", s + 1, gPresets[s].name, (unsigned long)ms);
            } else {
                dkusb::Log("slot write failed %d", s + 1);
            }
            settleTarget();
            finishWrite(cmd, ok, ms);
            return true;
        }
        case dklink::kErase: {
            if (n != 1 || p[0] >= ps::kSlots) {
                result(cmd, Result::kBadRequest, 0);
                return true;
            }
            const int k = p[0];
            const uint32_t t0 = System::GetNow();
            const bool ok = writeSlot(k, nullptr, 0);
            const uint32_t ms = System::GetNow() - t0;
            rescan(k);
            settleTarget();
            if (ok)
                dkusb::Log("erase slot %d %lu ms", k + 1, (unsigned long)ms);
            else
                dkusb::Log("slot write failed %d", k + 1);
            finishWrite(cmd, ok, ms);
            return true;
        }
        case dklink::kRename: {
            bool valid = n >= 2 && n <= 1 + dklink::kMaxName && p[0] < ps::kSlots;
            for (size_t i = 1; valid && i < n; ++i) valid = p[i] >= 0x20 && p[i] <= 0x7E;
            if (!valid) {
                result(cmd, Result::kBadRequest, 0);
                return true;
            }
            const int k = p[0];
            if (!gFilled[k]) {
                result(cmd, Result::kSlotState, 0);
                return true;
            }
            const size_t size = imageSize(k);
            std::memcpy(gScratch[0], mapped(slotOff(k)), size);
            ps::Rename(gScratch[0], size, reinterpret_cast<const char*>(p + 1), n - 1);
            const uint32_t t0 = System::GetNow();
            const bool ok = writeSlot(k, gScratch[0], size);
            const uint32_t ms = System::GetNow() - t0;
            rescan(k);
            settleTarget();
            if (ok)
                dkusb::Log("rename slot %d \"%s\" %lu ms", k + 1, gPresets[k].name, (unsigned long)ms);
            else
                dkusb::Log("slot write failed %d", k + 1);
            finishWrite(cmd, ok, ms);
            return true;
        }
        case dklink::kSwap: {
            if (n != 2 || p[0] >= ps::kSlots || p[1] >= ps::kSlots || p[0] == p[1]) {
                result(cmd, Result::kBadRequest, 0);
                return true;
            }
            const int a = p[0], b = p[1];
            const auto movable = [](int k) {
                return gSlotStatus[k] == ps::Status::kOk || gSlotStatus[k] == ps::Status::kEmpty;
            };
            if (!movable(a) || !movable(b)) {
                result(cmd, Result::kSlotState, 0);
                return true;
            }
            const size_t sizeA = gFilled[a] ? imageSize(a) : 0, sizeB = gFilled[b] ? imageSize(b) : 0;
            if (sizeA > 0) std::memcpy(gScratch[0], mapped(slotOff(a)), sizeA);
            if (sizeB > 0) std::memcpy(gScratch[1], mapped(slotOff(b)), sizeB);
            const uint32_t t0 = System::GetNow();
            int failed = -1;
            if (!writeSlot(a, sizeB > 0 ? gScratch[1] : nullptr, sizeB))
                failed = a;
            else if (!writeSlot(b, sizeA > 0 ? gScratch[0] : nullptr, sizeA))
                failed = b;
            const uint32_t now = System::GetNow(), ms = now - t0;
            rescan(a);
            rescan(b);
            if (failed < 0) {
                // Playback follows the presets: the playing one keeps playing under its new number.
                const int cur = gCur;
                for (int* v : {&gCur, &gTarget}) *v = *v == a ? b : *v == b ? a : *v;
                if (gCur != cur) {
                    gPersistPending = true;
                    gSwitchedAt = now;
                }
                settleTarget();
                dkusb::Log("swap slots %d %d %lu ms", a + 1, b + 1, (unsigned long)ms);
            } else {
                settleTarget();
                if (gTarget >= 0) gReload = true;  // the target's flash may no longer hold what plays
                dkusb::Log("slot write failed %d", failed + 1);
            }
            finishWrite(cmd, failed < 0, ms);
            return true;
        }
        case dklink::kRead: {
            if (n != 4 || p[0] >= ps::kSlots) {
                result(cmd, Result::kBadRequest, 0);
                return true;
            }
            const int k = p[0];
            if (!gFilled[k]) {
                result(cmd, Result::kSlotState, 0);
                return true;
            }
            const uint32_t off = dklink::GetSeptets(p + 1, 3);
            if (off >= imageSize(k)) {
                result(cmd, Result::kBadRequest, 0);
                return true;
            }
            const size_t len = std::min<size_t>(dklink::kReadBytes, imageSize(k) - off);
            std::memcpy(gReadBuf, mapped(slotOff(k)) + off, len);
            gReadDue = true;
            gReadSlot = uint8_t(k);
            gReadOff = off;
            gReadLen = len;
            result(cmd, Result::kOk, uint32_t(off + len));
            return true;
        }
        case dklink::kGate:
        case dklink::kGateTry: {
            dkdsp::GateSettings g;
            if (n != dklink::kGatePayload || p[0] >= ps::kSlots || !dklink::ParseGate(p + 1, n - 1, g)) {
                result(cmd, Result::kBadRequest, 0);
                return true;
            }
            const int k = p[0];
            if (cmd == dklink::kGateTry) {
                // An audition, sent while a slider moves: no log line.
                if (k != gCur) {
                    result(cmd, Result::kSlotState, 0);
                    return true;
                }
                publishGate(g);
                result(cmd, Result::kOk, 0);
                return true;
            }
            if (!gFilled[k]) {
                result(cmd, Result::kSlotState, 0);
                return true;
            }
            const size_t size = imageSize(k);
            std::memcpy(gScratch[0], mapped(slotOff(k)), size);
            ps::SetGate(gScratch[0], size, g);
            const uint32_t t0 = System::GetNow();
            const bool ok = writeSlot(k, gScratch[0], size);
            const uint32_t ms = System::GetNow() - t0;
            rescan(k);
            settleTarget();
            if (ok) {
                if (k == gCur && gFilled[k]) publishGate(gPresets[k].gate);
                dkusb::Log("gate slot %d %s %lu ms", k + 1, g.enabled ? "on" : "off", (unsigned long)ms);
            } else {
                dkusb::Log("slot write failed %d", k + 1);
            }
            finishWrite(cmd, ok, ms);
            return true;
        }
        default: return false;
    }
}

// Once per main-loop pass, before dkusb::Serve: LIST's SLOTs then its RESULT, READ's DATA, the pending RESULT, and the
// STATUS when WATCH is on and kWatchPeriodMs has passed. Whatever does not fit in Tx() waits for a later pass, in order.
void serveReplies(uint32_t now) {
    dklink::TxBuffer& tx = dkusb::Tx();
    while (gListNext >= 0 && gListNext < ps::kSlots) {
        const int k = gListNext;
        const bool ok = gFilled[k];
        const ps::Preset& pr = gPresets[k];
        const uint8_t flags = uint8_t((ok && pr.has_ir ? 1 : 0) | (k == gCur ? 2 : 0));
        if (!tx.AppendSlot(uint8_t(k), uint8_t(gSlotStatus[k]), flags, ok ? pr.name : nullptr,
                           ok ? pr.model.name : nullptr, ok && pr.has_ir ? pr.ir.name : nullptr))
            break;
        ++gListNext;
    }
    if (gListNext == ps::kSlots && !gResultDue) {
        result(dklink::kList, dklink::Result::kOk, ps::kSlots);
        gListNext = -1;
    }
    if (gReadDue && tx.AppendData(gReadSlot, gReadOff, dkmodel::Crc32(gReadBuf, gReadLen), gReadBuf, gReadLen))
        gReadDue = false;
    if (gResultDue && !gReadDue && tx.AppendResult(gResultCmd, gResultCode, gResultValue)) gResultDue = false;
    if (gWatch && now - gWatchAt >= dklink::kWatchPeriodMs) {
        dklink::StatusFrame s;
        s.load_avg = gLoadAvgPm;
        s.load_max = gLoadMaxPm;
        s.underruns = gUnderruns.load(std::memory_order_relaxed);
        s.slot = gCur < 0 ? 0x7F : uint8_t(gCur);
        s.muted = gMutedPub.load(std::memory_order_relaxed);
        for (int k = 0; k < 6; ++k)
            s.knob[k] = uint16_t(std::clamp(lroundf(gKnob[k].load(std::memory_order_relaxed) * 1000.0f), 0L, 1000L));
        for (int t = 0; t < 3; ++t) s.toggle[t] = gToggle[t].load(std::memory_order_relaxed);
        s.footswitches = gFsHeld.load(std::memory_order_relaxed);
        s.gate_level = tenthsBelow(gGateLevel.load(std::memory_order_relaxed));
        s.gate_cut = tenthsBelow(gGateGain.load(std::memory_order_relaxed));
        if (tx.AppendStatus(s)) {
            gWatchAt = now;
            gGateMeterReset.store(true, std::memory_order_relaxed);
        }
    }
}

}  // namespace

int main() {
    dkfw::Start(hw, dkdsp::kBlockSize, SaiHandle::Config::SampleRate::SAI_48KHZ);
    gLed1.Init(dkboard::Board::kLed1, false, dkdsp::kSampleRate / dkdsp::kBlockSize);
    gLed2.Init(dkboard::Board::kLed2, false, dkdsp::kSampleRate / dkdsp::kBlockSize);

    dkusb::Init(hw.seed);
    if (!gIr.Init()) {
        // No audio, LEDs off; USB (DFU included) keeps serving so working firmware can be flashed.
        logFatal();
        uint32_t lastLog = System::GetNow();
        for (;;) {
            dkusb::Serve(logFatal);
            const uint32_t now = System::GetNow();
            if (now - lastLog >= 1000) {
                lastLog = now;
                dkusb::Log("ir fft");
            }
        }
    }

    for (int k = 0; k < ps::kSlots; ++k) {
        gSlotStatus[k] = ps::Parse(mapped(ps::kBankOffset + k * ps::kSlotBytes), ps::kSlotBytes, &gPresets[k]);
        gFilled[k] = gSlotStatus[k] == ps::Status::kOk;
    }
    gStateIndex = ps::NewestState(mapped(ps::kStateOffset[0]), mapped(ps::kStateOffset[1]), gStateSeq, gStateSlot);
    if (gStateIndex < 0) gStateSlot = -1;
    gCur = gTarget = ps::BootSlot(gFilled, gStateSlot);
    loadPreset(gCur);
    gLevel = gNextLevel;
    gInCal = gNextInCal;
    gIrOn = gNextIrOn;
    gModelOn = gNextModelOn;
    if (gCur >= 0) {
        gBlinkCount.store(uint8_t(gCur + 1), std::memory_order_relaxed);
        gBlinkReq.store(++gBlinkSeq, std::memory_order_relaxed);
    }
    logBoot();

    gSwapFade.Init(false);
    gMute.Init(false);
    gInKnob.Init(dkdsp::kSampleRate / dkdsp::kBlockSize, dkdsp::kKnobSmoothS, 0.5f);
    gInGain.Init(gInCal);
    // The output fades in from silence at boot.
    gOutKnob.Init(dkdsp::kSampleRate / dkdsp::kBlockSize, dkdsp::kKnobSmoothS, 0.0f);
    gOutGain.Init(0.0f);
    gLoad.Init(hw.seed.AudioSampleRate(), hw.seed.AudioBlockSize());
    gBlockTicks = uint32_t(uint64_t(System::GetTickFreq()) * dkdsp::kBlockSize / uint32_t(dkdsp::kSampleRate));
    for (dkctl::ToggleFilter& f : gToggleFilter) f.Init(1);
    // Neutral-ish; the EQ settles on the real controls within ~100 ms, while the output fades in.
    gEq.Init({0.5f, 0.5f, 0.5f, 0.5f, 1, 1, 1});
    hw.seed.adc.Start();
    hw.seed.StartAudio(AudioCallback);

    uint32_t lastLog = System::GetNow();
    for (;;) {
        serveReplies(System::GetNow());
        // Slot commands block for their flash writes: the time is read after them.
        dkusb::Serve(logBoot, onCommand);
        const uint32_t now = System::GetNow();
        servePresets(now);
        if (now - lastLog >= 1000) {
            lastLog = now;
            logControls();
            logStatus();
        }
    }
}
