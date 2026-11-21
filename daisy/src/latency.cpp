// The FX loop's round trip and the crosstalk between the channels (daisy/Makefile APP=latency,
// build/<BOARD>-latency/ducky_latency.bin; docs/LOOP.md "Measuring the loop"). Measured through a cable from the
// output's ring (R Out, the send) to the input's ring (R In, the return). The pedal adds no latency of its own (no
// lookahead, no buffering beyond libDaisy's), so the round trip is what the loop adds: the click leaves through the
// output DMA and the DAC and comes back through the ADC and the input DMA, the same chain a send sample takes.
// 48 kHz, 24-sample blocks (the pedal's). Audio starts once, then 500 ms to settle. Each pass clicks R Out kTrials
// times (phase R), then L Out kTrials times (phase L). A trial captures kCapture samples of both inputs, kPre of them
// before a one-sample click of kClick on the one output, and reports, per input, the delay to its largest excursion
// (its peak: the codec filters' group delay, refined by a parabola) and to the first sample above a tenth of it (onset;
// the filters' pre-ringing stays below that). With the cable in, R->R is the round trip; R->L, L->R and L->L are
// crosstalk (and L->L whatever the left jacks have plugged in).
// Log lines (usb_log.py; docs/LOOP.md):
//   at power-on and on HELLO "ducky_latency <board> seed <version> <build id>";
//   per trial and input "lat O->I t T peak P.PP smp U us onset O smp amp A noise N" (O the clicked output, I the
//   input, L or R; A, N: 1/10000 full scale, signed A);
//   per trial and input with nothing above 4 x the noise "lat O->I t T no click (peak P noise N)";
//   per phase and input with clicks "lat O->I peak min P.PP max Q.QQ smp U..V us";
//   then "lat pass N done seed <version>".
// Passes repeat forever, 2 s apart. The link's DFU message restarts into the bootloader (checked between trials).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "board.h"
#include "dsp/blocks.h"
#include "firmware.h"
#include "memory.h"
#include "usb_link.h"

using namespace daisy;

