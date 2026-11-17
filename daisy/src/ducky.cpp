// Ducky pedal firmware (Cleveland Music Co. HotHouse, board.h). The left input goes through the compressor
// (dsp/compressor.h), Knob 1's input trim, the current preset's A2-Lite model (dsp/a2lite.h) and cab IR when it has
// one (dsp/convolver.h), the EQ (dsp/eq.h: low-cut, three bells, high-cut; the compressor, the EQ and the loop are
// driven by the parameters, set over the link or by the controls assigned to them, Knobs 3-6 and SW1-SW3: params.h,
// param_store.h; the assignments and values are kept in the settings record, settings.h), and Knob 2's output volume
// times the model's level match and the preset's noise gate (dsp/gain.h, dsp/noise_gate.h: the gate listens to the
// input before the compressor), then FS1's mute ramp and the preset switch's fade. loop_mode routes it (docs/LOOP.md):
// - MISO: the result is copied to both outputs (dual mono); the right input is not used.
// - pre-model: the compressor's output, times the send level, goes out on R (the send); R In (the return), times the
//   return level, feeds the trim, the model, the IR, the EQ and the output gain to L Out.
// - post-model: the model's output, times the level match, the gate and the send level, goes out on R; R In, times the
//   return level, feeds the IR, the EQ and Knob 2's volume to L Out.
// FS1 mutes L Out only; a loop_mode change fades both outputs out, reroutes and fades them in (gModeFade). Presets
// live in QSPI (preset.h: 8 slots, written by tools/preset_bank.ts over DFU, or slot by slot over the link:
// docs/MIDI.md) and are parsed into SDRAM at boot and after every slot change; corrupt or empty slots are skipped.
// With no valid slot the pedal plays dry (compressor, trim, EQ, volume, mute; no gate). If the IR convolver's FFT does
// not initialise, audio never starts: the pedal serves USB (DFU included) and logs "ir fft" once a second.
//
// Two threads of control:
// - AudioCallback (the audio DMA interrupt, 48 kHz, 24-sample blocks): reads and publishes the controls (30 ms toggle
//   hysteresis), toggles the mute on FS1, counts FS2 presses, applies the link's parameter edits and arbitrates the
//   control slots (param_store.h), steps the preset switch and the loop mode, runs the gate's detector, the
//   compressor, the trim, the engine, the IR, the EQ, the loop's send and return levels and the output gain, the mute
//   ramp, the preset fade and the mode fade, measures the input and output peaks (both channels), the gate's level and
//   cut and the compressor's gain reduction, drives LED1 and LED2's blinks, and measures the load and the underruns.
// - The main loop: USB (usb_link.h: Identity, HELLO, DFU; the slot and parameter commands in onCommand, their replies
//   and the STATUS and VALUES stream in serveReplies), the preset switch (servePresets), the state record
//   (writeState), the settings record (saveSettings), the slot writes (writeSlot), the once-a-second log lines and
//   the `loop` line when loop_mode changes.
// A preset switch: the main loop raises gSwapReq; the callback fades out (gSwapFade), then parks (writes silence,
// touches neither the engine nor the IR) and acks with gSwapParked; the main loop loads the next preset into the engine
// and the IR and publishes its gains with gSwapLoaded; the callback takes them and fades in.
// They share only atomics, each with one writer, and one single-producer single-consumer ring:
// - callback -> main loop (relaxed): gKnob, gToggle, gFsHeld, gMutedPub, gUnderruns, gPeak, gOutPeak, gRinPeak,
//   gRoutPeak, gLoadAvg, gLoadMax, gFs2Presses, gGateLevel, gGateGain, gCompGain, gParamPub, gAssignPub, gHeldPub;
//   gSwapParked and gEditsDone (release/acquire);
// - main loop -> callback (relaxed): gPeakReset, gLoadReset, gBlinkCount, gGateWord (publishGate), gMeterReset;
//   gSwapReq, gSwapLoaded (publishing gNextLevel, gNextInCal, gNextIrOn, gNextModelOn) and gBlinkReq
//   (release/acquire); gEdits (the ring: SET and ASSIGN, applied at the next block).
// Memory (memory.h): AudioCallback runs from ITCM; the engine, the compressor, the EQ and the callback's own state live
// in DTCM, the IR convolver (66 KB of spectra, streamed through the D-cache every block) in AXI SRAM, which the bench
// measured 24% faster for it than D2 SRAM (docs/FILTERS.md), the parsed presets in SDRAM. FPSCR.FZ is on (denormals
// flush to zero).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "binary.h"
#include "board.h"
#include "controls.h"
#include "dsp/a2lite.h"
#include "dsp/blocks.h"
#include "dsp/compressor.h"
#include "dsp/convolver.h"
#include "dsp/eq.h"
#include "dsp/gain.h"
#include "dsp/mute.h"
#include "dsp/noise_gate.h"
#include "firmware.h"
#include "ir.h"
#include "memory.h"
#include "model.h"
#include "param_store.h"
#include "params.h"
#include "preset.h"
#include "settings.h"
#include "spsc_ring.h"
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
DK_DTCM dkctl::ToggleFilter gToggleFilter[dkboard::kToggles];
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
DK_DTCM dkdsp::Compressor gComp;
DK_DTCM dkdsp::Eq gEq;
DK_DTCM dkparam::Store gStore;     // the parameter values and the control slots; Init before StartAudio
DK_DTCM uint32_t gEditsTaken = 0;  // edits taken from gEdits since boot
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
DK_DTCM float gCompGainAcc = 1.0f;                          // the compressor's lowest gain() since the last STATUS
DK_DTCM uint8_t gLoop = dkparam::kLoopMiso;  // the routing in use; follows loop_mode at a fade's silence
DK_DTCM dkdsp::MuteRamp gModeFade;           // a loop_mode change: both outputs out, reroute, in
DK_DTCM dkdsp::Smoother gSendKnob, gReturnKnob;  // loop_send, loop_return positions
DK_DTCM dkdsp::GainRamp gSendGain, gReturnGain;
DK_DTCM float gRinPeakAcc, gRoutPeakAcc;  // R input and R output peaks since the main loop last read them

