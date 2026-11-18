// The A2-Lite, IR, EQ and compressor benchmark and device check (daisy/Makefile APP=bench,
// build/<BOARD>-bench/ducky_bench.bin).
// No audio: the board and USB come up with the pedal's startup (firmware.h: FZ, the MPU), the committed fixtures
// tests/fixtures/nam/A2.bin and tests/fixtures/ir/decay.bin are compiled in (.incbin, with a slice of input.wav and
// its reference render), and then:
// - the check: a reset engine runs input.wav[48000, 72000) and its output is compared with A2.input.ref.wav's;
// - the IR check: a reset convolver runs input.wav[48000, 52800) and its output is compared with a direct convolution
//   computed here in double;
// - the model: dkdsp::A2Lite::Process on kBlockSize blocks of uniform noise (+-0.5), timed with the DWT cycle counter:
//   200 ms of warm-up blocks, then 2 s of measured ones, interrupts masked inside each measured block; the M3 gate is
//   its maximum against kGatePermille of the block period;
// - the layers: the same blocks, each stage of Process (BeginChunk, RunLayer per layer, EndChunk) timed separately;
// - the IR convolver with its state in D2 SRAM and in AXI SRAM, the EQ (its slopes, bell gains and bell frequencies
//   flipping every 10 blocks, so every glide and coefficient ramp runs on every block), the noise gate (enabled, the
//   longest hold: the longest detector scan), the compressor (on, blend, its Sustain, Level and Balance flipping every
//   10 blocks, so every glide and ramp runs on every block, and the noise drives the side chain into compression and
//   the clamp), the morph (dkdsp::A2Lite::Blend of one path segment on every block and the two gains the pedal
//   blends in dB: a knob that never rests; both weight sets are the model's own, so the engine goes on computing it)
//   and the pedal's whole chain (gate, morph, compressor, trim, engine, IR, EQ, output gain with the gate's gain,
//   mute), timed the same way; the chain gate is its maximum against kChainGatePermille, the load ceiling
//   (docs/ROADMAP.md "Decisions").
//   The engine is reloaded after the chain: a blend leaves it with the head scale folded into the head.
// Placement matches the pedal's: the engine, the compressor and the EQ in DTCM, the pedal's convolver in AXI SRAM
// (gIrAxi; gIrD2 is the D2 SRAM comparison) with the morph's segment (the pedal's other segments are not read by a
// blend, and SRAM has no room for them here), the stages and the timed blocks in ITCM.
// Log lines (usb_log.py; docs/FIRMWARE.md "Bench"):
//   at power-on and on HELLO "ducky_bench <board> sysclk N MHz <build id>";
//   "bench model "<name>" ok" (or "bench model <status>", repeated, when the embedded model does not parse);
//   "bench ir "<name>" ok" (or "bench ir <status>|fft", repeated, when the IR does not parse or the FFT does not
//   initialise);
//   "bench pass N budget B cycles" (B: cycles in one block period);
//   "bench check samples 24000 max err Ee-9 limit 10000e-9 ok|FAIL";
//   "bench ir check samples 4800 max err Ee-9 limit 10000e-9 ok|FAIL";
//   "bench a2lite cycles avg A max M permille avg P.p max Q.q" (permille of B), then
//   "bench gate a2lite max Q.q permille limit 450 go|no-go";
//   per stage "bench input|layer NN kK dD|head cycles avg ... " as above;
//   "bench ir d2|ir axi|eq|noise gate|comp|morph|chain cycles avg ..." as above, then
//   "bench gate chain max Q.q permille limit 850 go|no-go"; then "bench pass N done".
// Passes repeat forever, 2 s apart. The link's DFU message restarts into the bootloader (checked between stages).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "board.h"
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
#include "morph.h"
#include "usb_link.h"

using namespace daisy;
using dkdsp::kBlockSize;
namespace a2 = dkdsp::a2;

#ifndef DK_REPO_ROOT
#error "DK_REPO_ROOT (the repo root, absolute) comes from daisy/Makefile"
#endif

