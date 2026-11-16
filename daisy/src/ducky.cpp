// Ducky pedal firmware (Cleveland Music Co. HotHouse, board.h). M5: the left input goes through Knob 1's input trim,
// the A2-Lite model compiled in by daisy/Makefile MODEL= (dsp/a2lite.h), the cab IR compiled in by IR= when there is
// one (dsp/convolver.h), the EQ (dsp/filters.h: low-cut on Knobs 5-6 and SW1, the SW3 mid, high-cut on Knobs 4-3 and
// SW2), and Knob 2's output volume times the model's level match (dsp/gain.h), then FS1's mute ramp, and is copied to
// both outputs (dual mono). If the model or the IR does not parse, audio never starts: the pedal serves USB (DFU
// included) and logs "model ..." and "ir <status>" once a second.
//
// Two threads of control:
// - AudioCallback (the audio DMA interrupt, 48 kHz, 48-sample blocks): reads the controls (30 ms toggle hysteresis),
//   toggles the mute on FS1, smooths knobs 1-2, runs the trim, the engine, the IR, the EQ and the output gain, runs
//   the mute ramp, drives LED1 and measures the input and output peaks, the load and the underruns.
// - The main loop: USB (usb_link.h: Identity, HELLO, DFU) and the once-a-second log lines.
// They share only relaxed atomics, each with one writer:
// - callback -> main loop: gKnob, gToggle, gFsHeld, gMutedPub, gUnderruns, gPeak, gOutPeak, gLoadAvg, gLoadMax;
// - main loop -> callback: gPeakReset, gLoadReset.
// Memory (memory.h): AudioCallback runs from ITCM; the engine, the EQ and the callback's own state live in DTCM, the
// IR convolver (66 KB of spectra, streamed through the D-cache every block) in AXI SRAM, which the bench measured 21%
// faster for it than D2 SRAM (docs/FILTERS.md). FPSCR.FZ is on (denormals flush to zero).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "board.h"
#include "controls.h"
#include "dsp/a2lite.h"
#include "dsp/blocks.h"
#include "dsp/convolver.h"
#include "dsp/filters.h"
#include "dsp/gain.h"
#include "dsp/mute.h"
#include "firmware.h"
#include "ir.h"
#include "memory.h"
#include "model.h"
#include "usb_link.h"

using namespace daisy;

#ifndef DK_MODEL_PATH
#error "DK_MODEL_PATH (the compiled-in model binary, absolute) comes from daisy/Makefile"
#endif

// The compiled-in model. %progbits: '@' starts a comment on ARM.
asm(".section .rodata.dk_model,\"a\",%progbits\n"
    ".balign 4\n"
    ".global dk_model\n"
    "dk_model:\n"
    ".incbin \"" DK_MODEL_PATH "\"\n"
    ".global dk_model_end\n"
    "dk_model_end:\n"
    ".previous\n");
extern "C" const uint8_t dk_model[], dk_model_end[];

// The compiled-in IR, when daisy/Makefile IR= names one.
#ifdef DK_IR_PATH
asm(".section .rodata.dk_ir,\"a\",%progbits\n"
    ".balign 4\n"
    ".global dk_ir\n"
    "dk_ir:\n"
    ".incbin \"" DK_IR_PATH "\"\n"
    ".global dk_ir_end\n"
    "dk_ir_end:\n"
    ".previous\n");
extern "C" const uint8_t dk_ir[], dk_ir_end[];
constexpr bool kHaveIr = true;
#else
constexpr bool kHaveIr = false;
#endif

