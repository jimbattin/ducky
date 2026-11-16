// The USB side of the host link (link.h): the Seed's own USB port as a USB MIDI device, incoming bytes through an IRQ
// ring, outgoing messages double-buffered and sent without ever blocking, and the DFU restart. Main loop only, except
// the receive IRQ. libDaisy-dependent.
#pragma once

#include <cstdint>

#include "daisy_seed.h"
#include "link.h"

namespace dkusb {

// MidiUsbTransport INTERNAL (the Seed's own port); reception started; keeps &seed.usb_handle for sending.
void Init(daisy::DaisySeed& seed);
// Restarts reception if libDaisy stopped it (its ring overflowed).
void Poll();
// The next byte from the USB IRQ ring; false when it is empty.
bool ReadByte(uint8_t& b);
// The send buffer being filled.
dklink::TxBuffer& Tx();
// A LOG message appended to Tx(); dropped if it does not fit. Never blocks.
__attribute__((format(printf, 1, 2))) void Log(const char* fmt, ...);
// Starts the filled buffer's transfer if the USB endpoint is free; never blocks.
void Flush();
// Logs "dfu reset", Flush()es for ~100 ms, then restarts into the Daisy bootloader's DFU mode (no timeout).
[[noreturn]] void EnterDfu();
// The whole host session, once per main-loop pass: Poll, then every complete message: Identity Request queues the
// Identity Reply, HELLO calls onHello (it logs the boot lines), DFU EnterDfu()s, any other command of this pedal goes
// to onCommand (its payload, between the command byte and F7), which returns false for a command it does not know;
// without onCommand, or on false, it logs `link: unknown command CC`. Then the Identity Reply if it fits (else on a
// later pass), then Flush.
void Serve(void (*onHello)(), bool (*onCommand)(uint8_t cmd, const uint8_t* payload, size_t n) = nullptr);

}  // namespace dkusb