// The fixtures, at their canonical 44-byte WAV headers' data offsets. input.wav is 24-bit PCM: 48000 * 3 bytes
// skipped, 24000 * 3 read; A2.input.ref.wav is float32: 48000 * 4 skipped, 24000 * 4 read. The assembler rejects a
// range past the end of a file, so the sizes need no runtime check. %progbits: '@' starts a comment on ARM.
asm(".section .rodata.dk_bench,\"a\",%progbits\n"
    ".balign 4\n"
    ".global dk_bench_model\n"
    "dk_bench_model:\n"
    ".incbin \"" DK_REPO_ROOT "/tests/fixtures/nam/A2.bin\"\n"
    ".global dk_bench_model_end\n"
    "dk_bench_model_end:\n"
    ".balign 4\n"
    ".global dk_bench_input\n"
    "dk_bench_input:\n"
    ".incbin \"" DK_REPO_ROOT "/tests/fixtures/nam/input.wav\", 144044, 72000\n"
    ".global dk_bench_input_end\n"
    "dk_bench_input_end:\n"
    ".balign 4\n"
    ".global dk_bench_ref\n"
    "dk_bench_ref:\n"
    ".incbin \"" DK_REPO_ROOT "/tests/fixtures/nam/A2.input.ref.wav\", 192044, 96000\n"
    ".global dk_bench_ref_end\n"
    "dk_bench_ref_end:\n"
    ".balign 4\n"
    ".global dk_bench_ir\n"
    "dk_bench_ir:\n"
    ".incbin \"" DK_REPO_ROOT "/tests/fixtures/ir/decay.bin\"\n"
    ".global dk_bench_ir_end\n"
    "dk_bench_ir_end:\n"
    ".previous\n");
extern "C" const uint8_t dk_bench_model[], dk_bench_model_end[], dk_bench_input[], dk_bench_ref[], dk_bench_ir[],
    dk_bench_ir_end[];

namespace {

constexpr int kWarmBlocks = 200 * int(dkdsp::kBlocksPerMs), kRunBlocks = 2000 * int(dkdsp::kBlocksPerMs),
              kNoiseBlocks = 64;  // 200 ms, 2 s
constexpr uint32_t kPassGapMs = 2000;
// input.wav's first second is digital silence, so a reset engine is already in the state of sample 48000.
constexpr size_t kCheckOffset = 48000, kCheckSamples = 24000;
constexpr float kCheckLimit = 1e-5f;  // the M2 tolerance; FMA contraction measured 2.4e-7 on the host
constexpr uint32_t kGatePermille = 450;
constexpr uint32_t kChainGatePermille = 850;  // the load ceiling; 550 until the 24-sample blocks
constexpr size_t kIrCheckSamples = 4800;  // input.wav[48000, 52800): past the IR's 2048 taps
static_assert(kCheckSamples % kBlockSize == 0, "the check runs whole blocks");
static_assert(kIrCheckSamples % kBlockSize == 0 && kIrCheckSamples <= kCheckSamples, "the IR check runs whole blocks");
static_assert(kCheckOffset == 48000 && kCheckSamples == 24000, "the .incbin offsets and counts above");

// xorshift32; s must not be 0.
struct XorShift32 {
    uint32_t s;
    // [0, 1).
    float Uniform() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return float(s >> 8) * (1.0f / 16777216.0f);
    }
};