namespace {

static_assert(dkctl::kToggleStableBlocks * dkdsp::kBlockSize * 1000 == 30 * size_t(dkdsp::kSampleRate),
              "the toggle filter holds 30 ms");

dkboard::Board hw;

// Audio callback only (after boot).
DK_DTCM Led gLed1, gLed2;  // Update once per block (Init at the block rate)
DK_DTCM CpuLoadMeter gLoad;
DK_DTCM uint32_t gBlockTicks;  // System::GetTick() ticks in one block period (set before StartAudio)
DK_DTCM dkctl::ToggleFilter gToggleFilter[3];
DK_DTCM dkdsp::MuteRamp gMute;
DK_DTCM_BSS dkdsp::A2Lite gEngine;
DK_DTCM float gLevel = 1.0f;  // the model's level match (set before StartAudio)
DK_DTCM float gInCal = 1.0f;  // the model's input calibration (set before StartAudio)
DK_DTCM dkdsp::Smoother gInKnob, gOutKnob;  // knobs 1 and 2
DK_DTCM dkdsp::GainRamp gInGain, gOutGain;  // Knob 1's trim times gInCal; Knob 2's volume times gLevel
DK_DTCM float gPeakAcc;        // input peak since the main loop last read gPeak
DK_DTCM float gOutPeakAcc;     // output peak since the main loop last read gOutPeak
DK_DTCM bool gMuted = false;   // FS1's state; not persisted
dkdsp::IrConvolver gIr;        // AXI SRAM; Init and Load before StartAudio
DK_DTCM bool gIrOn = false;    // an IR is loaded (set before StartAudio)
DK_DTCM dkdsp::Eq gEq;

// Main loop only: written once in main before USB serves anything or audio starts.
dkmodel::Model gModel;
dkmodel::Status gModelStatus = dkmodel::Status::kOk;
dkir::Ir gIrData;
dkir::Status gIrStatus = dkir::Status::kOk;
bool gIrFft = true;  // the convolver's FFT initialised

// Shared with the main loop: in SRAM (AXI), relaxed.
// Callback -> main loop.
std::atomic<float> gKnob[6];               // knobs 1-6, 0..1
std::atomic<uint8_t> gToggle[3]{1, 1, 1};  // SW1-SW3 filtered: 0 down, 1 middle, 2 up
std::atomic<uint8_t> gFsHeld{0};           // bit 0 FS1, bit 1 FS2
std::atomic<bool> gMutedPub{false};        // gMuted
std::atomic<uint32_t> gUnderruns{0};       // blocks since boot whose callback ran longer than one block period
std::atomic<float> gPeak{0.0f};            // gPeakAcc, published every block
std::atomic<float> gOutPeak{0.0f};         // gOutPeakAcc, published every block
std::atomic<float> gLoadAvg{0.0f}, gLoadMax{0.0f};  // gLoad's, published every block
// Main loop -> callback.
std::atomic<bool> gPeakReset{false};  // restart gPeakAcc and gOutPeakAcc
std::atomic<bool> gLoadReset{false};  // restart gLoad

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

// The compiled-in model's loudness, level match, input level, input calibration and name, or its parse status. The
// name is last: Log truncates at 120 characters and a name can take 63.
void logModel() {
    if (gModelStatus != dkmodel::Status::kOk) {
        dkusb::Log("model %s", dkmodel::StatusName(gModelStatus));
        return;
    }
    const bool hasLoudness = (gModel.flags & dkmodel::kFlagHasLoudness) != 0;
    const bool hasInput = (gModel.flags & dkmodel::kFlagHasInputLevel) != 0;
    char loudness[12] = "none", level[12], input[12] = "none", cal[12];
    if (hasLoudness) tenthsDb(loudness, gModel.loudness);
    if (hasInput) tenthsDb(input, gModel.input_level_dbu);
    tenthsDb(level, 20.0f * log10f(gLevel));
    tenthsDb(cal, 20.0f * log10f(gInCal));
    dkusb::Log("model loudness %s%s level %s dB input %s%s cal %s dB \"%s\"", loudness, hasLoudness ? " dB" : "",
               level, input, hasInput ? " dBu" : "", cal, gModel.name);
}

// The compiled-in IR's tap count, source rate, normalisation gain and name; "none" without one; "fft" if the
// convolver's FFT did not initialise; otherwise its parse status. The name is last, as in logModel.
void logIr() {
    if (!kHaveIr) {
        dkusb::Log("ir none");
        return;
    }
    if (!gIrFft) {
        dkusb::Log("ir fft");
        return;
    }
    if (gIrStatus != dkir::Status::kOk) {
        dkusb::Log("ir %s", dkir::StatusName(gIrStatus));
        return;
    }
    char gain[12];
    tenthsDb(gain, 20.0f * log10f(gIrData.gain));
    dkusb::Log("ir taps %u src %u Hz gain %s dB \"%s\"", unsigned(gIrData.tap_count), unsigned(gIrData.source_rate),
               gain, gIrData.name);
}

void logBoot() {
    dkusb::Log("ducky %s seed %s sysclk %u MHz audio %u Hz %u bit block %u %s", dkboard::kName,
               SeedName(hw.seed.CheckBoardVersion()), unsigned(System::GetSysClkFreq() / 1000000),
               unsigned(hw.seed.AudioSampleRate()), bitDepth(hw.seed.AudioSaiHandle().GetConfig().bit_depth),
               unsigned(hw.seed.AudioBlockSize()), dkfw::kBuildId);
    logModel();
    logIr();
}

DK_ITCM void AudioCallback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    gLoad.OnBlockStart();
    const uint32_t start = System::GetTick();
    hw.Process();
    if (hw.FootswitchRising(0)) {
        gMuted = !gMuted;
        gMute.SetMuted(gMuted);
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
    // out[0] is the working buffer; the engine accepts in == out.
    const float inTarget = gInCal * dkdsp::InputGain(gInKnob.Process(hw.Knob(0)));
    gInGain.Process(in[0], out[0], size, inTarget);
    gEngine.Process(out[0], out[0], size);
    if (gIrOn) gIr.Process(out[0], out[0]);  // size is kBlockSize: dkfw::Start sets the block size
    // Knob 5 low-cut, Knob 6 its resonance, Knob 4 high-cut, Knob 3 its resonance.
    gEq.Process(out[0], size, {hw.Knob(4), hw.Knob(5), hw.Knob(3), hw.Knob(2), tog[0], tog[1], tog[2]});
    const float outTarget = gLevel * dkdsp::OutputGain(gOutKnob.Process(hw.Knob(1)));
    gOutGain.Process(out[0], out[0], size, outTarget);
    gMute.Process(out[0], out[0], size);
    std::copy(out[0], out[0] + size, out[1]);
    float outPeak = 0.0f;
    for (size_t i = 0; i < size; i++) outPeak = std::fmax(outPeak, std::fabs(out[0][i]));

    gLed1.Set(gMuted ? 0.0f : 1.0f);
    gLed1.Update();
    gLed2.Set(0.0f);  // unused until presets (M6)
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

    if (System::GetTick() - start > gBlockTicks) gUnderruns.fetch_add(1, std::memory_order_relaxed);
    // A Reset just before OnBlockEnd takes its first-cycle path: the published loads are never NaN.
    if (gLoadReset.exchange(false, std::memory_order_relaxed)) gLoad.Reset();
    gLoad.OnBlockEnd();
    gLoadAvg.store(gLoad.GetAvgCpuLoad(), std::memory_order_relaxed);
    gLoadMax.store(gLoad.GetMaxCpuLoad(), std::memory_order_relaxed);
}

int pct(float v) { return int(lroundf(v * 100.0f)); }

int dbfs(float peak) { return peak > 0.0f ? std::max(int(lroundf(20.0f * log10f(peak))), -120) : -120; }

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
    dkusb::Log("load avg %d max %d permille underruns %lu mute %s in %d dBFS out %d dBFS", int(avg * 1000),
               int(max * 1000), (unsigned long)gUnderruns.load(std::memory_order_relaxed),
               gMutedPub.load(std::memory_order_relaxed) ? "on" : "off", dbfs(peak), dbfs(outPeak));
}

}  // namespace

