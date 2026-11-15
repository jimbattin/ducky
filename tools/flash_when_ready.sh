#!/bin/sh
# Flash the Daisy Seed over its DFU bootloader (0483:df11) with dfu-util.
#
# If no DFU device is present, the script first sends the running pedal the
# link's DFU SysEx, `F0 7D 64 01 06 F7`, over USB MIDI (`usb_log.py 3 --dfu`):
# the pedal logs `dfu reset` and restarts into the Daisy bootloader's DFU mode.
# Close any other program holding the pedal's MIDI port first. By hand (e.g.
# the Seed runs other firmware): hold BOOT, tap RESET, release BOOT.
#
# Usage: tools/flash_when_ready.sh [FILE.bin]
#   FILE.bin defaults to the HotHouse build daisy/build/hothouse/ducky.bin,
#   resolved relative to this script (so it works from any working directory).
#
# Polls `dfu-util -l` every 0.2 s for up to 120 s, then runs:
#   dfu-util -a 0 -s 0x90040000:leave -D FILE.bin -d ,0483:df11
set -e
command -v dfu-util >/dev/null || { echo "dfu-util not found" >&2; exit 1; }
script_dir=$(cd "$(dirname "$0")" && pwd)
file=${1:-"$script_dir/../daisy/build/hothouse/ducky.bin"}

if ! dfu-util -l 2>/dev/null | grep -q '0483:df11'; then
    (cd "$script_dir/.." && uv run tools/usb_log.py 3 --dfu) || true
fi

for i in $(seq 1 600); do
    if dfu-util -l 2>/dev/null | grep -q '0483:df11'; then
        exec dfu-util -a 0 -s 0x90040000:leave -D "$file" -d ,0483:df11
    fi
    sleep 0.2
done
echo "no DFU device within 120 s" >&2
exit 1
