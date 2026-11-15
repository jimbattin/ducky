// Ducky pedal firmware (Cleveland Music Co. HotHouse, board.h). M0: the left input, through FS1's mute ramp, copied
// to both outputs (dual mono).
//
// Two threads of control:
// - AudioCallback (the audio DMA interrupt, 48 kHz, 48-sample blocks): reads the controls (30 ms toggle hysteresis),
//   toggles the mute on FS1, runs the mute ramp, drives LED1 and measures the input peak, the load and the underruns.
// - The main loop: USB (usb_link.h: Identity, HELLO, DFU) and the once-a-second log lines.
// They share only relaxed atomics, each with one writer:
// - callback -> main loop: gKnob, gToggle, gFsHeld, gMutedPub, gUnderruns, gPeak, gLoadAvg, gLoadMax;
// - main loop -> callback: gPeakReset, gLoadReset.
// Memory (memory.h): AudioCallback runs from ITCM; the callback's own state lives in DTCM. FPSCR.FZ is on (denormals
// flush to zero).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "board.h"
#include "controls.h"
#include "dsp/blocks.h"
#include "dsp/mute.h"
#include "firmware.h"
#include "memory.h"
#include "usb_link.h"

using namespace daisy;

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
DK_DTCM float gPeakAcc;        // input peak since the main loop last read gPeak
DK_DTCM bool gMuted = false;   // FS1's state; not persisted

// Shared with the main loop: in SRAM (AXI), relaxed.
// Callback -> main loop.
std::atomic<float> gKnob[6];               // knobs 1-6, 0..1
std::atomic<uint8_t> gToggle[3]{1, 1, 1};  // SW1-SW3 filtered: 0 down, 1 middle, 2 up
std::atomic<uint8_t> gFsHeld{0};           // bit 0 FS1, bit 1 FS2
std::atomic<bool> gMutedPub{false};        // gMuted
std::atomic<uint32_t> gUnderruns{0};       // blocks since boot whose callback ran longer than one block period
std::atomic<float> gPeak{0.0f};            // gPeakAcc, published every block
std::atomic<float> gLoadAvg{0.0f}, gLoadMax{0.0f};  // gLoad's, published every block
// Main loop -> callback.
std::atomic<bool> gPeakReset{false};  // restart gPeakAcc
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

void logBoot() {
    dkusb::Log("ducky %s seed %s sysclk %u MHz audio %u Hz %u bit block %u %s", dkboard::kName,
               SeedName(hw.seed.CheckBoardVersion()), unsigned(System::GetSysClkFreq() / 1000000),
               unsigned(hw.seed.AudioSampleRate()), bitDepth(hw.seed.AudioSaiHandle().GetConfig().bit_depth),
               unsigned(hw.seed.AudioBlockSize()), dkfw::kBuildId);
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
    for (int t = 0; t < 3; ++t)
        gToggle[t].store(gToggleFilter[t].Process(hw.Toggle(t)), std::memory_order_relaxed);
    gFsHeld.store(uint8_t((hw.FootswitchPressed(0) ? 1 : 0) | (hw.FootswitchPressed(1) ? 2 : 0)),
                  std::memory_order_relaxed);

    float peak = 0.0f;
    for (size_t i = 0; i < size; i++) peak = std::fmax(peak, std::fabs(in[0][i]));
    gMute.Process(in[0], out[0], size);
    std::copy(out[0], out[0] + size, out[1]);

    gLed1.Set(gMuted ? 0.0f : 1.0f);
    gLed1.Update();
    gLed2.Set(0.0f);  // unused until presets (M6)
    gLed2.Update();

    gMutedPub.store(gMuted, std::memory_order_relaxed);
    if (gPeakReset.exchange(false, std::memory_order_relaxed)) gPeakAcc = 0.0f;
    gPeakAcc = std::fmax(gPeakAcc, peak);
    gPeak.store(gPeakAcc, std::memory_order_relaxed);

    if (System::GetTick() - start > gBlockTicks) gUnderruns.fetch_add(1, std::memory_order_relaxed);
    // A Reset just before OnBlockEnd takes its first-cycle path: the published loads are never NaN.
    if (gLoadReset.exchange(false, std::memory_order_relaxed)) gLoad.Reset();
    gLoad.OnBlockEnd();
    gLoadAvg.store(gLoad.GetAvgCpuLoad(), std::memory_order_relaxed);
    gLoadMax.store(gLoad.GetMaxCpuLoad(), std::memory_order_relaxed);
}

int pct(float v) { return int(lroundf(v * 100.0f)); }

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
    const float peak = gPeak.load(std::memory_order_relaxed);
    gPeakReset.store(true, std::memory_order_relaxed);
    const float avg = gLoadAvg.load(std::memory_order_relaxed), max = gLoadMax.load(std::memory_order_relaxed);
    gLoadReset.store(true, std::memory_order_relaxed);
    const int db = peak > 0.0f ? std::max(int(lroundf(20.0f * log10f(peak))), -120) : -120;
    dkusb::Log("load avg %d max %d permille underruns %lu mute %s in %d dBFS", int(avg * 1000), int(max * 1000),
               (unsigned long)gUnderruns.load(std::memory_order_relaxed),
               gMutedPub.load(std::memory_order_relaxed) ? "on" : "off", db);
}

}  // namespace

int main() {
    dkfw::Start(hw, dkdsp::kBlockSize, SaiHandle::Config::SampleRate::SAI_48KHZ);
    gLed1.Init(dkboard::Board::kLed1, false, dkdsp::kSampleRate / dkdsp::kBlockSize);
    gLed2.Init(dkboard::Board::kLed2, false, dkdsp::kSampleRate / dkdsp::kBlockSize);

    dkusb::Init(hw.seed);
    logBoot();

    gMute.Init(false);
    gLoad.Init(hw.seed.AudioSampleRate(), hw.seed.AudioBlockSize());
    gBlockTicks = uint32_t(uint64_t(System::GetTickFreq()) * dkdsp::kBlockSize / uint32_t(dkdsp::kSampleRate));
    for (dkctl::ToggleFilter& f : gToggleFilter) f.Init(1);
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
