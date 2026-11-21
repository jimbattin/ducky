// Ducky pedal firmware: the xfade build (APP=xfade, DK_MORPH_XFADE=1, the default) and the weight build (APP=weight,
// DK_MORPH_XFADE=0) on the board board.h names. docs/FIRMWARE.md "Threads" describes the callback's steps, the main
// loop and every handoff between them; "Memory" the placement; docs/LOOP.md the routing per loop_mode.
//
// Chain: L In -> compressor (dsp/compressor.h or dsp/ota_compressor.h by comp_type) -> Knob 1's trim x the model's
// input calibration -> the preset's A2-Lite model (dsp/a2lite.h) -> its cab IR (dsp/convolver.h) -> EQ (dsp/eq.h) ->
// Knob 2's volume x the level match x the preset's noise gate (dsp/gain.h, dsp/noise_gate.h; the gate listens to L In
// before the compressor) -> FS1's mute -> the preset switch's fade -> the loop mode's fade. MISO copies L Out to R Out;
// the loop modes send on R Out and return on R In before the model (pre-model) or between the model and the IR
// (post-model). With no valid slot the pedal plays dry: no model, IR or gate.
//
// Morph: the xfade build runs the preset's model and its morph target's in two engines and crossfades their outputs
// at equal power (runXfade); the first overrun stops the second engine until the next load (`morph shed`). The weight
// build runs one engine on the blend of the two models' weights (stepMorph, morph.h).
//
// Two threads share single-writer atomics and the gEdits ring only:
// - AudioCallback (the audio DMA interrupt, in ITCM): controls, parameter edits (param_store.h), the preset switch,
//   the loop and morph steps, the chain, the meters, the LEDs, the load.
// - main(): the link (usb_link.h; onCommand, serveReplies), the preset switch (servePresets), the QSPI writes
//   (writeFlash), the log lines.
// A preset switch: gSwapReq -> the callback fades out and parks (gSwapParked) -> loadPreset -> gSwapLoaded -> the
// callback takes gNext* and fades in. FPSCR.FZ is set for the main loop only: the callback's interrupt context takes
// FPDSCR's 0 (docs/M7_PERFORMANCE.md "Core state").
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
#include "dsp/ota_compressor.h"
#include "firmware.h"
#include "fw_image.h"
#include "ir.h"
#include "memory.h"
#include "midi.h"
#include "model.h"
#include "morph.h"
#include "param_store.h"
#include "params.h"
#include "preset.h"
#include "settings.h"
#include "spsc_ring.h"
#include "upload.h"
#include "usb_link.h"

using namespace daisy;

// 1: the xfade build (the crossfade morph, the default); 0: the weight build (the weight morph, APP=weight).
#ifndef DK_MORPH_XFADE
#define DK_MORPH_XFADE 1
#endif

