#!/usr/bin/env -S uv run
"""Read the Ducky pedal's USB log directly over libusb.

The Seed's USB port (VID:PID 0483:5740) is a USB MIDI device
(`daisy/src/ducky.cpp`): the log travels as LOG SysEx messages
(`F0 7D 64 01 12 <ASCII> F7`, `daisy/src/link.h`). Other SysEx is skipped.
The kernel's MIDI driver is detached while reading and reattached afterwards,
so close any other program holding the MIDI port first.

This script talks to the device through `pyusb` (the `usb` package, with a
libusb backend such as `libusb-1.0`), so no MIDI driver binding is needed. It
polls `usb.core.find()` until the device appears, and again whenever it
disappears, so it keeps reading across resets and reflashes (the port
re-enumerates each time) for as long as it is asked to run.

Usage:
    uv run tools/usb_log.py [SECONDS] [--port BUS-PORT] [--dfu] [--identity]

--port picks the Seed by its USB path (BUS-PORT, e.g. 3-4, as in
/sys/bus/usb/devices and dfu-util's -p); it is required when more than one
Seed (0483:5740) is attached, and the script exits 2 without it. The path is
the physical port, so it survives resets and reflashes.

SECONDS is how long to capture (default 30). Each output line is prefixed
with the seconds since the device was last (re)connected. --dfu sends the
link's DFU SysEx (`F0 7D 64 01 06 F7`) once the pedal is connected: it logs
`dfu reset` and restarts into the Daisy bootloader's DFU mode, ready for
`tools/flash_when_ready.sh` (which sends it itself). --identity sends the
MIDI Identity Request once the pedal is connected and prints its reply as
`-- identity device 64 board 1 morph crossfade build <build id>` (`morph weight`
for the `APP=weight` image: the flags byte's bit 0).
Requires `pyusb` (in the repo's uv environment: `uv sync` at the repo root)
and a working libusb backend.
"""
import argparse
import sys
import time
from collections import deque

import usb.core
import usb.util

# Bytes per USB-MIDI event packet by code index number (libDaisy usb_midi.cpp).
CIN_SIZE = (3, 3, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1)
PEDAL_HEADER = bytes([0xF0, 0x7D, 0x64, 0x01])
LOG = 0x12  # pedal -> host
DFU = 0x06  # host -> pedal
IDENTITY_REQUEST = bytes([0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7])
IDENTITY_REPLY_HEADER = bytes([0xF0, 0x7E, 0x7F, 0x06, 0x02, 0x7D])


def sysex_packets(msg: bytes) -> bytes:
    """A complete F0 ... F7 message as USB-MIDI packets (cable 0), as TxBuffer::AppendRaw packs them: CIN 4 per 3
    bytes, the last 1/2/3 bytes as CIN 5/6/7."""
    out = bytearray()
    i = 0
    while len(msg) - i > 3:
        out += bytes([0x04]) + msg[i:i + 3]
        i += 3
    rest = msg[i:]
    out += bytes([0x04 + len(rest)]) + rest + bytes(3 - len(rest))
    return bytes(out)


def pedal_msg(cmd: int, payload: bytes = b'') -> bytes:
    return PEDAL_HEADER + bytes([cmd]) + payload + bytes([0xF7])


DFU_PACKETS = sysex_packets(pedal_msg(DFU))
IDENTITY_PACKETS = sysex_packets(IDENTITY_REQUEST)


class Session:
    """One claimed connection: send SysEx, and receive the pedal's messages. LOG lines and identity replies are
    printed as they arrive; other pedal SysEx is handed to the caller."""

    def __init__(self, ms_if, t0: float):
        self.ep_in = in_endpoint(ms_if)
        self.ep_out = out_endpoint(ms_if)
        self.t0 = t0
        self.msg = None  # SysEx being assembled
        self.pending = deque()  # complete pedal messages not yet handed out

    def send(self, msg: bytes) -> None:
        self.ep_out.write(sysex_packets(msg), timeout=1000)

    def messages(self, deadline: float):
        """Yields the pedal's non-LOG SysEx messages until the deadline."""
        while True:
            while self.pending:
                yield self.pending.popleft()
            if time.time() >= deadline:
                return
            self._pump(min(500, max(1, int((deadline - time.time()) * 1000))))

    def _pump(self, timeout_ms: int) -> None:
        try:
            data = bytes(self.ep_in.read(512, timeout=timeout_ms))
        except usb.core.USBTimeoutError:
            return
        for i in range(0, len(data) - 3, 4):
            cin = data[i] & 0x0F
            if cin < 2:  # reserved codes, skipped like libDaisy does
                continue
            for b in data[i + 1:i + 1 + CIN_SIZE[cin]]:
                if b >= 0xF8:  # realtime
                    continue
                if b == 0xF0:
                    self.msg = bytearray([b])
                elif self.msg is not None and (b < 0x80 or b == 0xF7):
                    self.msg.append(b)
                    if b == 0xF7:
                        self._complete(bytes(self.msg))
                        self.msg = None
                else:  # other status byte: not SysEx
                    self.msg = None

    def _complete(self, msg: bytes) -> None:
        if msg.startswith(PEDAL_HEADER) and len(msg) >= 6:
            if msg[4] == LOG:
                emit(self.t0, msg[5:-1].decode('ascii', errors='replace'))
            else:
                self.pending.append(msg)
        elif msg.startswith(IDENTITY_REPLY_HEADER) and len(msg) >= 16:
            build = msg[14:-1].decode('ascii', errors='replace')
            morph = 'weight' if msg[10] & 0x01 else 'crossfade'
            emit(self.t0, f'-- identity device {msg[6]:02X} board {msg[8]} morph {morph} build {build}')