// Shared with the main loop: in SRAM (AXI).
// Callback -> main loop.
std::atomic<float> gKnob[dkboard::kKnobs];                // knobs 1-6, 0..1
std::atomic<uint8_t> gToggle[dkboard::kToggles]{1, 1, 1};  // SW1-SW3 filtered: 0 down, 1 middle, 2 up
std::atomic<uint8_t> gFsHeld{0};           // bit 0 FS1, bit 1 FS2
std::atomic<bool> gMutedPub{false};        // gMuted
std::atomic<uint32_t> gUnderruns{0};       // blocks since boot whose callback ran longer than one block period
std::atomic<float> gPeak{0.0f};            // gPeakAcc, published every block
std::atomic<float> gOutPeak{0.0f};         // gOutPeakAcc, published every block
std::atomic<float> gRinPeak{0.0f};         // gRinPeakAcc, published every block
std::atomic<float> gRoutPeak{0.0f};        // gRoutPeakAcc, published every block
std::atomic<float> gLoadAvg{0.0f}, gLoadMax{0.0f};  // gLoad's, published every block
std::atomic<float> gGateLevel{0.0f};       // gGateLevelAcc, published every block
std::atomic<float> gGateGain{1.0f};        // gGateGainAcc, published every block
std::atomic<float> gCompGain{1.0f};        // gCompGainAcc, published every block
std::atomic<uint32_t> gFs2Presses{0};      // FS2 presses since boot
std::atomic<uint32_t> gSwapParked{0};      // the gSwapReq the callback has parked for (release)
std::atomic<float> gParamPub[dkparam::kParamCount];       // gStore's values, published every block
std::atomic<uint8_t> gAssignPub[dkparam::kControlSlots];  // gStore's assignments, published every block
std::atomic<uint8_t> gHeldPub{0};                         // gStore's held bits, published every block
std::atomic<uint32_t> gEditsDone{0};  // gEditsTaken once those edits' values are published (release)
// Main loop -> callback.
std::atomic<bool> gPeakReset{false};  // restart gPeakAcc, gOutPeakAcc, gRinPeakAcc and gRoutPeakAcc
std::atomic<bool> gLoadReset{false};  // restart gLoad
std::atomic<uint32_t> gSwapReq{0};     // a new value asks the callback to fade out and park (release)
std::atomic<uint32_t> gSwapLoaded{0};  // gSwapReq's value once the next preset is loaded (release): fade in
std::atomic<uint32_t> gBlinkReq{0};    // a new value starts gBlinkCount blinks on LED2 (release)
std::atomic<uint8_t> gBlinkCount{0};
// The gate's settings as preset.h's 8-byte block, two little-endian words (publishGate). A torn pair mixes two valid
// settings for one block; the reserved byte is 0, so no published pair equals gGateSeen's initial value.
std::atomic<uint32_t> gGateWord[2];
std::atomic<bool> gMeterReset{false};      // restart gGateLevelAcc, gGateGainAcc and gCompGainAcc
SpscRing<dkparam::Edit, 16> gEdits;        // SET and ASSIGN (onCommand), applied by the callback's next block
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

// The link's slot and parameter commands (onCommand, serveReplies). Main loop only.
DK_SDRAM uint8_t gUploadBuf[ps::kSizeWithIr];
dklink::Upload gUpload(gUploadBuf);
DK_SDRAM uint8_t gScratch[2][ps::kSizeWithIr];  // RENAME's, SWAP's and GATE's copies of slot images
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
uint32_t gEditsSent = 0;                  // edits pushed to gEdits since boot
int gSettingsIndex = -1;                  // the settings sector holding the newest record; -1 = none (factory)
uint32_t gSettingsSeq = 0;
int gDumpNext = -1;       // the next PARAM to send for DUMP; kParamCount: its VALUES; kParamCount + 1: its RESULT
bool gValuesDue = false;  // WATCH's VALUES, after its STATUS
uint8_t gLoopLogged = dkparam::kLoopMiso;  // the loop_mode the last `loop` line named

// Slots and models.

const uint8_t* mapped(uint32_t off) { return reinterpret_cast<const uint8_t*>(ps::kQspiBase + off); }
uint32_t slotOff(int k) { return ps::kBankOffset + uint32_t(k) * ps::kSlotBytes; }
// Slot k's 64 KB as mapped from QSPI.
const uint8_t* slotData(int k) { return mapped(slotOff(k)); }

float levelOf(const dkmodel::Model& m) {
    return dkdsp::LevelMatchGain((m.flags & dkmodel::kFlagHasLoudness) != 0, m.loudness);
}
float calOf(const dkmodel::Model& m) {
    return dkdsp::InputCalibrationGain((m.flags & dkmodel::kFlagHasInputLevel) != 0, m.input_level_dbu);
}

// Log lines.

// "+D.d" / "-D.d", clamped to +-999.9: newlib-nano's printf has no %f.
void tenthsDb(char (&buf)[12], float db) {
    const long t = std::clamp(lroundf(db * 10.0f), -9999L, 9999L);
    const unsigned a = unsigned(t < 0 ? -t : t);
    snprintf(buf, sizeof buf, "%c%u.%u", t < 0 ? '-' : '+', a / 10, a % 10);
}

