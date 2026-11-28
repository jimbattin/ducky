// midi_in.h.
#include "midi_in.h"

#include "hid/midi.h"
#include "spsc_ring.h"

namespace dkmidiin {
namespace {

daisy::MidiUartTransport gUart;
SpscRing<uint8_t, 256> gRx;  // UART IRQ -> main loop: incoming MIDI bytes (dropped when full)
dkmidi::CcParser gCc;        // Serve's Control Changes

// UART IRQ: every incoming MIDI byte goes to the main loop.
void onRx(uint8_t* data, size_t size, void*) {
    for (size_t i = 0; i < size; ++i) gRx.Push(data[i]);
}

}  // namespace

void Init() {
    daisy::MidiUartTransport::Config cfg;
    gUart.Init(cfg);
    gUart.StartRx(onRx, nullptr);
}

void Serve(void (*onControlChange)(const dkmidi::Cc& cc)) {
    // libDaisy's HAL_UART_ErrorCallback (a framing or overrun error, e.g. a cable plugged in) stops listening.
    if (!gUart.RxActive()) gUart.StartRx(onRx, nullptr);
    uint8_t b;
    while (gRx.Pop(b)) {
        dkmidi::Cc cc;
        if (gCc.Feed(b, cc)) onControlChange(cc);
    }
}

}  // namespace dkmidiin