def usb_path(d) -> str:
    """The device's USB path, BUS-PORT[.PORT...]: /sys/bus/usb/devices and dfu-util -p use the same string."""
    return f"{d.bus}-{'.'.join(str(p) for p in d.port_numbers)}"


def in_endpoint(intf):
    return next(e for e in intf if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_IN)


def out_endpoint(intf):
    return next(e for e in intf if usb.util.endpoint_direction(e.bEndpointAddress) == usb.util.ENDPOINT_OUT)


def emit(t0: float, text: str) -> None:
    print(f'{time.time() - t0:7.2f}  {text}', flush=True)


def detach_drivers(d, cfg) -> list:
    """Detach the kernel driver from every interface that has one; returns their numbers."""
    detached = []
    for n in sorted({i.bInterfaceNumber for i in cfg}):
        try:
            if d.is_kernel_driver_active(n):
                d.detach_kernel_driver(n)
                detached.append(n)
        except NotImplementedError:  # backend without driver control (not Linux)
            pass
    return detached


def read_midi(d, cfg, ms_if, end: float, pending: list) -> None:
    """`pending` holds (USB-MIDI packets, description) still to send (sent once, then removed). Reads the log until
    `end`."""
    # The audio driver owns both the control and the MIDI interface; detaching
    # one shuts its card down, so both are detached and reattached together.
    detached = detach_drivers(d, cfg)
    try:
        usb.util.claim_interface(d, ms_if.bInterfaceNumber)
        s = Session(ms_if, time.time())
        while pending:
            packets, text = pending.pop(0)
            s.ep_out.write(packets, timeout=1000)
            emit(s.t0, f'-- sent {text}')
        for _ in s.messages(end):
            pass
    finally:
        try:
            usb.util.release_interface(d, ms_if.bInterfaceNumber)
        except usb.core.USBError:
            pass
        for n in detached:
            try:
                d.attach_kernel_driver(n)
            except usb.core.USBError:  # gone, or already claimed by the reattached control interface
                pass


def main() -> None:
    ap = argparse.ArgumentParser(description='Read the Ducky pedal USB log; restart it into DFU for '
                                             'tools/flash_when_ready.sh.')
    ap.add_argument('seconds', nargs='?', type=float, default=30.0, help='capture time (default 30)')
    ap.add_argument('--dfu', action='store_true', help='send the DFU SysEx: the pedal restarts into DFU')
    ap.add_argument('--port', metavar='BUS-PORT',
                    help='the USB path of the pedal (e.g. 3-4, as in /sys/bus/usb/devices); '
                         'required when more than one Seed is attached')
    ap.add_argument('--identity', action='store_true', help='send the MIDI Identity Request and print the reply')
    args = ap.parse_args()
    pending = [(IDENTITY_PACKETS, 'identity')] if args.identity else []
    if args.dfu:
        pending.append((DFU_PACKETS, 'dfu'))
    end = time.time() + args.seconds
    while time.time() < end:
        seeds = list(usb.core.find(find_all=True, idVendor=0x0483, idProduct=0x5740))
        if args.port:
            d = next((s for s in seeds if usb_path(s) == args.port), None)
        elif len(seeds) > 1:
            print('-- usb: ' + str(len(seeds)) + ' Seeds attached (' + ', '.join(usb_path(s) for s in seeds)
                  + '); pass --port', flush=True)
            sys.exit(2)
        else:
            d = seeds[0] if seeds else None
        if d is None:
            time.sleep(0.2)
            continue
        try:
            cfg = d.get_active_configuration()
            ms_if = next((i for i in cfg if i.bInterfaceClass == 0x01 and i.bInterfaceSubClass == 0x03), None)
            if ms_if is None:
                print('-- usb: no MIDI streaming interface', flush=True)
                time.sleep(0.5)
                continue
            read_midi(d, cfg, ms_if, end, pending)
        except usb.core.USBError as e:
            print(f'-- usb: {e}', flush=True)
            time.sleep(0.5)


if __name__ == '__main__':
    main()