dkboard::Board hw;
DK_DTCM_BSS dkdsp::A2Lite gEngine;
dkmodel::Model gModel;
dkir::Ir gIrData;
DK_D2 dkdsp::IrConvolver gIrD2;  // D2 SRAM, for comparison
dkdsp::IrConvolver gIrAxi;       // AXI SRAM (.bss): the pedal's placement
DK_DTCM dkdsp::Eq gEq;
DK_DTCM uint32_t gEqBlocks;                   // counts the EQ's blocks: its settings flip every 10
DK_DTCM dkdsp::Compressor gComp;
DK_DTCM uint32_t gCompBlocks;                 // counts the compressor's blocks: its settings flip every 10
DK_DTCM float gValues[dkparam::kParamCount];  // the parameter values (eqValues, compValues: each writes its own)
DK_DTCM dkdsp::GainRamp gInGain, gOutGain;
DK_DTCM dkdsp::MuteRamp gMute;
DK_DTCM dkdsp::NoiseGate gGate;
float gNoise[kBlockSize * kNoiseBlocks];  // SRAM: DTCM has no room beside the engine and the stack reserve
// SDRAM: the IR check's buffers, dkmorph::Prepare's scratch and its weights are not timed, and SRAM (which holds the
// image and the fixtures too) has no room for them. Each is written before it is read.
DK_SDRAM float gIrIn[kIrCheckSamples], gIrOut[kIrCheckSamples];
DK_SDRAM float gMorphScratch[a2::kWeightCount];
DK_SDRAM dkmorph::Weights gMorphPrep;
DK_DTCM float gOut[kBlockSize];
float gMorphA[a2::kWeightCount], gMorphD[a2::kWeightCount];  // gMorphPrep's segment 0 in AXI SRAM (.bss), as the pedal's
DK_DTCM uint32_t gMorphBlocks;              // counts the morph's blocks: the blend position flips on each
DK_DTCM float gMorphGain[2];                // the level match and the input calibration the morph step computes
uint32_t gBudget = 1;  // cycles in one block period

void logBoot() {
    dkusb::Log("ducky_bench %s sysclk %u MHz %s", dkboard::kName, unsigned(System::GetSysClkFreq() / 1000000),
               dkfw::kBuildId);
}

void serveFor(uint32_t ms) {
    const uint32_t t0 = System::GetNow();
    while (System::GetNow() - t0 < ms) dkusb::Serve(logBoot);
}

// Logs a stage's average and maximum cycles per block; returns the maximum in tenths of a permille of gBudget. Serves
// USB for 5 ms after the line, so a run of lines never overflows the send buffer.
uint32_t logCycles(const char* name, uint64_t sum, uint32_t max) {
    const uint32_t avg = uint32_t(sum / kRunBlocks);
    const uint32_t pa = uint32_t(uint64_t(avg) * 10000 / gBudget), pm = uint32_t(uint64_t(max) * 10000 / gBudget);
    dkusb::Log("bench %s cycles avg %lu max %lu permille avg %lu.%lu max %lu.%lu", name, (unsigned long)avg,
               (unsigned long)max, (unsigned long)(pa / 10), (unsigned long)(pa % 10), (unsigned long)(pm / 10),
               (unsigned long)(pm % 10));
    serveFor(5);
    return pm;
}

// input.wav's 24-bit sample i of the slice, as tools/wav.h reads 24-bit PCM.
float inputSample(size_t i) {
    const uint8_t* p = dk_bench_input + 3 * i;
    const uint32_t u = uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24;
    return float(int32_t(u) >> 8) * (1.0f / 8388608.0f);
}

// "max err Ee-9 limit 10000e-9 ok|FAIL" for a check named `name`.
void logCheck(const char* name, size_t samples, float err, bool nan) {
    const bool ok = !nan && err <= kCheckLimit;
    if (nan)
        dkusb::Log("bench %s samples %u max err nan limit 10000e-9 FAIL", name, unsigned(samples));
    else  // capped at 1 for the integer print
        dkusb::Log("bench %s samples %u max err %lue-9 limit 10000e-9 %s", name, unsigned(samples),
                   (unsigned long)lroundf(std::fmin(err, 1.0f) * 1e9f), ok ? "ok" : "FAIL");
}

// The device check: input.wav[kCheckOffset, + kCheckSamples) through a reset engine against the reference render.
void check() {
    gEngine.Reset();
    float err = 0.0f;
    bool nan = false;
    float in[kBlockSize];
    for (size_t b = 0; b < kCheckSamples; b += kBlockSize) {
        for (size_t i = 0; i < kBlockSize; ++i) in[i] = inputSample(b + i);
        gEngine.Process(in, gOut, kBlockSize);
        for (size_t i = 0; i < kBlockSize; ++i) {
            float ref;
            std::memcpy(&ref, dk_bench_ref + 4 * (b + i), 4);
            const float d = std::fabs(gOut[i] - ref);
            if (std::isnan(d)) nan = true;
            else if (d > err) err = d;
        }
    }
    logCheck("check", kCheckSamples, err, nan);
}