namespace {

namespace ps = dkpreset;

// The image's tag (fw_image.h, the linker script's .ducky_image): a FIRMWARE update accepts only tagged images, and
// every tagged image serves FIRMWARE, so an update never installs an image that cannot update itself.
[[gnu::used, gnu::section(".ducky_image")]] const dkfwimg::Tag kImageTag = {
    dkfwimg::kTagMagic, dkfwimg::kTagVersion, dkboard::kBoardId, DK_MORPH_XFADE ? 0u : dklink::kIdentityWeightMorph};

dkboard::Board hw;

// Audio callback only (after boot).
DK_DTCM Led gLed1, gLed2;  // Update once per block (Init at the block rate)
DK_DTCM CpuLoadMeter gLoad;
DK_DTCM uint32_t gBlockTicks;  // System::GetTick() ticks in one block period (set before StartAudio)
DK_DTCM dkctl::ToggleFilter gToggleFilter[dkboard::kToggles];
DK_DTCM dkdsp::MuteRamp gMute;
DK_DTCM_BSS dkdsp::A2Lite gEngine;  // the main loop loads it while the callback is parked (and before StartAudio)
#if DK_MORPH_XFADE
// Both engines in DTCM with their rings (Bind), but for the three rings of dilation 239 (layers 6, 13 and 22: 44 KB
// an engine, the fewest cache lines read per byte), which DTCM has no room for: AXI SRAM (.bss).
constexpr uint32_t kFarRings = dkdsp::a2::LayersOfDilation(239);
constexpr size_t kNearFloats = dkdsp::a2::PoolFloats(dkdsp::a2::kAllLayers & ~kFarRings);
constexpr size_t kFarFloats = dkdsp::a2::PoolFloats(kFarRings);
DK_DTCM_BSS float gNearA[kNearFloats], gNearB[kNearFloats];
float gFarA[kFarFloats], gFarB[kFarFloats];
DK_DTCM_BSS dkdsp::A2Lite gEngineB;  // the morph target's model; loaded as gEngine is
#else
DK_DTCM_BSS float gHist[dkdsp::a2::kHistFloats];  // gEngine's rings (Bind)
#endif
DK_DTCM float gLevel = 1.0f;  // the current model's level match (the morph's blend of two)
DK_DTCM float gInCal = 1.0f;  // the current model's input calibration (likewise)
DK_DTCM bool gMorphOn = false;               // the current preset has a morph target (gMorphW, or gEngineB, is its)
DK_DTCM dkdsp::Smoother gMorphKnob;          // morph's position
#if DK_MORPH_XFADE
DK_DTCM float gLevelB = 1.0f;     // with gMorphOn: the target model's level match (gLevel is the preset's)
DK_DTCM float gCalRatioB = 1.0f;  // with gMorphOn: the target's input calibration over the preset's
DK_DTCM float gMixGainA = 1.0f, gMixGainB = 0.0f;  // stepMorph's: each model's level match times its mix share
DK_DTCM dkdsp::GainRamp gMixA, gMixB;
DK_DTCM float gBufB[dkdsp::kBlockSize];  // engine B's block
DK_DTCM bool gShed = false;              // an overrun stopped engine B until the next preset load
#else
DK_DTCM float gLevelDb[2], gInCalDb[2];      // with gMorphOn: gLevel and gInCal in dB, the preset's and its target's
DK_DTCM float gMorphApplied = -1.0f;         // the blend position the engine, gLevel and gInCal hold; -1: none yet
// A blend position this close to the applied one is not applied (a resting knob's noise), unless it is an end.
constexpr float kMorphDeadband = 0.001f;
#endif
DK_DTCM dkdsp::Smoother gInKnob, gOutKnob;  // knobs 1 and 2
DK_DTCM dkdsp::GainRamp gInGain, gOutGain;  // Knob 1's trim times gInCal; Knob 2's volume times gLevel
DK_DTCM float gPeakAcc;        // input peak since the main loop last read gPeak
DK_DTCM float gOutPeakAcc;     // output peak since the main loop last read gOutPeak
DK_DTCM bool gMuted = false;   // FS1's state; not persisted
DK_DTCM_BSS dkdsp::IrConvolver gIr;  // Init before StartAudio; the main loop loads it as it does gEngine
alignas(32) float gIrPool[dkdsp::kIrTailPoolFloats];  // gIr's tail spectra: AXI SRAM (.bss), on D-cache lines
DK_DTCM bool gIrOn = false;    // the current preset has an IR
DK_DTCM bool gModelOn = false;  // a preset is loaded: false plays dry (taken from gNextModelOn when fading in)
DK_DTCM dkdsp::Compressor gComp;
DK_DTCM dkdsp::OtaCompressor gOta;
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
SpscRing<dkparam::Edit, 16> gEdits;        // SET, ASSIGN and CC 11 (onCommand, onControlChange), applied by the
                                           // callback's next block
float gNextLevel = 1.0f, gNextInCal = 1.0f;  // the loaded preset's gains, IR and model, published by gSwapLoaded
bool gNextIrOn = false, gNextModelOn = false;
// The loaded preset's morph, published by gSwapLoaded: whether it has a target, then
bool gNextMorphOn = false;
#if DK_MORPH_XFADE
// the target model's level match and its input calibration over the preset's.
float gNextLevelB = 1.0f, gNextCalRatioB = 1.0f;
std::atomic<uint32_t> gShedCount{0};  // callback -> main loop: the overruns that stopped engine B, since boot
#else
// the two models' gains in dB (the preset's, the target's) and the path's weights. AXI SRAM: the callback reads
// gMorphW only while not parked.
float gNextLevelDb[2], gNextInCalDb[2];
dkmorph::Weights gMorphW;
#endif

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
int gMorphFrom = -1;           // the slot the loaded preset morphs with (ps::ResolveMorph at its load); -1 = none
#if DK_MORPH_XFADE
uint32_t gShedLogged = 0;      // the gShedCount the last `morph shed` line reported
#else
DK_SDRAM float gMorphScratch[dkdsp::a2::kWeightCount];  // dkmorph::Prepare's
#endif

// The link's slot and parameter commands (onCommand, serveReplies). Main loop only.
DK_SDRAM uint8_t gUploadBuf[ps::kSizeMax];
dklink::Upload gUpload(gUploadBuf);
DK_SDRAM uint8_t gFwBuf[dkfwimg::kMaxImageBytes];  // FIRMWARE's staging
dklink::FirmwareUpload gFwUpload(gFwBuf);
bool gRestartDue = false;  // FIRMWARE COMMIT wrote an image: restart into it once its RESULT is out
DK_SDRAM uint8_t gScratch[2][ps::kSizeMax];  // RENAME's, SWAP's, GATE's and MORPH's copies of slot images
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
uint8_t gExpChannel = dkmidi::kOmni;      // CC 11's and CC 102's channel (EXP CHANNEL, the settings record)
int gExpCc = -1;                          // the last CC 11 value taken, -1 before the first (the ctl line)
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

// The current preset's morph: `morph slot N fit F U % waypoints K "NAME"` (the slot it morphs with, the block's fit
// and unaligned fit in percent, its path's waypoint count, that preset's name, last as in logModel); `morph missing
// slot N` when no slot holds the model the block was fitted to (N: the block's target); `morph none` without a
// block, or dry. The xfade build: `morph slot N xfade "NAME"` for a target.
void logMorph() {
    if (gCur < 0 || !gPresets[gCur].has_morph) {
        dkusb::Log("morph none");
        return;
    }
    const dkmorph::Block& m = gPresets[gCur].morph;
    if (gMorphFrom < 0) {
        dkusb::Log("morph missing slot %d", m.target + 1);
        return;
    }
#if DK_MORPH_XFADE
    dkusb::Log("morph slot %d xfade \"%s\"", gMorphFrom + 1, gPresets[gMorphFrom].name);
#else
    dkusb::Log("morph slot %d fit %d %d %% waypoints %d \"%s\"", gMorphFrom + 1, pct(m.fit), pct(m.unaligned_fit),
               gPresets[gCur].waypoint_count, gPresets[gMorphFrom].name);
#endif
}

// The current preset (1-based), with its load time when loadUs > 0, then its model, IR, gate and morph; "preset none"
// when dry.
void logPreset(uint32_t loadUs) {
    if (gCur < 0) {
        dkusb::Log("preset none");
        logModel(nullptr);
        logIr(nullptr);
        logNoiseGate(nullptr);
        logMorph();
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
    logMorph();
}

// The weight build's line ends " weight".
void logBootLine() {
    dkusb::Log("ducky %s seed %s sysclk %u MHz audio %u Hz %u bit block %u %s%s", dkboard::kName,
               dkfw::SeedName(hw.seed.CheckBoardVersion()), unsigned(System::GetSysClkFreq() / 1000000),
               unsigned(hw.seed.AudioSampleRate()), bitDepth(hw.seed.AudioSaiHandle().GetConfig().bit_depth),
               unsigned(hw.seed.AudioBlockSize()), dkfw::kBuildId, DK_MORPH_XFADE ? "" : " weight");
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

// The expression channel as the log writes it: `omni` or 1-16 (formatted into buf).
const char* expChannelText(char (&buf)[4]) {
    if (gExpChannel == dkmidi::kOmni) return "omni";
    std::snprintf(buf, sizeof buf, "%u", unsigned(gExpChannel));
    return buf;
}

void logExpChannel() {
    char b[4];
    dkusb::Log("exp channel %s", expChannelText(b));
}

// The settings record in use (`settings factory` with none), the live layout: each knob and switch slot's parameter,
// then the expression slot's with its channel, and the loop mode. Seven parameters on one control each fit the layout
// line; a longer layout (a parameter on several controls) is cut at 120 characters.
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
    char b[4];
    dkusb::Log("exp %s channel %s", n[dkparam::kExpressionSlot], expChannelText(b));
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

// The controls once a second: the knobs (%), the toggles, the footswitches held and the last CC 11 value (`-` before
// the first).
void logControls() {
    int k[dkboard::kKnobs];
    for (int i = 0; i < dkboard::kKnobs; ++i) k[i] = pct(gKnob[i].load(std::memory_order_relaxed));
    const uint8_t fs = gFsHeld.load(std::memory_order_relaxed);
    char e[12] = "-";
    if (gExpCc >= 0) std::snprintf(e, sizeof e, "%d", gExpCc);
    dkusb::Log("ctl k %d %d %d %d %d %d sw %u %u %u fs %d %d exp %s", k[0], k[1], k[2], k[3], k[4], k[5],
               unsigned(gToggle[0].load(std::memory_order_relaxed)),
               unsigned(gToggle[1].load(std::memory_order_relaxed)),
               unsigned(gToggle[2].load(std::memory_order_relaxed)), fs & 1, (fs >> 1) & 1, e);
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
    gMorphOn = gNextMorphOn;
#if DK_MORPH_XFADE
    gLevelB = gNextLevelB;
    gCalRatioB = gNextCalRatioB;
    gShed = false;
#else
    for (int i = 0; i < 2; ++i) {
        gLevelDb[i] = gNextLevelDb[i];
        gInCalDb[i] = gNextInCalDb[i];
    }
    gMorphApplied = -1.0f;  // the engine holds the preset's own model: the next block blends
#endif
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
    gCompGainAcc = std::fmin(gCompGainAcc, std::fmin(gComp.gain(), gOta.gain()));
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

#if DK_MORPH_XFADE
DK_DTCM float gMixT = -1.0f, gMixCos = 1.0f, gMixSin = 0.0f;  // the position stepMorph's cosine and sine are of

// Step 6, first: the crossfade's gains. The blend position follows the morph parameter (smoothed every block, so it
// stays settled); with a morph target each model's gain is its equal-power share of the position (computed when the
// position moves) times its level match.
DK_ITCM void stepMorph(float value, bool parked) {
    const float t = dkmorph::BlendPosition(gMorphKnob.Process(value));
    if (parked || !gMorphOn) return;
    if (t != gMixT) {
        constexpr float kHalfPi = 1.57079633f;
        const float angle = kHalfPi * t;
        gMixCos = cosf(angle);
        gMixSin = sinf(angle);
        gMixT = t;
    }
    gMixGainA = gLevel * gMixCos;
    gMixGainB = gLevelB * gMixSin;
}

// Both models on buf: B's input is A's times the ratio of their input calibrations; each output times its ramped mix
// gain (stepMorph), summed into buf.
DK_ITCM void runXfade(float* buf, size_t n) {
    for (size_t i = 0; i < n; ++i) gBufB[i] = buf[i] * gCalRatioB;
    gEngine.Process(buf, buf, n);
    gEngineB.Process(gBufB, gBufB, n);
    gMixA.Process(buf, buf, n, gMixGainA);
    gMixB.Process(gBufB, gBufB, n, gMixGainB);
    for (size_t i = 0; i < n; ++i) buf[i] += gBufB[i];
}
#else
// Step 6, first: the morph (morph.h). The blend position follows the morph parameter (smoothed every block, so it stays
// settled). With a morph target, a position that has moved makes the engine's weights the blend of the two nodes of
// the path's segment it lies in (the preset's model and its aligned target when the path has no waypoints), and the
// level match and the input calibration the blend of the two models' in dB: before the block's gains are computed.
// The histories stay: the path's networks share their coordinates.
DK_ITCM void stepMorph(float value, bool parked) {
    const float t = dkmorph::BlendPosition(gMorphKnob.Process(value));
    if (parked || !gMorphOn) return;
    const float moved = std::fabs(t - gMorphApplied);
    if (moved > kMorphDeadband || (moved > 0.0f && (t == 0.0f || t == 1.0f))) {
        const dkmorph::Segment g = dkmorph::SegmentAt(t, gMorphW.segments);
        gEngine.Blend(gMorphW.a[g.s], gMorphW.d[g.s], g.f);
        gLevel = dkdsp::DbToGain(gLevelDb[0] + t * (gLevelDb[1] - gLevelDb[0]));
        gInCal = dkdsp::DbToGain(gInCalDb[0] + t * (gInCalDb[1] - gInCalDb[0]));
        gMorphApplied = t;
    }
}
#endif

DK_ITCM void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    gLoad.OnBlockStart();
    const uint32_t start = System::GetTick();
    uint8_t tog[dkboard::kToggles];
    readControls(tog);
    stepParams(tog);
    stepSwap();
    stepLoop();
    const bool parked = gSwap == Swap::kParked;  // the main loop owns the engine, the IR and the morph weights
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
    stepMorph(v[dkparam::kMorph], parked);
    const float sendPos = gSendKnob.Process(v[dkparam::kLoopSend]);
    const float returnPos = gReturnKnob.Process(v[dkparam::kLoopReturn]);
    const float inTarget = gInCal * dkdsp::InputGain(gInKnob.Process(hw.Knob(0)));
    const float vol = dkdsp::OutputGain(gOutKnob.Process(hw.Knob(1)));
    const bool pre = gLoop == dkparam::kLoopPre, post = gLoop == dkparam::kLoopPost;
    // In pre-model the compressor's output, times the send level, is the send: independent of the preset, so the swap
    // fade leaves it alone. The return feeds the rest of the chain. The type comp_type does not pick is a bit-exact
    // bypass; a switch runs both for 20 ms, one fading out while the other fades in.
    float* const comp = pre ? out[1] : out[0];
    gComp.Process(in[0], comp, size, v);
    gOta.Process(comp, comp, size, v);
    if (pre) {
        gSendGain.Process(out[1], out[1], size, dkdsp::InputGain(sendPos));
        gReturnGain.Process(in[1], out[0], size, dkdsp::InputGain(returnPos));
    }
    gInGain.Process(out[0], out[0], size, inTarget);
#if DK_MORPH_XFADE
    const bool xfading = !parked && gModelOn && gMorphOn && !gShed;
    if (xfading)
        runXfade(out[0], size);
    else if (!parked && gModelOn)
        gEngine.Process(out[0], out[0], size);
    const float level = xfading ? 1.0f : gLevel;  // the crossfade's gains carry both level matches
#else
    if (!parked && gModelOn) gEngine.Process(out[0], out[0], size);
    const float level = gLevel;
#endif
    if (post) {
        // The send: the model's output times the level match, the gate and the send level. The return feeds the IR.
        gSendGain.Process(out[0], out[1], size, level * gate * dkdsp::InputGain(sendPos));
        gReturnGain.Process(in[1], out[0], size, dkdsp::InputGain(returnPos));
    }
    // In MISO gSendGain and gReturnGain rest; a loop mode resumes them behind the mode fade.
    if (parked)
        std::fill(out[0], out[0] + size, 0.0f);
    else if (gModelOn && gIrOn)
        gIr.Process(out[0], out[0]);  // size is kBlockSize: dkfw::Start sets the block size
    gEq.Process(out[0], size, v);
    // Knob 2's volume, times the level match and the gate unless they rode the send.
    gOutGain.Process(out[0], out[0], size, post ? vol : level * vol * gate);
    // FS1's mute (L Out only), the preset switch's fade (in post-model the send's too: both stay silent while parked)
    // and, step 7, MISO's copy to R Out and the mode fade on both outputs.
    const bool miso = gLoop == dkparam::kLoopMiso;
    if (gMute.Settled() && gSwapFade.Settled() && gModeFade.Settled()) {
        // Every fade rests at 0 or 1: one product scales as the three passes would, bit for bit, and 1 needs none.
        const float g0 = gMute.Gain() * gSwapFade.Gain() * gModeFade.Gain();
        if (g0 != 1.0f)
            for (size_t i = 0; i < size; ++i) out[0][i] *= g0;
        if (miso) {
            dkdsp::CopyFloats(out[1], out[0], size);
        } else {
            const float g1 = (post ? gSwapFade.Gain() : 1.0f) * gModeFade.Gain();
            if (g1 != 1.0f)
                for (size_t i = 0; i < size; ++i) out[1][i] *= g1;
        }
    } else {
        gMute.Process(out[0], out[0], size);
        if (post)
            gSwapFade.ProcessPair(out[0], out[1], size);
        else
            gSwapFade.Process(out[0], out[0], size);
        if (miso) dkdsp::CopyFloats(out[1], out[0], size);
        gModeFade.ProcessPair(out[0], out[1], size);
    }
    if (gSwap == Swap::kFadingOut && gSwapFade.Silent()) {
        gSwap = Swap::kParked;
        gSwapParked.store(gSwapSeen, std::memory_order_release);
    }
    // In MISO R Out is L Out bit for bit: one peak serves both meters.
    const float outPeak = dkdsp::Peak(out[0], size);
    publishMeters(peak, outPeak, dkdsp::Peak(in[1], size), miso ? outPeak : dkdsp::Peak(out[1], size), gate);
    driveLeds();
    // Step 9.
    if (System::GetTick() - start > gBlockTicks) {
        gUnderruns.fetch_add(1, std::memory_order_relaxed);
#if DK_MORPH_XFADE
        // Two engines do not fit the block: the target's stops (the output steps to the preset's model at its full
        // level) until the next preset load.
        if (xfading) {
            gShed = true;
            gShedCount.fetch_add(1, std::memory_order_relaxed);
        }
#endif
    }
    // A Reset just before OnBlockEnd takes its first-cycle path: the published loads are never NaN.
    if (gLoadReset.exchange(false, std::memory_order_relaxed)) gLoad.Reset();
    gLoad.OnBlockEnd();
    gLoadAvg.store(gLoad.GetAvgCpuLoad(), std::memory_order_relaxed);
    gLoadMax.store(gLoad.GetMaxCpuLoad(), std::memory_order_relaxed);
}

// Presets.

// The engine and the IR from gPresets[slot], its gains, IR and model state into gNext*, and its morph (the target
// ps::ResolveMorph finds now, gMorphFrom) into gMorphW and gNext* (the xfade build: the target's model into gEngineB);
// slot < 0 (dry) only sets gNext* to no model, no IR, no morph, unity gains. Only while the callback is parked or
// before StartAudio.
void loadPreset(int slot) {
    publishGate(slot < 0 ? dkdsp::kGateOff : gPresets[slot].gate);
    gMorphFrom = -1;
    gNextMorphOn = false;
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
    gMorphFrom = ps::ResolveMorph(gPresets, gFilled, slot);
    if (gMorphFrom < 0) return;
    const dkmodel::Model& b = gPresets[gMorphFrom].model;
#if DK_MORPH_XFADE
    gEngineB.Load(b.weights, b.head_scale);
    gNextLevelB = levelOf(b);
    gNextCalRatioB = calOf(b) / gNextInCal;
#else
    dkmorph::Prepare(p.morph.t, p.model, b, p.waypoints[0], p.waypoint_count, gMorphScratch, &gMorphW);
    gNextLevelDb[0] = 20.0f * log10f(gNextLevel);
    gNextLevelDb[1] = 20.0f * log10f(levelOf(b));
    gNextInCalDb[0] = 20.0f * log10f(gNextInCal);
    gNextInCalDb[1] = 20.0f * log10f(calOf(b));
#endif
    gNextMorphOn = true;
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

// Erases QSPI [off, off + span), writes data[0, n) at off, invalidates the D-cache over the span and verifies the
// result through the memory-mapped QSPI: the data, or 0xFF throughout for data == nullptr. The callback reads no
// QSPI, so the blocking erase and write do not stall audio. libDaisy's 64 KB block erase leaves the QSPI in indirect
// mode, where reading the mapped region stops the pedal (an ERASE without the fix never answered); its Write and its
// 4 KB sector erase return to memory-mapped mode, so an erase without data ends with a sector erase of the (already
// erased) first 4 KB.
bool writeFlash(uint32_t off, uint32_t span, const uint8_t* data, size_t n) {
    bool ok = hw.seed.qspi.Erase(off, off + span) == QSPIHandle::Result::OK;
    if (ok) {
        // libDaisy's API is not const-correct: Write only reads the buffer.
        ok = data != nullptr
                 ? hw.seed.qspi.Write(off, uint32_t(n), const_cast<uint8_t*>(data)) == QSPIHandle::Result::OK
                 : hw.seed.qspi.EraseSector(off) == QSPIHandle::Result::OK;
    }
    SCB_InvalidateDCache_by_Addr(reinterpret_cast<uint32_t*>(ps::kQspiBase + off), int32_t(span));
    if (!ok) return false;
    if (data != nullptr) return std::memcmp(mapped(off), data, n) == 0;
    const uint32_t* w = reinterpret_cast<const uint32_t*>(mapped(off));
    for (size_t i = 0; i < span / 4; ++i)
        if (w[i] != 0xFFFFFFFFu) return false;
    return true;
}

// Writes slot as the newest state record, into the state sector not holding the newest one (writeFlash). On a
// failure the previous record stays the newest; the next switch tries again.
void writeState(int slot) {
    const uint32_t t0 = System::GetNow();
    const int idx = gStateIndex == 0 ? 1 : 0;
    const uint32_t off = ps::kStateOffset[idx];
    uint8_t buf[ps::kStateBytes];
    ps::BuildState(buf, gStateSeq + 1, slot);
    uint32_t seq = 0;
    int written = -1;
    const bool ok = writeFlash(off, ps::kStateSectorBytes, buf, sizeof buf) &&
                    ps::StateValid(mapped(off), seq, written) && seq == gStateSeq + 1 && written == slot;
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
// not holding the newest one (writeFlash); ms is the time it took. On a failure the previous record stays the newest.
bool saveSettings(uint32_t& ms) {
    const uint32_t t0 = System::GetNow();
    const int idx = gSettingsIndex == 0 ? 1 : 0;
    const uint32_t off = dkparam::kSettingsOffset[idx];
    dkparam::Settings s;
    for (int c = 0; c < dkparam::kControlSlots; ++c) s.assign[c] = gAssignPub[c].load(std::memory_order_relaxed);
    for (int i = 0; i < dkparam::kParamCount; ++i) s.value[i] = gParamPub[i].load(std::memory_order_relaxed);
    s.exp_channel = gExpChannel;
    uint8_t buf[dkparam::kSettingsBytes];
    dkparam::BuildSettings(buf, gSettingsSeq + 1, s);
    uint32_t seq = 0;
    const bool ok = writeFlash(off, ps::kStateSectorBytes, buf, sizeof buf) &&
                    dkparam::SettingsValid(mapped(off), seq) && seq == gSettingsSeq + 1;
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
// load. A loaded preset whose morph target is no longer the slot it was loaded with (the target was uploaded over,
// erased or swapped, or one appeared) reloads. kPersistDelayMs after the last switch the slot goes to the state
// record (dry is not recorded).
void servePresets(uint32_t now) {
    const uint32_t presses = gFs2Presses.load(std::memory_order_relaxed);
    if (presses != gFs2Seen && gTarget >= 0) {
        int t = gTarget;
        for (uint32_t n = presses - gFs2Seen; n > 0; --n) t = ps::NextFilled(gFilled, t);
        retarget(t);
    }
    gFs2Seen = presses;

    if (!gSwapping && gCur >= 0 && ps::ResolveMorph(gPresets, gFilled, gCur) != gMorphFrom) gReload = true;
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
size_t imageSize(int k) {
    return ps::ImageSize(gPresets[k].has_ir, gPresets[k].has_morph, gPresets[k].waypoint_count);
}

// Copies filled slot k's image into dst; returns its size.
size_t copySlot(int k, uint8_t* dst) {
    const size_t n = imageSize(k);
    std::memcpy(dst, slotData(k), n);
    return n;
}

// writeFlash over slot k's 64 KB block: data[0, size), or erased for nullptr. The caller rescans the slot.
bool writeSlot(int k, const uint8_t* data, size_t size) { return writeFlash(slotOff(k), ps::kSlotBytes, data, size); }

// FIRMWARE COMMIT: writeFlash over the 64 KB blocks from QSPI 0x90040000 that size covers (read back through MPU
// region 5); ms is the time it took. The running image is in SRAM: it takes effect at the next restart.
bool writeFirmware(const uint8_t* data, size_t size, uint32_t& ms) {
    const uint32_t t0 = System::GetNow();
    const uint32_t off = dkfwimg::kQspiOffset, end = off + ((uint32_t(size) + 0xFFFFu) & ~0xFFFFu);
    const bool same = writeFlash(off, end - off, data, size);
    ms = System::GetNow() - t0;
    return same;
}

// writeSlot, timed into ms, then rescan(k) and settleTarget(); logs `slot write failed N` when it fails.
bool rewriteSlot(int k, const uint8_t* data, size_t size, uint32_t& ms) {
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

// On EXP CHANNEL's channel (any with omni): CC 11 sets EXP's position, applied by the next block (param_store.h); CC
// 102 switches to slot value + 1 through servePresets, as FS2 does. A preset CC past the slots, or naming a slot that
// is not ok, is logged and ignored.
void onControlChange(const dkmidi::Cc& c) {
    if (!dkmidi::ChannelMatches(gExpChannel, c.channel)) return;
    if (c.number == dkmidi::kExpressionCc) {
        gExpCc = c.value;
        pushEdit({dkparam::EditKind::kExpression, 0, 0, float(c.value) / 127.0f});
    } else if (c.number == dkmidi::kPresetCc) {
        if (c.value >= ps::kSlots)
            dkusb::Log("preset cc %u ignored", unsigned(c.value));
        else if (!gFilled[c.value])
            dkusb::Log("preset cc %u ignored: slot %u %s", unsigned(c.value), unsigned(c.value) + 1,
                       ps::StatusName(gSlotStatus[c.value]));
        else
            retarget(c.value);
    }
}

// UPLOAD COMMIT: validates the staged image, writes its slot, and reloads or retargets playback.
void commitUpload(uint8_t cmd, const uint8_t*, size_t n) {
    using dklink::Result;
    if (n != 0) {
        result(cmd, Result::kBadRequest, 0);
        return;
    }
    const Result r = gUpload.Ready();
    if (r != Result::kOk) {
        result(cmd, r, 0);
        return;
    }
    const ps::Status st = gUpload.Validate(&gCheck);
    if (st != ps::Status::kOk) {
        gUpload.Clear();
        result(cmd, Result::kInvalid, uint8_t(st));
        return;
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
}

// FIRMWARE COMMIT: validates the staged image, writes it to QSPI (writeFirmware) and schedules the restart.
void commitFirmware(uint8_t cmd, const uint8_t*, size_t n) {
    using dklink::Result;
    if (n != 0) {
        result(cmd, Result::kBadRequest, 0);
        return;
    }
    const Result r = gFwUpload.Ready();
    if (r != Result::kOk) {
        result(cmd, r, 0);
        return;
    }
    const dkfwimg::Check check = gFwUpload.Validate(dkboard::kBoardId);
    if (check != dkfwimg::Check::kOk) {
        gFwUpload.Clear();
        dkusb::Log("firmware rejected %s", dkfwimg::CheckName(check));
        result(cmd, Result::kInvalid, uint8_t(check));
        return;
    }
    uint32_t ms;
    if (!writeFirmware(gFwUpload.data(), gFwUpload.size(), ms)) {
        // The staging is kept: COMMIT can be sent again, and the running image is untouched until a restart.
        dkusb::Log("firmware write failed");
        result(cmd, Result::kFlash, 0);
        return;
    }
    dkusb::Log("firmware written %lu bytes %lu ms", (unsigned long)gFwUpload.size(), (unsigned long)ms);
    result(cmd, Result::kOk, ms);
    gFwUpload.Clear();
    gRestartDue = true;
}

// ERASE: empties a slot.
void eraseSlot(uint8_t cmd, const uint8_t* p, size_t n) {
    using dklink::Result;
    if (n != 1 || p[0] >= ps::kSlots) {
        result(cmd, Result::kBadRequest, 0);
        return;
    }
    const int k = p[0];
    uint32_t ms;
    const bool ok = rewriteSlot(k, nullptr, 0, ms);
    if (ok) dkusb::Log("erase slot %d %lu ms", k + 1, (unsigned long)ms);
    finishWrite(cmd, ok, ms);
}

// RENAME: rewrites a filled slot's name.
void renameSlot(uint8_t cmd, const uint8_t* p, size_t n) {
    using dklink::Result;
    bool valid = n >= 2 && n <= 1 + dklink::kMaxName && p[0] < ps::kSlots;
    for (size_t i = 1; valid && i < n; ++i) valid = p[i] >= 0x20 && p[i] <= 0x7E;
    if (!valid) {
        result(cmd, Result::kBadRequest, 0);
        return;
    }
    const int k = p[0];
    if (!gFilled[k]) {
        result(cmd, Result::kSlotState, 0);
        return;
    }
    const size_t size = copySlot(k, gScratch[0]);
    ps::Rename(gScratch[0], size, reinterpret_cast<const char*>(p + 1), n - 1);
    uint32_t ms;
    const bool ok = rewriteSlot(k, gScratch[0], size, ms);
    if (ok) dkusb::Log("rename slot %d \"%s\" %lu ms", k + 1, gPresets[k].name, (unsigned long)ms);
    finishWrite(cmd, ok, ms);
}

// SWAP: exchanges two slots' contents; playback follows the presets.
void swapSlots(uint8_t cmd, const uint8_t* p, size_t n) {
    using dklink::Result;
    if (n != 2 || p[0] >= ps::kSlots || p[1] >= ps::kSlots || p[0] == p[1]) {
        result(cmd, Result::kBadRequest, 0);
        return;
    }
    const int a = p[0], b = p[1];
    const auto movable = [](int k) {
        return gSlotStatus[k] == ps::Status::kOk || gSlotStatus[k] == ps::Status::kEmpty;
    };
    if (!movable(a) || !movable(b)) {
        result(cmd, Result::kSlotState, 0);
        return;
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
}

// READ: copies up to kReadBytes of a filled slot's image for serveReplies' DATA.
void readSlot(uint8_t cmd, const uint8_t* p, size_t n) {
    using dklink::Result;
    if (n != 4 || p[0] >= ps::kSlots) {
        result(cmd, Result::kBadRequest, 0);
        return;
    }
    const int k = p[0];
    if (!gFilled[k]) {
        result(cmd, Result::kSlotState, 0);
        return;
    }
    const uint32_t off = dklink::GetSeptets(p + 1, 3);
    if (off >= imageSize(k)) {
        result(cmd, Result::kBadRequest, 0);
        return;
    }
    const size_t len = std::min<size_t>(dklink::kReadBytes, imageSize(k) - off);
    std::memcpy(gReadBuf, slotData(k) + off, len);
    gReadDue = true;
    gReadSlot = uint8_t(k);
    gReadOff = off;
    gReadLen = len;
    result(cmd, Result::kOk, uint32_t(off + len));
}

// GATE: writes a slot's noise gate; GATE TRY: auditions one on the playing slot.
void setGate(uint8_t cmd, const uint8_t* p, size_t n) {
    using dklink::Result;
    dkdsp::GateSettings g;
    if (n != dklink::kGatePayload || p[0] >= ps::kSlots || !dklink::ParseGate(p + 1, n - 1, g)) {
        result(cmd, Result::kBadRequest, 0);
        return;
    }
    const int k = p[0];
    if (cmd == dklink::kGateTry) {
        // An audition, sent while a slider moves: no log line.
        if (k != gCur) {
            result(cmd, Result::kSlotState, 0);
            return;
        }
        publishGate(g);
        result(cmd, Result::kOk, 0);
        return;
    }
    if (!gFilled[k]) {
        result(cmd, Result::kSlotState, 0);
        return;
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
}

// MORPH: sets or clears a slot's morph block. One byte clears the block; kMorphPayload sets it.
void setMorph(uint8_t cmd, const uint8_t* p, size_t n) {
    using dklink::Result;
    const bool set = n == dklink::kMorphPayload;
    uint8_t raw[dkmorph::kBlockBytes];
    dkmorph::Block block{};
    size_t len = 0;
    bool valid = (n == 1 || set) && p[0] < ps::kSlots;
    if (valid && set)
        valid = dklink::Unpack7(p + 1, n - 1, raw, len) && len == sizeof raw &&
                dkmorph::ParseBlock(raw, &block) && block.target != p[0];
    if (!valid) {
        result(cmd, Result::kBadRequest, 0);
        return;
    }
    const int k = p[0];
    if (!gFilled[k] || (set && (!gFilled[block.target] || gPresets[block.target].model.crc != block.model_crc))) {
        result(cmd, Result::kSlotState, 0);
        return;
    }
    copySlot(k, gScratch[0]);
    const size_t newSize = ps::SetMorph(gScratch[0], set ? raw : nullptr, gScratch[1]);
    uint32_t ms;
    const bool ok = rewriteSlot(k, gScratch[1], newSize, ms);
    if (ok) {
        // A new fit to the same target changes nothing servePresets would see: reload here.
        if (k == gTarget) gReload = true;
        if (set)
            dkusb::Log("morph slot %d -> %d %lu ms", k + 1, block.target + 1, (unsigned long)ms);
        else
            dkusb::Log("morph slot %d off %lu ms", k + 1, (unsigned long)ms);
    }
    finishWrite(cmd, ok, ms);
}

// SET: a parameter's value, applied by the next block. Sent while a slider moves: no log line.
void setParam(uint8_t cmd, const uint8_t* p, size_t n) {
    using dklink::Result;
    if (n != 3 || p[0] >= dkparam::kParamCount) {
        result(cmd, Result::kBadRequest, 0);
        return;
    }
    const dkparam::ParamDef& d = dkparam::kParams[p[0]];
    const float v = dkparam::FromCode(d, dklink::GetSeptets(p + 1, 2));
    pushEdit({dkparam::EditKind::kSet, p[0], 0, v});
    result(cmd, Result::kOk, dkparam::ToCode(d, v));
}

// ASSIGN: puts a parameter (or none) on a control slot, applied by the next block.
void assignControl(uint8_t cmd, const uint8_t* p, size_t n) {
    using dklink::Result;
    if (n != 2 || !dkparam::Accepts(p[0], p[1])) {
        result(cmd, Result::kBadRequest, 0);
        return;
    }
    pushEdit({dkparam::EditKind::kAssign, p[0], p[1], 0.0f});
    dkusb::Log("assign %s %s", dkparam::kControlNames[p[0]],
               p[1] == dkparam::kNoParam ? "none" : dkparam::kParams[p[1]].name);
    result(cmd, Result::kOk, 0);
}

// SAVE: writes the settings record once every edit so far is applied.
void saveCommand(uint8_t cmd, const uint8_t*, size_t n) {
    using dklink::Result;
    if (n != 0) {
        result(cmd, Result::kBadRequest, 0);
        return;
    }
    awaitEdits();
    uint32_t ms;
    const bool ok = saveSettings(ms);
    result(cmd, ok ? Result::kOk : Result::kFlash, ok ? ms : 0);
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
        case dklink::kUploadCommit: commitUpload(cmd, p, n); return true;
        case dklink::kFwBegin: {
            const Result r = gFwUpload.Begin(p, n);
            result(cmd, r, gFwUpload.next());
            return true;
        }
        case dklink::kFwChunk: {
            const Result r = gFwUpload.Chunk(p, n);
            result(cmd, r, gFwUpload.next());
            return true;
        }
        case dklink::kFwCommit: commitFirmware(cmd, p, n); return true;
        case dklink::kErase: eraseSlot(cmd, p, n); return true;
        case dklink::kRename: renameSlot(cmd, p, n); return true;
        case dklink::kSwap: swapSlots(cmd, p, n); return true;
        case dklink::kRead: readSlot(cmd, p, n); return true;
        case dklink::kGate:
        case dklink::kGateTry: setGate(cmd, p, n); return true;
        case dklink::kMorph: setMorph(cmd, p, n); return true;
        case dklink::kDump:
            if (n != 0) {
                result(cmd, Result::kBadRequest, 0);
            } else {
                awaitEdits();   // the VALUES includes every SET and ASSIGN answered so far
                gDumpNext = 0;  // serveReplies sends the PARAMs and the VALUES, then the RESULT
            }
            return true;
        case dklink::kSet: setParam(cmd, p, n); return true;
        case dklink::kAssign: assignControl(cmd, p, n); return true;
        case dklink::kSave: saveCommand(cmd, p, n); return true;
        case dklink::kExpChannel:
            if (n != 1 || p[0] > dkmidi::kChannels) {
                result(cmd, Result::kBadRequest, 0);
                return true;
            }
            gExpChannel = p[0];
            logExpChannel();
            result(cmd, Result::kOk, 0);
            return true;
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
    v.exp_channel = gExpChannel;
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

    // The Identity Reply's flags tell hosts which morph this image plays.
    dkusb::Init(hw.seed, DK_MORPH_XFADE ? 0 : dklink::kIdentityWeightMorph);
#if DK_MORPH_XFADE
    gEngine.Bind(gNearA, gFarA, kFarRings);
    gEngineB.Bind(gNearB, gFarB, kFarRings);
#else
    gEngine.Bind(gHist, nullptr, 0);
#endif
    if (!gIr.Init(gIrPool)) {
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
    const dkparam::Settings settings =
        gSettingsIndex < 0 ? dkparam::FactorySettings()
                           : dkparam::DecodeSettings(mapped(dkparam::kSettingsOffset[gSettingsIndex]));
    gStore.Init(settings);
    gExpChannel = settings.exp_channel;
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
    gMorphKnob.Init(dkdsp::kBlockRate, dkdsp::kKnobSmoothS, gStore.Value(dkparam::kMorph));
    gSendGain.Init(0.0f);
    gReturnGain.Init(dkdsp::InputGain(gStore.Value(dkparam::kLoopReturn)));
    gLoad.Init(hw.seed.AudioSampleRate(), hw.seed.AudioBlockSize());
    gBlockTicks = uint32_t(uint64_t(System::GetTickFreq()) * dkdsp::kBlockSize / uint32_t(dkdsp::kSampleRate));
    for (dkctl::ToggleFilter& f : gToggleFilter) f.Init(1);
    // The compressors and the EQ start settled at the parameters' values; a control's first reading reaches them
    // through the smoothing.
    gComp.Init(gStore.Values());
    gOta.Init(gStore.Values());
    gEq.Init(gStore.Values());
    hw.seed.adc.Start();
    hw.seed.StartAudio(AudioCallback);

    uint32_t lastLog = System::GetNow();
    for (;;) {
        serveReplies(System::GetNow());
        // Slot commands block for their flash writes: the time is read after them.
        dkusb::Serve(logBoot, onCommand, onControlChange);
        // FIRMWARE COMMIT's RESULT went out with this Serve (serveReplies appended it): the pending state record
        // first, then the restart into the new image.
        if (gRestartDue && !gResultDue) {
            if (gPersistPending && gCur >= 0 && gCur != gStateSlot) writeState(gCur);
            dkusb::Restart();
        }
        const uint32_t now = System::GetNow();
        servePresets(now);
#if DK_MORPH_XFADE
        const uint32_t shed = gShedCount.load(std::memory_order_relaxed);
        if (shed != gShedLogged) {
            gShedLogged = shed;
            dkusb::Log("morph shed");
        }
#endif
        if (uint8_t(gParamPub[dkparam::kLoopMode].load(std::memory_order_relaxed)) != gLoopLogged) logLoop();
        if (now - lastLog >= 1000) {
            lastLog = now;
            logControls();
            logStatus();
        }
    }
}