int pct(float v) { return int(lroundf(v * 100.0f)); }

int dbfs(float peak) { return peak > 0.0f ? std::max(int(lroundf(20.0f * log10f(peak))), -120) : -120; }

// A linear level as tenths of a dB below 1, 0..1200 (1200 for 0 and below -120 dB).
uint16_t tenthsBelow(float lin) {
    return lin > 0.0f ? uint16_t(std::clamp(lroundf(-200.0f * log10f(lin)), 0L, 1200L)) : 1200;
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

const char* seedName(DaisySeed::BoardVersion v) {
    switch (v) {
        case DaisySeed::BoardVersion::DAISY_SEED: return "rev4-ak4556";
        case DaisySeed::BoardVersion::DAISY_SEED_1_1: return "1.1-wm8731";
        case DaisySeed::BoardVersion::DAISY_SEED_2_DFM: return "2dfm-pcm3060";
        case DaisySeed::BoardVersion::DAISY_SEED_1_2: return "1.2-pcm3060";
        case DaisySeed::BoardVersion::DAISY_SEED_3: return "3-tac5242";
    }
    return "unknown";
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
               seedName(hw.seed.CheckBoardVersion()), unsigned(System::GetSysClkFreq() / 1000000),
               unsigned(hw.seed.AudioSampleRate()), bitDepth(hw.seed.AudioSaiHandle().GetConfig().bit_depth),
               unsigned(hw.seed.AudioBlockSize()), dkfw::kBuildId);
}

// Each slot's status (1 to 8).
void logSlots() {
    const char* s[ps::kSlots];
    for (int k = 0; k < ps::kSlots; ++k) s[k] = ps::StatusName(gSlotStatus[k]);
    dkusb::Log("slots %s %s %s %s %s %s %s %s", s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
}

// The published loop mode, as a `loop` line; it becomes gLoopLogged.
void logLoop() {
    static const char* const kLoopNames[3] = {"miso", "pre-model", "post-model"};
    gLoopLogged = uint8_t(gParamPub[dkparam::kLoopMode].load(std::memory_order_relaxed));
    dkusb::Log("loop %s", kLoopNames[gLoopLogged < 3 ? gLoopLogged : 0]);
}

// The settings record in use (`settings factory` with none), the live layout: each control slot's parameter, and the
// loop mode. Seven parameters on one control each fit a log line; a longer layout (a parameter on several controls) is
// cut at 120 characters.
void logSettings() {
    if (gSettingsIndex < 0)
        dkusb::Log("settings factory");
    else
        dkusb::Log("settings seq %lu", (unsigned long)gSettingsSeq);
    const char* n[dkparam::kControlSlots];
    for (int c = 0; c < dkparam::kControlSlots; ++c) {
        const uint8_t id = gAssignPub[c].load(std::memory_order_relaxed);
        n[c] = id < dkparam::kParamCount ? dkparam::kParams[id].name : "none";
    }
    dkusb::Log("layout k3 %s k4 %s k5 %s k6 %s sw1 %s sw2 %s sw3 %s", n[0], n[1], n[2], n[3], n[4], n[5], n[6]);
    logLoop();
}

// At boot and on HELLO: the boot line, each slot's status, the current preset, the settings and the layout.
void logBoot() {
    logBootLine();
    logSlots();
    logPreset(0);
    logSettings();
}

// The no-audio loop's boot and HELLO lines.
void logFatal() {
    logBootLine();
    dkusb::Log("ir fft");
}

void logControls() {
    int k[dkboard::kKnobs];
    for (int i = 0; i < dkboard::kKnobs; ++i) k[i] = pct(gKnob[i].load(std::memory_order_relaxed));
    const uint8_t fs = gFsHeld.load(std::memory_order_relaxed);
    dkusb::Log("ctl k %d %d %d %d %d %d sw %u %u %u fs %d %d", k[0], k[1], k[2], k[3], k[4], k[5],
               unsigned(gToggle[0].load(std::memory_order_relaxed)),
               unsigned(gToggle[1].load(std::memory_order_relaxed)),
               unsigned(gToggle[2].load(std::memory_order_relaxed)), fs & 1, (fs >> 1) & 1);
}

void logStatus() {
    const float peak = gPeak.load(std::memory_order_relaxed), outPeak = gOutPeak.load(std::memory_order_relaxed);
    const float rinPeak = gRinPeak.load(std::memory_order_relaxed);
    const float routPeak = gRoutPeak.load(std::memory_order_relaxed);
    gPeakReset.store(true, std::memory_order_relaxed);
    const float avg = gLoadAvg.load(std::memory_order_relaxed), max = gLoadMax.load(std::memory_order_relaxed);
    gLoadReset.store(true, std::memory_order_relaxed);
    const int avgPm = int(avg * 1000), maxPm = int(max * 1000);
    gLoadAvgPm = uint16_t(std::clamp(avgPm, 0, 16383));
    gLoadMaxPm = uint16_t(std::clamp(maxPm, 0, 16383));
    dkusb::Log("load avg %d max %d permille underruns %lu mute %s in %d dBFS out %d dBFS rin %d dBFS rout %d dBFS",
               avgPm, maxPm, (unsigned long)gUnderruns.load(std::memory_order_relaxed),
               gMutedPub.load(std::memory_order_relaxed) ? "on" : "off", dbfs(peak), dbfs(outPeak), dbfs(rinPeak),
               dbfs(routPeak));
}

// Handoffs between the threads.

// Hands g to the callback, which applies it at its next block (takeGate). Main loop (or before StartAudio).
void publishGate(const dkdsp::GateSettings& g) {
    uint8_t b[ps::kGateSize];
    ps::PutGate(b, g);
    for (int w = 0; w < 2; ++w) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= uint32_t(b[4 * w + i]) << (8 * i);
        gGateWord[w].store(v, std::memory_order_relaxed);
    }
}