namespace {

constexpr size_t kCapture = 8192;  // samples per trial (171 ms at 48 kHz)
constexpr size_t kPre = 256;       // of them before the click
constexpr float kClick = 0.25f;
constexpr int kTrials = 5;
constexpr uint32_t kSettleMs = 500, kTrialGapMs = 200, kPassGapMs = 2000;
constexpr uint32_t kHz = 48000;
constexpr char kChan[2] = {'L', 'R'};

dkboard::Board hw;

// Callback -> main loop: the capture. Main arms a trial (gCapStart and gClickOut, then gArmed with release); the
// callback fills gCap and sets gDone (release) after its last sample.
DK_SDRAM float gCap[2][kCapture];  // per input: 0 L, 1 R
std::atomic<uint32_t> gN{0};       // samples since StartAudio (callback writes)
uint32_t gCapStart = 0;            // absolute sample of gCap[*][0]
std::atomic<int> gClickOut{1};     // the output clicked: 0 L, 1 R
std::atomic<bool> gArmed{false}, gDone{false};

void Callback(AudioHandle::InputBuffer in, AudioHandle::OutputBuffer out, size_t size) {
    const uint32_t n0 = gN.load(std::memory_order_relaxed);
    const bool armed = gArmed.load(std::memory_order_acquire);
    bool done = false;
    const int o = gClickOut.load(std::memory_order_relaxed);
    for (size_t i = 0; i < size; ++i) {
        float y = 0.0f;
        if (armed) {
            const uint32_t k = n0 + uint32_t(i) - gCapStart;  // wraps past kCapture before the start
            if (k < kCapture) {
                gCap[0][k] = in[0][i];
                gCap[1][k] = in[1][i];
                if (k == kPre) y = kClick;
                if (k == kCapture - 1) done = true;
            }
        }
        out[o][i] = y;
        out[1 - o][i] = 0.0f;
    }
    gN.store(n0 + uint32_t(size), std::memory_order_relaxed);
    if (done) {
        gArmed.store(false, std::memory_order_relaxed);
        gDone.store(true, std::memory_order_release);
    }
}

void logBoot() {
    dkusb::Log("ducky_latency %s seed %s %s", dkboard::kName, dkfw::SeedName(hw.seed.CheckBoardVersion()),
               dkfw::kBuildId);
}

// Input in's capture of the last trial; returns the peak delay in hundredths of a sample, or -1 when nothing came
// back above the noise.
long analyse(int t, int in) {
    const float* cap = gCap[in];
    const char o = kChan[gClickOut.load(std::memory_order_relaxed)], i = kChan[in];
    double dc = 0.0;
    for (size_t k = 0; k < kPre; ++k) dc += cap[k];
    dc /= double(kPre);
    float noise = 0.0f;
    for (size_t k = 0; k < kPre; ++k) noise = std::fmax(noise, std::fabs(float(cap[k] - dc)));
    size_t p = kPre;
    for (size_t k = kPre; k < kCapture - 1; ++k)  // the parabola reads cap[p + 1]
        if (std::fabs(cap[k] - dc) > std::fabs(cap[p] - dc)) p = k;
    const float peak = float(cap[p] - dc);
    if (std::fabs(peak) <= 4.0f * noise) {
        dkusb::Log("lat %c->%c t %d no click (peak %d noise %d)", o, i, t, int(std::lround(peak * 1e4f)),
                   int(std::lround(noise * 1e4f)));
        return -1;
    }
    // Parabola through the peak and its neighbours, in the peak's polarity.
    const float s = peak > 0.0f ? 1.0f : -1.0f;
    const float a = s * float(cap[p - 1] - dc), b = s * peak, c = s * float(cap[p + 1] - dc);
    const float den = a - 2.0f * b + c;
    const float frac = den != 0.0f ? 0.5f * (a - c) / den : 0.0f;
    const long cs = std::lround((float(p - kPre) + frac) * 100.0f);
    size_t onset = kPre;
    while (onset < p && std::fabs(cap[onset] - dc) <= 0.1f * std::fabs(peak)) ++onset;
    const long us = std::lround(double(cs) * 1e4 / double(kHz));
    dkusb::Log("lat %c->%c t %d peak %ld.%02ld smp %ld us onset %u smp amp %d noise %d", o, i, t, cs / 100, cs % 100,
               us, unsigned(onset - kPre), int(std::lround(peak * 1e4f)), int(std::lround(noise * 1e4f)));
    return cs;
}

// One click on gClickOut, captured on both inputs.
void trial() {
    gDone.store(false, std::memory_order_relaxed);
    gCapStart = gN.load(std::memory_order_relaxed) + 4 * uint32_t(dkdsp::kBlockSize);  // a few blocks ahead
    gArmed.store(true, std::memory_order_release);
    while (!gDone.load(std::memory_order_acquire)) dkusb::Serve(logBoot);
}

// kTrials clicks on output o, each input's delays logged, then each input's range.
void runPhase(int o) {
    gClickOut.store(o, std::memory_order_relaxed);  // the callback reads it only while armed; no trial is armed here
    long lo[2] = {-1, -1}, hi[2] = {-1, -1};
    for (int t = 0; t < kTrials; ++t) {
        trial();
        for (int in = 0; in < 2; ++in) {
            const long cs = analyse(t, in);
            if (cs >= 0) {
                lo[in] = lo[in] < 0 ? cs : std::min(lo[in], cs);
                hi[in] = std::max(hi[in], cs);
            }
            dkusb::Serve(logBoot);
        }
        dkusb::ServeFor(kTrialGapMs, logBoot);
    }
    for (int in = 0; in < 2; ++in) {
        if (lo[in] < 0) continue;
        dkusb::Log("lat %c->%c peak min %ld.%02ld max %ld.%02ld smp %ld..%ld us", kChan[o], kChan[in], lo[in] / 100,
                   lo[in] % 100, hi[in] / 100, hi[in] % 100, std::lround(double(lo[in]) * 1e4 / double(kHz)),
                   std::lround(double(hi[in]) * 1e4 / double(kHz)));
        dkusb::Serve(logBoot);
    }
}

}  // namespace

int main() {
    dkfw::Start(hw, dkdsp::kBlockSize, SaiHandle::Config::SampleRate::SAI_48KHZ);
    dkusb::Init(hw.seed);
    logBoot();
    hw.seed.StartAudio(Callback);
    dkusb::ServeFor(kSettleMs, logBoot);
    for (unsigned pass = 1;; ++pass) {
        runPhase(1);
        runPhase(0);
        dkusb::Log("lat pass %u done seed %s", pass, dkfw::SeedName(hw.seed.CheckBoardVersion()));
        dkusb::ServeFor(kPassGapMs, logBoot);
    }
}