// The IR check: input.wav[kCheckOffset, + kIrCheckSamples) through the reset convolver against a direct
// convolution in double.
void irCheck() {
    for (size_t i = 0; i < kIrCheckSamples; ++i) gIrIn[i] = inputSample(i);
    gIrAxi.Reset();
    for (size_t b = 0; b < kIrCheckSamples; b += kBlockSize) gIrAxi.Process(gIrIn + b, gIrOut + b);
    float err = 0.0f;
    bool nan = false;
    for (size_t n = 0; n < kIrCheckSamples; ++n) {
        double want = 0.0;
        for (size_t k = 0; k <= n && k < dkdsp::kIrTaps; ++k) want += double(gIrData.taps[k]) * gIrIn[n - k];
        const float d = float(std::fabs(double(gIrOut[n]) - want));
        if (std::isnan(d)) nan = true;
        else if (d > err) err = d;
    }
    logCheck("ir check", kIrCheckSamples, err, nan);
}

// The timed blocks, in ITCM like the pedal's AudioCallback.
DK_ITCM void blockModel(const float* in) { gEngine.Process(in, gOut, kBlockSize); }
DK_ITCM void blockIrD2(const float* in) { gIrD2.Process(in, gOut); }
DK_ITCM void blockIrAxi(const float* in) { gIrAxi.Process(in, gOut); }

// The EQ's worst case: enabled, both cuts mid-travel at the top resonance, their slopes flipping between 24 and
// 12 dB every 10 blocks (slope fades), every bell flipping between -12 dB at 0.25 and +12 dB at 0.75 of its
// frequency range: with a 20 ms glide and a flip every 5 ms, every glide and ramp runs on every block.
DK_ITCM const float* eqValues() {
    using namespace dkparam;
    const bool flip = (gEqBlocks++ / 10) % 2 != 0;
    float* v = gValues;
    v[kEqEnable] = 1.0f;
    v[kLowCutFreq] = v[kHighCutFreq] = 0.5f;
    v[kLowCutRes] = v[kHighCutRes] = 1.0f;
    v[kLowCutSlope] = v[kHighCutSlope] = flip ? 1.0f : 0.0f;
    for (int b = 0; b < 3; ++b) {
        v[kBell1Gain + 3 * b] = flip ? 1.0f : 0.0f;
        v[kBell1Freq + 3 * b] = flip ? 0.75f : 0.25f;
        v[kBell1Q + 3 * b] = 0.5f;
    }
    return v;
}

DK_ITCM void blockEq(const float* in) {
    std::memcpy(gOut, in, sizeof gOut);
    gEq.Process(gOut, kBlockSize, eqValues());
}

// The compressor's worst case: on, blend (both mix terms), Sustain flipping between 0.25 and 1.0 of its range, Level
// between -3 and +3 dB and Balance between 0.25 and 0.75 every 10 blocks: every glide, ramp and pow runs on every
// block, and the +-0.5 noise (0.92 V at the jack) keeps the side chain compressing and the stage clipping.
DK_ITCM const float* compValues() {
    using namespace dkparam;
    const bool flip = (gCompBlocks++ / 10) % 2 != 0;
    float* v = gValues;
    v[kCompMode] = kCompBlend;
    v[kCompSustain] = flip ? 1.0f : 0.25f;
    v[kCompLevel] = flip ? 2.0f : 0.0f;
    v[kCompBalance] = flip ? 0.75f : 0.25f;
    return v;
}

DK_ITCM void blockComp(const float* in) { gComp.Process(in, gOut, kBlockSize, compValues()); }

// The gate's detector input is the block's input peak, as in the pedal's AudioCallback.
DK_ITCM void blockGate(const float* in) { gOut[0] = gGate.Process(dkdsp::Peak(in, kBlockSize)); }