// The callback's half of publishGate: a changed gGateWord pair goes to gGate.
DK_ITCM void takeGate() {
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
}

// The loaded preset's gains and flags (gNext*), taken by the callback when it fades in, and once at boot.
DK_ITCM void takeNextPreset() {
    gLevel = gNextLevel;
    gInCal = gNextInCal;
    gIrOn = gNextIrOn;
    gModelOn = gNextModelOn;
}

// The audio callback (docs/FIRMWARE.md "Threads": its steps in order).

// Steps 1-2: the controls, the toggles filtered, all published; FS1 toggles the mute; FS2 presses are counted.
DK_ITCM void readControls(uint8_t (&tog)[dkboard::kToggles]) {
    hw.Process();
    for (int t = 0; t < dkboard::kToggles; ++t) {
        tog[t] = gToggleFilter[t].Process(hw.Toggle(t));
        gToggle[t].store(tog[t], std::memory_order_relaxed);
    }
    for (int k = 0; k < dkboard::kKnobs; ++k) gKnob[k].store(hw.Knob(k), std::memory_order_relaxed);
    gFsHeld.store(uint8_t((hw.FootswitchPressed(0) ? 1 : 0) | (hw.FootswitchPressed(1) ? 2 : 0)),
                  std::memory_order_relaxed);
    if (hw.FootswitchRising(0)) {
        gMuted = !gMuted;
        gMute.SetMuted(gMuted);
    }
    if (hw.FootswitchRising(1)) gFs2Presses.fetch_add(1, std::memory_order_relaxed);
}

// gStore's values, assignments and held bits, for the main loop. The callback, and main() before StartAudio.
DK_ITCM void publishParams() {
    for (int i = 0; i < dkparam::kParamCount; ++i)
        gParamPub[i].store(gStore.Value(uint8_t(i)), std::memory_order_relaxed);
    for (int c = 0; c < dkparam::kControlSlots; ++c)
        gAssignPub[c].store(gStore.Assignment(c), std::memory_order_relaxed);
    gHeldPub.store(gStore.HeldBits(), std::memory_order_relaxed);
}

static_assert(dkparam::kFirstSlotKnob + dkparam::kKnobSlots == dkboard::kKnobs &&
                  dkparam::kSwitchSlots == dkboard::kToggles,
              "the control slots are Knobs 3-6 and SW1-SW3");

// Step 3: the link's queued SETs and ASSIGNs, then the control slots write their parameters (param_store.h); the
// result is published, then the count of edits it includes (awaitEdits).
DK_ITCM void stepParams(const uint8_t (&tog)[dkboard::kToggles]) {
    float knobs[dkparam::kKnobSlots];
    for (int s = 0; s < dkparam::kKnobSlots; ++s) knobs[s] = hw.Knob(dkparam::kFirstSlotKnob + s);
    dkparam::Edit e;
    while (gEdits.Pop(e)) {
        gStore.Apply(e, knobs);
        ++gEditsTaken;
    }
    gStore.Update(knobs, tog);
    publishParams();
    gEditsDone.store(gEditsTaken, std::memory_order_release);
}

// Step 4: the preset switch's handshake (servePresets).
DK_ITCM void stepSwap() {
    if (gSwap == Swap::kPlaying) {
        const uint32_t req = gSwapReq.load(std::memory_order_acquire);
        if (req != gSwapSeen) {
            gSwapSeen = req;
            gSwapFade.SetMuted(true);
            gSwap = Swap::kFadingOut;
        }
    } else if (gSwap == Swap::kParked && gSwapLoaded.load(std::memory_order_acquire) == gSwapSeen) {
        takeNextPreset();
        gSwapFade.SetMuted(false);
        gSwap = Swap::kPlaying;
    }
}

// Step 4b: the loop mode. A change fades both outputs out; at silence the routing follows loop_mode and they fade back
// in. A mode that flips back before the fade lands just fades back in.
DK_ITCM void stepLoop() {
    const uint8_t want = uint8_t(gStore.Value(dkparam::kLoopMode));
    if (want != gLoop && gModeFade.Silent()) gLoop = want;
    gModeFade.SetMuted(want != gLoop);
}

// Step 7: the mute flag, the input and output peaks (both channels), the gate's level and cut and the compressor's
// gain reduction, accumulated and published.
DK_ITCM void publishMeters(float peak, float outPeak, float rinPeak, float routPeak, float gate) {
    gMutedPub.store(gMuted, std::memory_order_relaxed);
    if (gPeakReset.exchange(false, std::memory_order_relaxed)) {
        gPeakAcc = 0.0f;
        gOutPeakAcc = 0.0f;
        gRinPeakAcc = 0.0f;
        gRoutPeakAcc = 0.0f;
    }
    gPeakAcc = std::fmax(gPeakAcc, peak);
    gPeak.store(gPeakAcc, std::memory_order_relaxed);
    gOutPeakAcc = std::fmax(gOutPeakAcc, outPeak);
    gOutPeak.store(gOutPeakAcc, std::memory_order_relaxed);
    gRinPeakAcc = std::fmax(gRinPeakAcc, rinPeak);
    gRinPeak.store(gRinPeakAcc, std::memory_order_relaxed);
    gRoutPeakAcc = std::fmax(gRoutPeakAcc, routPeak);
    gRoutPeak.store(gRoutPeakAcc, std::memory_order_relaxed);
    if (gMeterReset.exchange(false, std::memory_order_relaxed)) {
        gGateLevelAcc = 0.0f;
        gGateGainAcc = 1.0f;
        gCompGainAcc = 1.0f;
    }
    gGateLevelAcc = std::fmax(gGateLevelAcc, gGate.level());
    gGateLevel.store(gGateLevelAcc, std::memory_order_relaxed);
    gGateGainAcc = std::fmin(gGateGainAcc, gate);
    gGateGain.store(gGateGainAcc, std::memory_order_relaxed);
    gCompGainAcc = std::fmin(gCompGainAcc, gComp.gain());
    gCompGain.store(gCompGainAcc, std::memory_order_relaxed);
}

