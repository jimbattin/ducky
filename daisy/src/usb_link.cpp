// usb_link.h. gTx is CPU-copied to the OTG FIFO (libDaisy's usbd_conf.c: dma_enable = DISABLE), so cacheable SRAM
// needs no maintenance.
#include "usb_link.h"

#include <cstdarg>
#include <cstdio>

#include "board.h"
#include "firmware.h"
#include "hid/usb_midi.h"
#include "spsc_ring.h"
#include "usbd_core.h"

// A SETUP aborts the control transfer before it (USB 2.0 8.5.3), but ST's HAL leaves that transfer's unsent IN packet
// in EP0's TxFIFO. When the host re-sends a SETUP (its first ACK lost), the device loads the reply twice and every
// later control read, until the next USB reset, returns the reply before its own: string 2 (the product) reads as
// string 0's langid table `04 03 09 04`, U+0409 "Љ", so the MIDI port is named "Љ MIDI 1" and the editor's /daisy/i
// port match misses it. Seen on about 1 boot in 6 after a DFU flash; a device-side EP0 trace showed the langid SETUP
// arriving twice, 260 us apart. This wrapper drops the stale packet first (a SETUP with DIEPTSIZ0.PKTCNT non-zero
// flushes TxFIFO 0); daisy/Makefile links every USBD_LL_SetupStage call (libDaisy's usbd_conf.c) through it with
// -Wl,--wrap, so libDaisy stays unpatched.
extern "C" USBD_StatusTypeDef __real_USBD_LL_SetupStage(USBD_HandleTypeDef* pdev, uint8_t* psetup);
extern "C" USBD_StatusTypeDef __wrap_USBD_LL_SetupStage(USBD_HandleTypeDef* pdev, uint8_t* psetup) {
    USB_OTG_GlobalTypeDef* const usb = static_cast<PCD_HandleTypeDef*>(pdev->pData)->Instance;
    const auto* in0 = reinterpret_cast<const USB_OTG_INEndpointTypeDef*>(uintptr_t(usb) + USB_OTG_IN_ENDPOINT_BASE);
    if ((in0->DIEPTSIZ & USB_OTG_DIEPTSIZ_PKTCNT) != 0) USB_FlushTxFifo(usb, 0);
    return __real_USBD_LL_SetupStage(pdev, psetup);
}

using namespace daisy;

namespace dkusb {
namespace {

MidiUsbTransport gMidi;
UsbHandle* gUsb = nullptr;
dklink::TxBuffer gTx[2];  // gTx[gFill] fills while the other may be in flight
int gFill = 0;
SpscRing<uint8_t, 512> gRx;  // USB IRQ -> main loop: incoming MIDI bytes (dropped when full)
// One host frame is in flight at a time (the host waits for each RESULT), so the ring holds the largest.
static_assert(512 >= dklink::kMaxHostFrame, "gRx holds an UPLOAD CHUNK frame");
dklink::SysExAssembler<dklink::kMaxHostFrame> gHostIn;  // Serve's host messages
dkmidi::CcParser gCc;                                   // Serve's Control Changes
bool gSendIdentity = false;                             // an Identity Request is waiting for room in Tx()
uint8_t gIdentityFlags = 0;                             // Init's

// USB IRQ: every incoming MIDI byte goes to the main loop.
void onMidiRx(uint8_t* data, size_t size, void*) {
    for (size_t i = 0; i < size; ++i) gRx.Push(data[i]);
}

// Restarts reception if libDaisy stopped it (its ring overflowed).
void Poll() {
    if (!gMidi.RxActive()) {
        gMidi.FlushRx();
        gMidi.StartRx(onMidiRx, nullptr);
    }
}

// The next byte from the USB IRQ ring; false when it is empty.
bool ReadByte(uint8_t& b) { return gRx.Pop(b); }

// Starts the filled buffer's transfer if the USB endpoint is free; never blocks. A successful start means the previous
// transfer finished, so the other buffer can be refilled; a busy endpoint keeps the batch for the next pass.
// (libDaisy's MidiUsbHandler::SendMessage retries a busy endpoint with a delay, so the link bypasses it.)
void Flush() {
    dklink::TxBuffer& t = gTx[gFill];
    if (t.size() == 0) return;
    // The transfer only reads the buffer; libDaisy's API is not const-correct.
    if (gUsb->TransmitInternal(const_cast<uint8_t*>(t.data()), t.size()) != UsbHandle::Result::OK) return;
    gFill ^= 1;
    gTx[gFill].Clear();
}

// Logs line, Flush()es for ~100 ms, then restarts into the Daisy bootloader in mode.
[[noreturn]] void ResetInto(const char* line, System::BootloaderMode mode) {
    Log("%s", line);
    for (int i = 0; i < 100; ++i) {
        Flush();
        System::Delay(1);
    }
    System::ResetToBootloader(mode);
    for (;;) {
    }
}

// The DFU command: the bootloader's DFU mode, with no timeout.
[[noreturn]] void EnterDfu() { ResetInto("dfu reset", System::BootloaderMode::DAISY_INFINITE_TIMEOUT); }

}  // namespace

void Init(DaisySeed& seed, uint8_t identityFlags) {
    gUsb = &seed.usb_handle;
    gIdentityFlags = identityFlags;
    MidiUsbTransport::Config cfg;
    cfg.periph = MidiUsbTransport::Config::INTERNAL;
    gMidi.Init(cfg);
    gMidi.StartRx(onMidiRx, nullptr);
}

dklink::TxBuffer& Tx() { return gTx[gFill]; }

void Log(const char* fmt, ...) {
    static char buf[128];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    gTx[gFill].AppendLog(buf);
}

void Restart() { ResetInto("firmware restart", System::BootloaderMode::DAISY_SKIP_TIMEOUT); }

// noipa: ServeFor calls it instead of holding a second, specialised copy.
[[gnu::noipa]] void Serve(void (*onHello)(), bool (*onCommand)(uint8_t cmd, const uint8_t* payload, size_t n),
           void (*onControlChange)(const dkmidi::Cc& cc)) {
    Poll();
    uint8_t b;
    while (ReadByte(b)) {
        dkmidi::Cc cc;
        if (gCc.Feed(b, cc) && onControlChange != nullptr) onControlChange(cc);
        if (!gHostIn.Feed(b)) continue;
        const uint8_t* m = gHostIn.data();
        const size_t n = gHostIn.size();
        if (dklink::IsIdentityRequest(m, n)) {
            gSendIdentity = true;
            continue;
        }
        const int cmd = dklink::PedalCommand(m, n);
        if (cmd < 0) continue;
        if (cmd == dklink::kHello && n == 6)
            onHello();
        else if (cmd == dklink::kDfu && n == 6)
            EnterDfu();
        else if (onCommand == nullptr || !onCommand(uint8_t(cmd), m + 5, n - 6))
            Log("link: unknown command %02X", cmd);
    }
    if (gSendIdentity && Tx().AppendIdentity(dkboard::kBoardId, gIdentityFlags, dkfw::kBuildId))
        gSendIdentity = false;
    Flush();
}

void ServeFor(uint32_t ms, void (*onHello)()) {
    const uint32_t t0 = System::GetNow();
    while (System::GetNow() - t0 < ms) Serve(onHello);
}

}  // namespace dkusb