// The pedal's morph step with a knob that never rests: the weights blended on every block, at positions 0 and 1 in
// turn (the two ends of a straight blend's one segment), and the level match and the input calibration from dB.
DK_ITCM void stepMorph() {
    const float t = float(gMorphBlocks++ & 1u);
    const dkmorph::Segment g = dkmorph::SegmentAt(t, 1);
    gEngine.Blend(gMorphA, gMorphD, g.f);
    gMorphGain[0] = dkdsp::DbToGain(-3.0f + 6.0f * t);
    gMorphGain[1] = dkdsp::DbToGain(-7.5f + 3.0f * t);
}

DK_ITCM void blockMorph(const float*) { stepMorph(); }

// The pedal's AudioCallback chain, without the control reads and meters.
DK_ITCM void blockChain(const float* in) {
    const float gate = gGate.Process(dkdsp::Peak(in, kBlockSize));
    stepMorph();
    gComp.Process(in, gOut, kBlockSize, compValues());
    gInGain.Process(gOut, gOut, kBlockSize, 1.0f);
    gEngine.Process(gOut, gOut, kBlockSize);
    gIrAxi.Process(gOut, gOut);
    gEq.Process(gOut, kBlockSize, eqValues());
    gOutGain.Process(gOut, gOut, kBlockSize, gate);
    gMute.Process(gOut, gOut, kBlockSize);
}

constexpr int kStages = a2::kLayers + 2;  // input, the layers, head
// c[0]: BeginChunk; c[1 + l]: RunLayer(l); c[kStages - 1]: EndChunk.
DK_ITCM void blockLayers(const float* in, uint32_t* c) {
    __asm__ volatile("" ::: "memory");
    uint32_t t = DWT->CYCCNT;
    gEngine.BeginChunk(in, kBlockSize);
    __asm__ volatile("" ::: "memory");
    uint32_t t1 = DWT->CYCCNT;
    c[0] = t1 - t;
    t = t1;
    for (int l = 0; l < a2::kLayers; ++l) {
        gEngine.RunLayer(l, kBlockSize);
        __asm__ volatile("" ::: "memory");
        t1 = DWT->CYCCNT;
        c[1 + l] = t1 - t;
        t = t1;
    }
    gEngine.EndChunk(gOut, kBlockSize);
    __asm__ volatile("" ::: "memory");
    c[kStages - 1] = DWT->CYCCNT - t;
}

const float* noiseBlock(int k) { return gNoise + size_t((k + kWarmBlocks) % kNoiseBlocks) * kBlockSize; }

// block(in) once per block, k from -kWarmBlocks; logs the measured blocks as stage `name` and returns their maximum
// in tenths of a permille. noipa: block stays an indirect call, so GCC cannot clone this (SRAM) function and inline an
// ITCM block into it.
[[gnu::noipa]] uint32_t timeStage(const char* name, void (*block)(const float* in)) {
    uint64_t sum = 0;
    uint32_t max = 0;
    for (int k = -kWarmBlocks; k < kRunBlocks; ++k) {
        const float* in = noiseBlock(k);
        __disable_irq();
        __asm__ volatile("" ::: "memory");
        const uint32_t t0 = DWT->CYCCNT;
        block(in);
        __asm__ volatile("" ::: "memory");
        const uint32_t c = DWT->CYCCNT - t0;
        __enable_irq();
        if (k < 0) continue;
        sum += c;
        if (c > max) max = c;
    }
    return logCycles(name, sum, max);
}

void logGate(const char* name, uint32_t pm, uint32_t limit) {
    dkusb::Log("bench gate %s max %lu.%lu permille limit %lu %s", name, (unsigned long)(pm / 10),
               (unsigned long)(pm % 10), (unsigned long)limit, pm <= limit * 10 ? "go" : "no-go");
    serveFor(5);
}

[[gnu::noipa]] void timeLayers(void (*block)(const float* in, uint32_t* c)) {
    uint64_t sum[kStages] = {};
    uint32_t max[kStages] = {}, c[kStages];
    for (int k = -kWarmBlocks; k < kRunBlocks; ++k) {
        const float* in = noiseBlock(k);
        __disable_irq();
        block(in, c);
        __enable_irq();
        if (k < 0) continue;
        for (int s = 0; s < kStages; ++s) {
            sum[s] += c[s];
            if (c[s] > max[s]) max[s] = c[s];
        }
    }
    logCycles("input", sum[0], max[0]);
    for (int l = 0; l < a2::kLayers; ++l) {
        char name[24];
        snprintf(name, sizeof name, "layer %02d k%d d%d", l, a2::kLayout[l].kernel, a2::kLayout[l].dilation);
        logCycles(name, sum[1 + l], max[1 + l]);
    }
    logCycles("head", sum[kStages - 1], max[kStages - 1]);
}