// Step 8: LED1 (on unless muted) and LED2's blinks.
DK_ITCM void driveLeds() {
    gLed1.Set(gMuted ? 0.0f : 1.0f);
    gLed1.Update();
    const uint32_t blink = gBlinkReq.load(std::memory_order_acquire);
    if (blink != gBlinkSeen) {
        gBlinkSeen = blink;
        gBlinker.Start(gBlinkCount.load(std::memory_order_relaxed));
    }
    gLed2.Set(gBlinker.Tick() ? 1.0f : 0.0f);
    gLed2.Update();
}

DK_ITCM void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    gLoad.OnBlockStart();
    const uint32_t start = System::GetTick();
    uint8_t tog[dkboard::kToggles];
    readControls(tog);
    stepParams(tog);
    stepSwap();
    stepLoop();
    // Step 5: the gate's settings (publishGate), then its detector on the raw input peak: its gain joins the output
    // ramp (the send's in post-model).
    const float peak = dkdsp::Peak(in[0], size);
    takeGate();
    const float gate = gGate.Process(peak);
    // Step 6: the chain, routed by gLoop. out[0] is the working buffer, out[1] the send in the loop modes; the
    // compressor, the engine and the IR accept in == out. The compressor's, the EQ's and the loop's controls are
    // parameters (stepParams). The knobs' smoothers run every block in every mode, so they stay settled; the send and
    // return gains are computed only in the loop modes.
    const float* v = gStore.Values();
    const float sendPos = gSendKnob.Process(v[dkparam::kLoopSend]);
    const float returnPos = gReturnKnob.Process(v[dkparam::kLoopReturn]);
    const float inTarget = gInCal * dkdsp::InputGain(gInKnob.Process(hw.Knob(0)));
    const float vol = dkdsp::OutputGain(gOutKnob.Process(hw.Knob(1)));
    const bool parked = gSwap == Swap::kParked;  // the main loop owns the engine and the IR
    const bool pre = gLoop == dkparam::kLoopPre, post = gLoop == dkparam::kLoopPost;
    // In pre-model the compressor's output, times the send level, is the send: independent of the preset, so the swap
    // fade leaves it alone. The return feeds the rest of the chain.
    gComp.Process(in[0], pre ? out[1] : out[0], size, v);
    if (pre) {
        gSendGain.Process(out[1], out[1], size, dkdsp::InputGain(sendPos));
        gReturnGain.Process(in[1], out[0], size, dkdsp::InputGain(returnPos));
    }
    gInGain.Process(out[0], out[0], size, inTarget);
    if (!parked && gModelOn) gEngine.Process(out[0], out[0], size);
    if (post) {
        // The send: the model's output times the level match, the gate and the send level. The return feeds the IR.
        gSendGain.Process(out[0], out[1], size, gLevel * gate * dkdsp::InputGain(sendPos));
        gReturnGain.Process(in[1], out[0], size, dkdsp::InputGain(returnPos));
    }
    // In MISO gSendGain and gReturnGain rest; a loop mode resumes them behind the mode fade.
    if (parked)
        std::fill(out[0], out[0] + size, 0.0f);
    else if (gModelOn && gIrOn)
        gIr.Process(out[0], out[0]);  // size is kBlockSize: dkfw::Start sets the block size
    gEq.Process(out[0], size, v);
    // Knob 2's volume, times the level match and the gate unless they rode the send.
    gOutGain.Process(out[0], out[0], size, post ? vol : gLevel * vol * gate);
    gMute.Process(out[0], out[0], size);  // FS1 mutes L Out only
    // In post-model the preset switch fades the send too: both stay silent while parked.
    if (gLoop == dkparam::kLoopPost)
        gSwapFade.ProcessPair(out[0], out[1], size);
    else
        gSwapFade.Process(out[0], out[0], size);
    if (gSwap == Swap::kFadingOut && gSwapFade.Silent()) {
        gSwap = Swap::kParked;
        gSwapParked.store(gSwapSeen, std::memory_order_release);
    }
    // Step 7.
    if (gLoop == dkparam::kLoopMiso) std::copy(out[0], out[0] + size, out[1]);
    gModeFade.ProcessPair(out[0], out[1], size);
    publishMeters(peak, dkdsp::Peak(out[0], size), dkdsp::Peak(in[1], size), dkdsp::Peak(out[1], size), gate);
    driveLeds();
    // Step 9.
    if (System::GetTick() - start > gBlockTicks) gUnderruns.fetch_add(1, std::memory_order_relaxed);
    // A Reset just before OnBlockEnd takes its first-cycle path: the published loads are never NaN.
    if (gLoadReset.exchange(false, std::memory_order_relaxed)) gLoad.Reset();
    gLoad.OnBlockEnd();
    gLoadAvg.store(gLoad.GetAvgCpuLoad(), std::memory_order_relaxed);
    gLoadMax.store(gLoad.GetMaxCpuLoad(), std::memory_order_relaxed);
}

