// The A2-Lite benchmark and device check (daisy/Makefile APP=bench, build/<BOARD>-bench/ducky_bench.bin). No audio:
// the board and USB come up with the pedal's startup (firmware.h: FZ, the MPU), the committed fixture model
// tests/fixtures/nam/A2.bin is compiled in (.incbin, with a slice of input.wav and its reference render), and then:
// - the check: a reset engine runs input.wav[48000, 72000) and its output is compared with A2.input.ref.wav's;
// - the model: dkdsp::A2Lite::Process on kBlockSize blocks of uniform noise (+-0.5), timed with the DWT cycle counter:
//   200 ms of warm-up blocks, then 2 s of measured ones, interrupts masked inside each measured block; the M3 gate is
//   its maximum against kGatePermille of the block period;
// - the layers: the same blocks, each stage of Process (BeginChunk, RunLayer per layer, EndChunk) timed separately.
// Placement matches the pedal's: the engine in DTCM (DK_DTCM_BSS), its stages and the timed blocks in ITCM.
// Log lines (usb_log.py; docs/FIRMWARE.md "Bench"):
//   at power-on and on HELLO "ducky_bench <board> sysclk N MHz <build id>";
//   "bench model "<name>" ok" (or "bench model <status>", repeated, when the embedded model does not parse);
//   "bench pass N budget B cycles" (B: cycles in one block period);
//   "bench check samples 24000 max err Ee-9 limit 10000e-9 ok|FAIL";
//   "bench a2lite cycles avg A max M permille avg P.p max Q.q" (permille of B), then
//   "bench gate a2lite max Q.q permille limit 450 go|no-go";
//   per stage "bench input|layer NN kK dD|head cycles avg ... " as above; then "bench pass N done".
// Passes repeat forever, 2 s apart. The link's DFU message restarts into the bootloader (checked between stages).
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "board.h"
#include "dsp/a2lite.h"
#include "dsp/blocks.h"
#include "firmware.h"
#include "memory.h"
#include "model.h"
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
    ".previous\n");
extern "C" const uint8_t dk_bench_model[], dk_bench_model_end[], dk_bench_input[], dk_bench_ref[];

namespace {

constexpr int kWarmBlocks = 200, kRunBlocks = 2000, kNoiseBlocks = 64;  // 200 ms and 2 s of 1 ms blocks
constexpr uint32_t kPassGapMs = 2000;
// input.wav's first second is digital silence, so a reset engine is already in the state of sample 48000.
constexpr size_t kCheckOffset = 48000, kCheckSamples = 24000;
constexpr float kCheckLimit = 1e-5f;  // the M2 tolerance; FMA contraction measured 2.4e-7 on the host
constexpr uint32_t kGatePermille = 450;
static_assert(kCheckSamples % kBlockSize == 0, "the check runs whole blocks");
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
float gNoise[kBlockSize * kNoiseBlocks];  // SRAM: DTCM has no room beside the engine and the stack reserve
DK_DTCM float gOut[kBlockSize];
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

// The device check: input.wav[kCheckOffset, + kCheckSamples) through a reset engine against the reference render.
void check() {
    gEngine.Reset();
    float err = 0.0f;
    bool nan = false;
    float in[kBlockSize];
    for (size_t b = 0; b < kCheckSamples; b += kBlockSize) {
        for (size_t i = 0; i < kBlockSize; ++i) {
            const uint8_t* p = dk_bench_input + 3 * (b + i);
            const uint32_t u = uint32_t(p[0]) << 8 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 24;
            in[i] = float(int32_t(u) >> 8) * (1.0f / 8388608.0f);  // as tools/wav.h reads 24-bit PCM
        }
        gEngine.Process(in, gOut, kBlockSize);
        for (size_t i = 0; i < kBlockSize; ++i) {
            float ref;
            std::memcpy(&ref, dk_bench_ref + 4 * (b + i), 4);
            const float d = std::fabs(gOut[i] - ref);
            if (std::isnan(d)) nan = true;
            else if (d > err) err = d;
        }
    }
    const bool ok = !nan && err <= kCheckLimit;
    if (nan)
        dkusb::Log("bench check samples %u max err nan limit 10000e-9 FAIL", unsigned(kCheckSamples));
    else  // capped at 1 for the integer print
        dkusb::Log("bench check samples %u max err %lue-9 limit 10000e-9 %s", unsigned(kCheckSamples),
                   (unsigned long)lroundf(std::fmin(err, 1.0f) * 1e9f), ok ? "ok" : "FAIL");
}

// The timed blocks, in ITCM like the pedal's AudioCallback.
DK_ITCM void blockModel(const float* in) { gEngine.Process(in, gOut, kBlockSize); }

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

// block(in) once per block, k from -kWarmBlocks; logs the measured blocks, then the gate. noipa: block stays an
// indirect call, so GCC cannot clone this (SRAM) function and inline an ITCM block into it.
[[gnu::noipa]] void timeModel(void (*block)(const float* in)) {
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
    const uint32_t pm = logCycles("a2lite", sum, max);
    dkusb::Log("bench gate a2lite max %lu.%lu permille limit %lu %s", (unsigned long)(pm / 10),
               (unsigned long)(pm % 10), (unsigned long)kGatePermille, pm <= kGatePermille * 10 ? "go" : "no-go");
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
    gEngine.Reset();
    timeModel(blockModel);
    dkusb::Serve(logBoot);
    timeLayers(blockLayers);
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

    for (unsigned pass = 1;; ++pass) runPass(pass);
}