void runPass(unsigned pass) {
    dkusb::Log("bench pass %u budget %lu cycles", pass, (unsigned long)gBudget);
    dkusb::Serve(logBoot);
    check();
    dkusb::Serve(logBoot);
    irCheck();
    dkusb::Serve(logBoot);
    gEngine.Reset();
    logGate("a2lite", timeStage("a2lite", blockModel), kGatePermille);
    dkusb::Serve(logBoot);
    timeLayers(blockLayers);
    dkusb::Serve(logBoot);
    timeStage("ir d2", blockIrD2);
    dkusb::Serve(logBoot);
    timeStage("ir axi", blockIrAxi);
    dkusb::Serve(logBoot);
    timeStage("eq", blockEq);
    dkusb::Serve(logBoot);
    timeStage("noise gate", blockGate);
    dkusb::Serve(logBoot);
    timeStage("comp", blockComp);
    dkusb::Serve(logBoot);
    timeStage("morph", blockMorph);
    dkusb::Serve(logBoot);
    logGate("chain", timeStage("chain", blockChain), kChainGatePermille);
    gEngine.Load(gModel.weights, gModel.head_scale);  // the next pass's check runs the model as loaded
    dkusb::Log("bench pass %u done", pass);
    serveFor(kPassGapMs);
}

}  // namespace

int main() {
    dkfw::Start(hw, kBlockSize, SaiHandle::Config::SampleRate::SAI_48KHZ);
    dkusb::Init(hw.seed);

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->LAR = 0xC5ACCE55;
    DWT->CYCCNT = 0;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    gBudget = uint32_t(uint64_t(System::GetSysClkFreq()) * kBlockSize / uint32_t(dkdsp::kSampleRate));

    XorShift32 rng{0x12345678u};
    for (float& v : gNoise) v = rng.Uniform() - 0.5f;

    logBoot();
    const dkmodel::Status st = dkmodel::Parse(dk_bench_model, size_t(dk_bench_model_end - dk_bench_model), &gModel);
    if (st != dkmodel::Status::kOk) {
        for (;;) {
            dkusb::Log("bench model %s", dkmodel::StatusName(st));
            serveFor(kPassGapMs);
        }
    }
    dkusb::Log("bench model \"%s\" ok", gModel.name);
    gEngine.Load(gModel.weights, gModel.head_scale);

    const dkir::Status ist = dkir::Parse(dk_bench_ir, size_t(dk_bench_ir_end - dk_bench_ir), &gIrData);
    const bool fft = gIrD2.Init() && gIrAxi.Init();
    if (ist != dkir::Status::kOk || !fft) {
        for (;;) {
            dkusb::Log("bench ir %s", fft ? dkir::StatusName(ist) : "fft");
            serveFor(kPassGapMs);
        }
    }
    dkusb::Log("bench ir \"%s\" ok", gIrData.name);
    gIrD2.Load(gIrData.taps);
    gIrAxi.Load(gIrData.taps);
    dkmorph::Prepare(dkmorph::kIdentity, gModel, gModel, nullptr, 0, gMorphScratch, &gMorphPrep);
    std::memcpy(gMorphA, gMorphPrep.a[0], sizeof gMorphA);
    std::memcpy(gMorphD, gMorphPrep.d[0], sizeof gMorphD);
    gEq.Init(eqValues());
    gComp.Init(compValues());
    gInGain.Init(1.0f);
    gOutGain.Init(1.0f);
    gMute.Init(false);
    gGate.Set({true, 60, 5, 100, 100, 40});  // the longest hold: the longest scan

    for (unsigned pass = 1;; ++pass) runPass(pass);
}