// Presets.

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

// Sets the wanted slot and restarts LED2's repeating blinks for it; dry turns them off.
void retarget(int s) {
    gTarget = s;
    gBlinkCount.store(uint8_t(s + 1), std::memory_order_relaxed);
    gBlinkReq.store(++gBlinkSeq, std::memory_order_release);
}

// A target slot that is no longer filled moves on to the next filled one (dry when none is).
void settleTarget() {
    if (gTarget >= 0 && !gFilled[gTarget]) retarget(ps::NextFilled(gFilled, gTarget));
}

// Erases the 4 KB sector at QSPI offset off, writes buf[0, n) at its start and verifies it through the memory-mapped
// QSPI. The callback reads nothing from QSPI, so the blocking erase and write do not stall audio.
bool writeSector(uint32_t off, uint8_t* buf, size_t n) {
    const bool ok = hw.seed.qspi.Erase(off, off + ps::kStateSectorBytes) == QSPIHandle::Result::OK &&
                    hw.seed.qspi.Write(off, uint32_t(n), buf) == QSPIHandle::Result::OK;
    SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t*>(ps::kQspiBase + off), int32_t(ps::kStateSectorBytes));
    return ok && std::memcmp(mapped(off), buf, n) == 0;
}

// Writes slot as the newest state record, into the state sector not holding the newest one (writeSector). On a
// failure the previous record stays the newest; the next switch tries again.
void writeState(int slot) {
    const uint32_t t0 = System::GetNow();
    const int idx = gStateIndex == 0 ? 1 : 0;
    const uint32_t off = ps::kStateOffset[idx];
    uint8_t buf[ps::kStateBytes];
    ps::BuildState(buf, gStateSeq + 1, slot);
    uint32_t seq = 0;
    int written = -1;
    const bool ok = writeSector(off, buf, sizeof buf) && ps::StateValid(mapped(off), seq, written) &&
                    seq == gStateSeq + 1 && written == slot;
    if (!ok) {
        dkusb::Log("state write failed");
        return;
    }
    gStateIndex = idx;
    gStateSeq = seq;
    gStateSlot = slot;
    dkusb::Log("state slot %d seq %lu %lu ms", slot + 1, (unsigned long)seq, (unsigned long)(System::GetNow() - t0));
}

