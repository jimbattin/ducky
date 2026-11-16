#!/bin/sh
# Flash the Daisy Seed over its DFU bootloader (0483:df11) with dfu-util.
#
# If no DFU device is present, the script first sends the running pedal the
# link's DFU SysEx, `F0 7D 64 01 06 F7`, over USB MIDI (`usb_log.py 3 --dfu`):
# the pedal logs `dfu reset` and restarts into the Daisy bootloader's DFU mode.
# Close any other program holding the pedal's MIDI port first. By hand (e.g.
# the Seed runs other firmware): hold BOOT, tap RESET, release BOOT.
#
# Usage: [DK_USB_PORT=BUS-PORT] tools/flash_when_ready.sh [FILE.bin [ADDR]]
#   FILE.bin defaults to the HotHouse build daisy/build/hothouse/ducky.bin
#   (the bench: daisy/build/hothouse-bench/ducky_bench.bin),
#   resolved relative to this script (so it works from any working directory).
#   ADDR is 0x90040000 (firmware, the default) or 0x90400000 (a preset bank
#   from tools/preset_bank.ts; docs/FIRMWARE.md "Preset store").
#   DK_USB_PORT is the pedal's USB path (e.g. 3-4, as in /sys/bus/usb/devices
#   and dfu-util's -p; it is the physical port, so it survives the DFU reset).
#   It is required when more than one Seed (0483:5740 or 0483:df11) is
#   attached: the DFU SysEx, the DFU device check and the flash then all
#   address that path only.
#
# Polls `dfu-util -l` every 0.2 s for up to 120 s, then runs:
#   dfu-util -a 0 -s ADDR:leave -D FILE.bin -d ,0483:df11 [-p DK_USB_PORT]
set -e
command -v dfu-util >/dev/null || { echo "dfu-util not found" >&2; exit 1; }
script_dir=$(cd "$(dirname "$0")" && pwd)
file=${1:-"$script_dir/../daisy/build/hothouse/ducky.bin"}
addr=${2:-0x90040000}
port=${DK_USB_PORT:-}

case "$addr" in
    0x90040000|0x90400000) ;;
    *) echo "ADDR must be 0x90040000 (firmware) or 0x90400000 (preset bank)" >&2; exit 1 ;;
esac

seeds=$(( $(lsusb -d 0483:5740 | wc -l) + $(lsusb -d 0483:df11 | wc -l) ))
if [ "$seeds" -gt 1 ] && [ -z "$port" ]; then
    echo "DK_USB_PORT=<bus-port> required: more than one Seed attached" >&2
    exit 1
fi

dfu_present() {
    if [ -n "$port" ]; then
        dfu-util -l 2>/dev/null | grep '0483:df11' | grep -q "path=\"$port\""
    else
        dfu-util -l 2>/dev/null | grep -q '0483:df11'
    fi
}

if ! dfu_present; then
    if [ -n "$port" ]; then
        (cd "$script_dir/.." && uv run tools/usb_log.py 3 --dfu --port "$port") || true
    else
        (cd "$script_dir/.." && uv run tools/usb_log.py 3 --dfu) || true
    fi
fi

for i in $(seq 1 600); do
    if dfu_present; then
        if [ -n "$port" ]; then
            exec dfu-util -a 0 -s "$addr":leave -D "$file" -d ,0483:df11 -p "$port"
        fi
        exec dfu-util -a 0 -s "$addr":leave -D "$file" -d ,0483:df11
    fi
    sleep 0.2
done
echo "no DFU device within 120 s" >&2
exit 1
