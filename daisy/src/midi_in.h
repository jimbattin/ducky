// A BurningBungalow's MIDI IN jack (TRS type A): libDaisy's MidiUartTransport on USART1 at 31250 baud (RX D14 / PB7;
// TX D13 / PB6 idles high, nothing is sent), bytes from the UART's DMA IRQ through a ring to the main loop, parsed
// for Control Changes. Only ducky.cpp on a board with dkboard::kHasMidiIn calls it. libDaisy-dependent; main loop
// only, except the receive IRQ.
#pragma once

#include "midi.h"

namespace dkmidiin {

// The UART on libDaisy's default MidiUartTransport::Config (USART1, PB7/PB6, its DMA_BUFFER_MEM_SECTION rx buffer:
// uncached D2 SRAM, firmware.h's MPU region 1); reception started.
void Init();
// Once per main-loop pass: restarts reception if a UART error stopped it, then every byte received since: each
// complete Control Change goes to onControlChange.
void Serve(void (*onControlChange)(const dkmidi::Cc& cc));

}  // namespace dkmidiin