// SAVE: writes the live assignments and values (as published) as the newest settings record, into the settings sector
// not holding the newest one (writeSector); ms is the time it took. On a failure the previous record stays the newest.
bool saveSettings(uint32_t& ms) {
    const uint32_t t0 = System::GetNow();
    const int idx = gSettingsIndex == 0 ? 1 : 0;
    const uint32_t off = dkparam::kSettingsOffset[idx];
    dkparam::Settings s;
    for (int c = 0; c < dkparam::kControlSlots; ++c) s.assign[c] = gAssignPub[c].load(std::memory_order_relaxed);
    for (int i = 0; i < dkparam::kParamCount; ++i) s.value[i] = gParamPub[i].load(std::memory_order_relaxed);
    uint8_t buf[dkparam::kSettingsBytes];
    dkparam::BuildSettings(buf, gSettingsSeq + 1, s);
    uint32_t seq = 0;
    const bool ok =
        writeSector(off, buf, sizeof buf) && dkparam::SettingsValid(mapped(off), seq) && seq == gSettingsSeq + 1;
    ms = System::GetNow() - t0;
    if (!ok) {
        dkusb::Log("settings write failed");
        return false;
    }
    gSettingsIndex = idx;
    gSettingsSeq = seq;
    dkusb::Log("settings seq %lu %lu ms", (unsigned long)seq, (unsigned long)ms);
    return true;
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

// Slot storage.

// Re-parses slot k from flash into gPresets[k], gSlotStatus[k] and gFilled[k].
void rescan(int k) {
    gSlotStatus[k] = ps::Parse(slotData(k), ps::kSlotBytes, &gPresets[k]);
    gFilled[k] = gSlotStatus[k] == ps::Status::kOk;
}

// A filled slot's image size.
size_t imageSize(int k) { return gPresets[k].has_ir ? ps::kSizeWithIr : ps::kSizeNoIr; }

// Copies filled slot k's image into dst; returns its size.
size_t copySlot(int k, uint8_t* dst) {
    const size_t n = imageSize(k);
    std::memcpy(dst, slotData(k), n);
    return n;
}

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

// writeSlot, timed into ms, then rescan(k) and settleTarget(); logs `slot write failed N` when it fails.
bool rewriteSlot(int k, uint8_t* data, size_t size, uint32_t& ms) {
    const uint32_t t0 = System::GetNow();
    const bool ok = writeSlot(k, data, size);
    ms = System::GetNow() - t0;
    rescan(k);
    settleTarget();
    if (!ok) dkusb::Log("slot write failed %d", k + 1);
    return ok;
}

// The link.

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

// Hands e to the callback, which applies it at its next block (stepParams). The ring holds 16 and the callback
// empties it every 0.5 ms, so a full ring clears at once.
void pushEdit(const dkparam::Edit& e) {
    while (!gEdits.Push(e)) {
    }
    ++gEditsSent;
}

// Returns once the callback has applied every edit pushed so far and published the result: at most a block, 0.5 ms.
// Only once audio runs (onCommand).
void awaitEdits() {
    while (gEditsDone.load(std::memory_order_acquire) != gEditsSent) {
    }
}

// The link's slot and parameter commands (docs/MIDI.md); false for a command this does not know. Each sets the RESULT
// (LIST and DUMP through serveReplies); slot changes log their line and the `slots` line.
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
            // The first STATUS comes one period later and covers only that period: the meters restart now.
            gWatchAt = System::GetNow();
            gMeterReset.store(true, std::memory_order_relaxed);
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
            uint32_t ms;
            const bool ok = rewriteSlot(s, gUpload.data(), gUpload.size(), ms);
            gUpload.Clear();
            if (ok) {
                if (gTarget == s)
                    gReload = true;
                else if (gTarget < 0)
                    retarget(s);
                dkusb::Log("upload slot %d \"%s\" %lu ms", s + 1, gPresets[s].name, (unsigned long)ms);
            }
            finishWrite(cmd, ok, ms);
            return true;
        }
        case dklink::kErase: {
            if (n != 1 || p[0] >= ps::kSlots) {
                result(cmd, Result::kBadRequest, 0);
                return true;
            }
            const int k = p[0];
            uint32_t ms;
            const bool ok = rewriteSlot(k, nullptr, 0, ms);
            if (ok) dkusb::Log("erase slot %d %lu ms", k + 1, (unsigned long)ms);
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
            const size_t size = copySlot(k, gScratch[0]);
            ps::Rename(gScratch[0], size, reinterpret_cast<const char*>(p + 1), n - 1);
            uint32_t ms;
            const bool ok = rewriteSlot(k, gScratch[0], size, ms);
            if (ok) dkusb::Log("rename slot %d \"%s\" %lu ms", k + 1, gPresets[k].name, (unsigned long)ms);
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
            // Both writes come before any rescan or settleTarget, which would retarget away from a half-swapped pair.
            const size_t sizeA = gFilled[a] ? copySlot(a, gScratch[0]) : 0;
            const size_t sizeB = gFilled[b] ? copySlot(b, gScratch[1]) : 0;
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
            std::memcpy(gReadBuf, slotData(k) + off, len);
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
            const size_t size = copySlot(k, gScratch[0]);
            ps::SetGate(gScratch[0], size, g);
            uint32_t ms;
            const bool ok = rewriteSlot(k, gScratch[0], size, ms);
            if (ok) {
                if (k == gCur && gFilled[k]) publishGate(gPresets[k].gate);
                dkusb::Log("gate slot %d %s %lu ms", k + 1, g.enabled ? "on" : "off", (unsigned long)ms);
            }
            finishWrite(cmd, ok, ms);
            return true;
        }
        case dklink::kDump:
            if (n != 0) {
                result(cmd, Result::kBadRequest, 0);
            } else {
                awaitEdits();   // the VALUES includes every SET and ASSIGN answered so far
                gDumpNext = 0;  // serveReplies sends the PARAMs and the VALUES, then the RESULT
            }
            return true;
        case dklink::kSet: {
            if (n != 3 || p[0] >= dkparam::kParamCount) {
                result(cmd, Result::kBadRequest, 0);
                return true;
            }
            // Sent while a slider moves: no log line.
            const dkparam::ParamDef& d = dkparam::kParams[p[0]];
            const float v = dkparam::FromCode(d, dklink::GetSeptets(p + 1, 2));
            pushEdit({dkparam::EditKind::kSet, p[0], 0, v});
            result(cmd, Result::kOk, dkparam::ToCode(d, v));
            return true;
        }
        case dklink::kAssign: {
            if (n != 2 || !dkparam::Accepts(p[0], p[1])) {
                result(cmd, Result::kBadRequest, 0);
                return true;
            }
            pushEdit({dkparam::EditKind::kAssign, p[0], p[1], 0.0f});
            dkusb::Log("assign %s %s", dkparam::kControlNames[p[0]],
                       p[1] == dkparam::kNoParam ? "none" : dkparam::kParams[p[1]].name);
            result(cmd, Result::kOk, 0);
            return true;
        }
        case dklink::kSave: {
            if (n != 0) {
                result(cmd, Result::kBadRequest, 0);
                return true;
            }
            awaitEdits();
            uint32_t ms;
            const bool ok = saveSettings(ms);
            result(cmd, ok ? Result::kOk : Result::kFlash, ok ? ms : 0);
            return true;
        }
        default: return false;
    }
}

static_assert(sizeof(dklink::StatusFrame::knob) / sizeof(uint16_t) == dkboard::kKnobs &&
                  sizeof(dklink::StatusFrame::toggle) == dkboard::kToggles,
              "STATUS carries every knob and toggle");

// VALUES' fields, from what the callback last published.
dklink::ValuesFrame valuesFrame() {
    dklink::ValuesFrame v;
    for (int c = 0; c < dkparam::kControlSlots; ++c) v.assign[c] = gAssignPub[c].load(std::memory_order_relaxed);
    v.held = gHeldPub.load(std::memory_order_relaxed);
    for (int i = 0; i < dkparam::kParamCount; ++i)
        v.code[i] = dkparam::ToCode(dkparam::kParams[i], gParamPub[i].load(std::memory_order_relaxed));
    return v;
}