int main() {
    dkfw::Start(hw, dkdsp::kBlockSize, SaiHandle::Config::SampleRate::SAI_48KHZ);
    gLed1.Init(dkboard::Board::kLed1, false, dkdsp::kSampleRate / dkdsp::kBlockSize);
    gLed2.Init(dkboard::Board::kLed2, false, dkdsp::kSampleRate / dkdsp::kBlockSize);

    dkusb::Init(hw.seed);
    gModelStatus = dkmodel::Parse(dk_model, size_t(dk_model_end - dk_model), &gModel);
    if (gModelStatus == dkmodel::Status::kOk) {
        gLevel = dkdsp::LevelMatchGain((gModel.flags & dkmodel::kFlagHasLoudness) != 0, gModel.loudness);
        gInCal = dkdsp::InputCalibrationGain((gModel.flags & dkmodel::kFlagHasInputLevel) != 0,
                                             gModel.input_level_dbu);
    }
#ifdef DK_IR_PATH
    gIrStatus = dkir::Parse(dk_ir, size_t(dk_ir_end - dk_ir), &gIrData);
    gIrFft = gIr.Init();
#endif
    if (gModelStatus != dkmodel::Status::kOk || !gIrFft || gIrStatus != dkir::Status::kOk) {
        // No audio, LEDs off; USB (DFU included) keeps serving so a good model and IR can be flashed.
        logBoot();
        uint32_t lastLog = System::GetNow();
        for (;;) {
            dkusb::Serve(logBoot);
            const uint32_t now = System::GetNow();
            if (now - lastLog >= 1000) {
                lastLog = now;
                logModel();
                logIr();
            }
        }
    }
    gEngine.Load(gModel.weights, gModel.head_scale);
    if (kHaveIr) {
        gIr.Load(gIrData.taps);
        gIrOn = true;
    }
    logBoot();

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
        dkusb::Serve(logBoot);
        const uint32_t now = System::GetNow();
        if (now - lastLog >= 1000) {
            lastLog = now;
            logControls();
            logStatus();
        }
    }
}