// Once per main-loop pass, before dkusb::Serve: LIST's SLOTs then its RESULT, DUMP's PARAMs and VALUES then its
// RESULT, READ's DATA, the pending RESULT, and the STATUS, then a VALUES, when WATCH is on and kWatchPeriodMs has
// passed. Whatever does not fit in Tx() waits for a later pass, in order.
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
    while (gDumpNext >= 0 && gDumpNext < dkparam::kParamCount) {
        if (!tx.AppendParam(dkparam::kParams[gDumpNext])) break;
        ++gDumpNext;
    }
    if (gDumpNext == dkparam::kParamCount && tx.AppendValues(valuesFrame())) ++gDumpNext;
    if (gDumpNext == dkparam::kParamCount + 1 && !gResultDue) {
        result(dklink::kDump, dklink::Result::kOk, dkparam::kParamCount);
        gDumpNext = -1;
    }
    if (gReadDue && tx.AppendData(gReadSlot, gReadOff, dkbin::Crc32(gReadBuf, gReadLen), gReadBuf, gReadLen))
        gReadDue = false;
    // A RESULT comes last: after READ's DATA, and never inside a DUMP's PARAMs and VALUES.
    const bool dumping = gDumpNext >= 0 && gDumpNext <= dkparam::kParamCount;
    if (gResultDue && !gReadDue && !dumping && tx.AppendResult(gResultCmd, gResultCode, gResultValue))
        gResultDue = false;
    if (gWatch && now - gWatchAt >= dklink::kWatchPeriodMs) {
        dklink::StatusFrame s;
        s.load_avg = gLoadAvgPm;
        s.load_max = gLoadMaxPm;
        s.underruns = gUnderruns.load(std::memory_order_relaxed);
        s.slot = gCur < 0 ? 0x7F : uint8_t(gCur);
        s.muted = gMutedPub.load(std::memory_order_relaxed);
        for (int k = 0; k < dkboard::kKnobs; ++k)
            s.knob[k] = uint16_t(std::clamp(lroundf(gKnob[k].load(std::memory_order_relaxed) * 1000.0f), 0L, 1000L));
        for (int t = 0; t < dkboard::kToggles; ++t) s.toggle[t] = gToggle[t].load(std::memory_order_relaxed);
        s.footswitches = gFsHeld.load(std::memory_order_relaxed);
        s.gate_level = tenthsBelow(gGateLevel.load(std::memory_order_relaxed));
        s.gate_cut = tenthsBelow(gGateGain.load(std::memory_order_relaxed));
        s.comp_cut = tenthsBelow(gCompGain.load(std::memory_order_relaxed));
        if (tx.AppendStatus(s)) {
            gWatchAt = now;
            gMeterReset.store(true, std::memory_order_relaxed);
            gValuesDue = true;
        }
    }
    if (gValuesDue && tx.AppendValues(valuesFrame())) gValuesDue = false;
}

}  // namespace

int main() {
    dkfw::Start(hw, dkdsp::kBlockSize, SaiHandle::Config::SampleRate::SAI_48KHZ);
    gLed1.Init(dkboard::Board::kLed1, false, dkdsp::kBlockRate);
    gLed2.Init(dkboard::Board::kLed2, false, dkdsp::kBlockRate);

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

    for (int k = 0; k < ps::kSlots; ++k) rescan(k);
    gStateIndex = ps::NewestState(mapped(ps::kStateOffset[0]), mapped(ps::kStateOffset[1]), gStateSeq, gStateSlot);
    if (gStateIndex < 0) gStateSlot = -1;
    gCur = ps::BootSlot(gFilled, gStateSlot);
    retarget(gCur);
    loadPreset(gCur);
    takeNextPreset();
    gSettingsIndex = dkparam::NewestSettings(mapped(dkparam::kSettingsOffset[0]), mapped(dkparam::kSettingsOffset[1]),
                                             gSettingsSeq);
    gStore.Init(gSettingsIndex < 0 ? dkparam::FactorySettings()
                                   : dkparam::DecodeSettings(mapped(dkparam::kSettingsOffset[gSettingsIndex])));
    publishParams();
    logBoot();

    gSwapFade.Init(false);
    gMute.Init(false);
    gInKnob.Init(dkdsp::kBlockRate, dkdsp::kKnobSmoothS, 0.5f);
    gInGain.Init(gInCal);
    // The output and the send fade in from silence at boot.
    gOutKnob.Init(dkdsp::kBlockRate, dkdsp::kKnobSmoothS, 0.0f);
    gOutGain.Init(0.0f);
    gLoop = uint8_t(gStore.Value(dkparam::kLoopMode));
    gModeFade.Init(false);
    gSendKnob.Init(dkdsp::kBlockRate, dkdsp::kKnobSmoothS, gStore.Value(dkparam::kLoopSend));
    gReturnKnob.Init(dkdsp::kBlockRate, dkdsp::kKnobSmoothS, gStore.Value(dkparam::kLoopReturn));
    gSendGain.Init(0.0f);
    gReturnGain.Init(dkdsp::InputGain(gStore.Value(dkparam::kLoopReturn)));
    gLoad.Init(hw.seed.AudioSampleRate(), hw.seed.AudioBlockSize());
    gBlockTicks = uint32_t(uint64_t(System::GetTickFreq()) * dkdsp::kBlockSize / uint32_t(dkdsp::kSampleRate));
    for (dkctl::ToggleFilter& f : gToggleFilter) f.Init(1);
    // The compressor and the EQ start settled at the parameters' values; a control's first reading reaches them
    // through the smoothing.
    gComp.Init(gStore.Values());
    gEq.Init(gStore.Values());
    hw.seed.adc.Start();
    hw.seed.StartAudio(AudioCallback);

    uint32_t lastLog = System::GetNow();
    for (;;) {
        serveReplies(System::GetNow());
        // Slot commands block for their flash writes: the time is read after them.
        dkusb::Serve(logBoot, onCommand);
        const uint32_t now = System::GetNow();
        servePresets(now);
        if (uint8_t(gParamPub[dkparam::kLoopMode].load(std::memory_order_relaxed)) != gLoopLogged) logLoop();
        if (now - lastLog >= 1000) {
            lastLog = now;
            logControls();
            logStatus();
        }
    }
}
