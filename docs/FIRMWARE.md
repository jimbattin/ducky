# Firmware

The pedal firmware in `daisy/`, plus the host tests and tools that go with it. Every command runs from the repo root.

## Setup (once per clone)

```sh
git submodule update --init --recursive   # libDaisy v9.0.0 and its driver submodules (HAL, CMSIS_5, CMSIS-DSP, USB device)
make -C daisy/libDaisy                    # daisy/libDaisy/build/libdaisy.a
uv sync                                   # .venv with pyusb for tools/usb_log.py
```

This needs `arm-none-eabi-gcc` (tested with 13.3.rel1), `dfu-util`, `uv`, `g++`, GoogleTest visible to `pkg-config gtest_main`, and Bun (tested with 1.4.4) for `editor/` and `tools/nam_convert.ts`. `make -C editor` installs the editor's dependencies on first use.

## Build, test, flash, log

```sh
make test                        # host GoogleTest: build/host/tests (tests/*.cpp); also builds build/host/render
make -C daisy -j                 # daisy/build/hothouse/ducky.bin; the link prints the memory-usage table
make -C daisy APP=bench -j       # daisy/build/hothouse-bench/ducky_bench.bin ("Bench" below)
tools/flash_when_ready.sh [FILE.bin]    # sends DFU over USB-MIDI, waits for 0483:df11, flashes 0x90040000
uv run tools/usb_log.py 10 --identity   # 10 s of log, preceded by the Identity Reply
make -C editor test check        # converter tests (bun:test), tsc, Biome
bun tools/nam_convert.ts models/*.nam   # .nam -> .bin next to each input (docs/NAM_FORMAT.md)
tools/nam_reference.sh           # NAM reference renders (docs/NAM_FORMAT.md "Reference renders")
```

- **Build options:** `daisy/Makefile` takes `BOARD=hothouse` (the only board) and `APP=ducky` (default) or `APP=bench`. The build directory is `daisy/build/<BOARD>`, or `daisy/build/<BOARD>-bench` for the bench.
- **Optimisation flags:** `-O3 -fno-schedule-insns -fno-schedule-insns2`. With GCC's instruction scheduling off, the order of the DSP's statements is the order of its instructions, which the engine relies on (`docs/M7_PERFORMANCE.md`).
- **Build id:** the build writes `build_id.c` with the short commit hash, plus `+dirty` when tracked files differ. Untracked files do not make it dirty.
- **Flashing:** `flash_when_ready.sh [FILE.bin]` needs no button presses while Ducky is running. If the Seed runs other firmware, either send that firmware's own DFU command or hold BOOT, tap RESET, release BOOT. Then run the script.
- **Several Seeds attached:** every Seed enumerates as `0483:5740`. Pick Ducky's by its USB path (`BUS-PORT`, as in `/sys/bus/usb/devices` and `dfu-util -p`; the physical port, so it survives the DFU reset): `uv run tools/usb_log.py 10 --port 3-4` and `DK_USB_PORT=3-4 tools/flash_when_ready.sh`. Without it, `usb_log.py` prints `-- usb: N Seeds attached (…); pass --port` and exits 2, and the flasher refuses when more than one `0483:5740`/`0483:df11` is present. `--identity` shows which pedal a path holds (`device 64` is Ducky).
- **Port conflicts:** `usb_log.py` and `flash_when_ready.sh` claim the USB-MIDI interface through libusb. They detach the kernel driver, which drops the ALSA port while they run. A program holding the ALSA port does not block them. It does block `amidi`, which then fails with "Device or resource busy"; a browser tab using Web MIDI is a common holder.
- **Build warnings:** libDaisy's startup file, newlib's `_read`/`_write`/… stubs and the linker's "RWX segment" all warn on every build. Ducky's own sources build without warnings.

## Source layout

| Path | Contents | Host-portable |
|---|---|---|
| `daisy/src/ducky.cpp` | The pedal: `AudioCallback`, the main loop, the log lines. | no |
| `daisy/src/bench.cpp` | `APP=bench`: the A2-Lite device check and cycle timing ("Bench"). | no |
| `daisy/src/board.h` | `dkboard::Board`: the HotHouse pins, knobs, toggles, footswitches. `kBoardId` 1. | no |
| `daisy/src/firmware.h` | `dkfw::Start` (flush-to-zero, board init, MPU) and `kBuildId`. | no |
| `daisy/src/usb_link.h/.cpp` | `dkusb`: USB-MIDI transport, a non-blocking double-buffered send, `Serve` (Identity, HELLO, DFU), `EnterDfu`. | no |
| `daisy/src/link.h` | `dklink`: the SysEx wire format, `TxBuffer`, `SysExAssembler`, `PedalCommand`. The protocol reference. | yes |
| `daisy/src/controls.h` | `dkctl::ToggleFilter` (30 ms toggle hysteresis). | yes |
| `daisy/src/dsp/blocks.h` | `dkdsp::kSampleRate` (48 kHz), `kBlockSize` (48). | yes |
| `daisy/src/dsp/mute.h` | `dkdsp::MuteRamp`: FS1's 5 ms linear mute. | yes |
| `daisy/src/dsp/a2lite.h` | `dkdsp::A2Lite`: the A2-Lite network (`docs/NAM_FORMAT.md` "Engine"); `dkdsp::a2` holds the shape and memory layout; `dkdsp::FpHere`. | yes |
| `daisy/src/model.h` | `dkmodel::Parse`: validates and decodes a format-1 model binary; `Crc32`. | yes |
| `daisy/src/memory.h/.cpp` | `DK_ITCM`, `DK_DTCM`, `DK_DTCM_BSS`, `DK_D2`, `DK_SDRAM` placement and the boot-time TCM copy. | yes (`.h`) |
| `daisy/src/spsc_ring.h` | The lock-free single-producer/single-consumer ring (USB IRQ → main loop). | yes |
| `daisy/STM32H750IB_sram_ducky.lds` | libDaisy's BOOT_SRAM script plus the `.dtcm_data`, `.dtcm_bss`, `.itcm_text`, `.d2_bss`, `.sdram_bss` sections and the 16 KB stack reserve. | |
| `tests/` | GoogleTest for the host-portable headers. Never include `board.h`, `firmware.h` or `usb_link.h` here. | |
| `tools/usb_log.py`, `tools/flash_when_ready.sh` | Log reader with `--port`, `--identity` and `--dfu`, and the flasher (`DK_USB_PORT`). | |
| `editor/src/nam/`, `tools/nam_convert.ts`, `tools/nam_reference.sh`, `tools/make_stimuli.py` | The `.nam` → model binary converter and its CLI, the NAM reference renders, and their stimulus (`docs/NAM_FORMAT.md`). | |
| `tools/render.cpp`, `tools/wav.h` | `build/host/render` (`make render`): the engine over a WAV file; `dkwav` WAV reading and float32 writing (host only). | |
| `tests/fixtures/nam/` | Upstream MIT `.nam` and WAV fixtures, `A2.bin` converted from `A2.nam`, and the committed `A2.nam` reference render. | |

- **Naming:** namespaces are `dkboard`, `dkfw`, `dkusb`, `dklink`, `dkctl`, `dkdsp`, `dkmodel` and (host only) `dkwav`. Macros use the `DK_` prefix, and the board define is `DUCKY_BOARD_HOTHOUSE`.
- **DSP code** goes under `daisy/src/dsp/`, host-portable and covered by `tests/`. The top-level `Makefile` rebuilds the tests and `render` whenever any `daisy/src/*.h`, `daisy/src/dsp/*.h`, `tests/*.h` or `tools/*.h` changes. Tests find fixtures through `DK_REPO_ROOT`, so `build/host/tests` runs from any directory.
- **Before writing or optimising a DSP kernel,** read `docs/M7_PERFORMANCE.md`: what float code costs on this core, measured, and the rules that follow. Host tests cannot see performance; the bench can.

## Threads

- **`AudioCallback`** is the audio DMA interrupt at 48 kHz, in 48-sample blocks (1 ms). It runs from ITCM, and its state lives in DTCM. In order, it:
  1. reads the controls;
  2. toggles the mute on FS1;
  3. measures the input peak;
  4. runs `MuteRamp` on the left input;
  5. copies the left output to the right;
  6. drives the LEDs;
  7. measures the load and the underruns (a callback longer than one block period).
- **Main loop** runs `dkusb::Serve(logBoot)` and the once-a-second log lines.
- **Sharing:** the two threads share only relaxed `std::atomic`s, each with one writer:
  - callback → main loop: `gKnob`, `gToggle`, `gFsHeld`, `gMutedPub`, `gUnderruns`, `gPeak`, `gLoadAvg`, `gLoadMax`;
  - main loop → callback: `gPeakReset`, `gLoadReset`.

## Memory

| Region | Size | Holds now | Reserved for |
|---|---|---|---|
| ITCM `0x00000000` | 64 KB | `AudioCallback` (1.2 KB); in the bench, the A2-Lite stages (4.0 KB) | |
| DTCM `0x20000000` | 128 KB | Callback state; the 16 KB stack reserve at the top; in the bench, the engine (99,124 bytes in `.dtcm_bss`: history 90,600, weights 7,480) | The engine in the pedal (M4) |
| AXI SRAM `0x24000000` | 480 KB | Code, `.data`, `.bss` (124 KB in total) | |
| D2 SRAM `0x30008000` | 256 KB | — | IR partitions (M5) |
| SDRAM `0xC0000000` | 64 MB | — | Preset buffers (M6) |
| QSPI `0x90000000` | 8 MB | Image at `0x90040000` | Presets in the upper 4 MB (M6) |

- **TCM copy:** `.itcm_text` and `.dtcm_data` are copied from their SRAM load image in `.preinit_array` (`memory.cpp`), before any constructor runs; `.dtcm_bss` is zeroed there.
- **No-load sections:** `.dtcm_bss` (`DK_DTCM_BSS`) is `NOLOAD`, so a large all-zero object such as the engine adds nothing to the image; only objects with an all-zero static initialiser belong there. `.d2_bss` and `.sdram_bss` are `NOLOAD` too, but nothing zeroes them; their owner clears them.
- **Template placement:** GCC 13 drops section attributes from template instantiations. A hot template reaches ITCM only when inlined into a `DK_ITCM` caller, so mark it `[[gnu::always_inline]]`. Check placement with `arm-none-eabi-nm -C daisy/build/hothouse/ducky.elf`.
- **MPU** (`firmware.h`):
  - Region 0 is a no-access background over `0x60000000–0xDFFFFFFF`, which covers all of QSPI. This avoids the ES0392 2.7.5 stall on speculative reads of `0x907FFFFF`.
  - Regions 1–3 are libDaisy's: D2 DMA, SDRAM and backup SRAM.
  - M6 must add a read-only cached region for the preset area. Bad Tape's region 4 is the pattern to copy.

## Link protocol

`daisy/src/link.h` is the reference. Frames are `F0 7D 64 01 <cmd> … F7` (device byte `0x64`, version `0x01`).

| Direction | Command | Meaning |
|---|---|---|
| host → pedal | `01` HELLO | The pedal answers with the boot line. |
| host → pedal | `06` DFU | The pedal logs `dfu reset` and restarts into the Daisy bootloader with no timeout. |
| host → pedal | Identity Request `F0 7E <id> 06 01 F7` | The pedal answers `F0 7E 7F 06 02 7D 64 00 01 00 00 00 00 00 <build id> F7`. |
| pedal → host | `12` LOG | One printable-ASCII line of at most 120 characters. |

Other commands with this header log `link: unknown command CC`. Other devices' frames are ignored. M7 extends the protocol and generates `docs/MIDI.md` from it.

## Log lines

```
ducky hothouse seed 1.2-pcm3060 sysclk 480 MHz audio 48000 Hz 24 bit block 48 06ea416        (boot / HELLO)
ctl k 31 22 33 58 25 34 sw 2 0 1 fs 0 0                                                       (every second)
load avg 6 max 6 permille underruns 0 mute off in -84 dBFS                                    (every second)
```

- **`ctl` line:**
  - `k`: knobs 1–6, in percent.
  - `sw`: SW1–SW3 after the 30 ms filter, as 2 up, 1 middle, 0 down.
  - `fs`: FS1 and FS2 held.
- **`load` line:**
  - `avg` and `max`: libDaisy's `CpuLoadMeter`, in permille of the block period, measured over the last second.
  - `underruns`: the count since boot.
  - `mute`: FS1's state, which is not persisted.
  - `in`: the left-input peak over the last second, floored at −120 dBFS.

## Measured (M0, Seed 1.2 / PCM3060)

| | |
|---|---|
| Passthrough load | avg 6, max 7 permille |
| Underruns | 0 |
| DTCM | 16.5 KB of 128 KB (including the 16 KB stack reserve) |
| ITCM | 1.2 KB of 64 KB |
| SRAM | 124 KB of 480 KB |
| Image | 96,796 bytes |

## Bench (M3)

`APP=bench` (`daisy/src/bench.cpp`) has no audio. It compiles in `tests/fixtures/nam/A2.bin` and a slice of `input.wav` and `A2.input.ref.wav` (`.incbin`; the Makefile rebuilds `bench.o` when they change). It places the engine like the pedal will: the object in DTCM, its stages and the timed blocks in ITCM. Each pass:

1. **Check:** a reset engine runs `input.wav[48000, 72000)` (the file's first second is silence, so a reset engine is already in that state) against the reference render; limit 1e-5.
2. **Model:** `A2Lite::Process` on 48-sample blocks of uniform noise (±0.5), timed with the DWT cycle counter: 200 warm-up blocks, then 2000 measured, interrupts masked inside each block. The gate is the maximum against 450 permille of the block period.
3. **Layers:** the same blocks, each stage (`BeginChunk`, `RunLayer` per layer, `EndChunk`) timed separately.

Passes repeat every 2 s; the DFU command works between stages, so `flash_when_ready.sh` returns to the pedal with no button presses.

```
ducky_bench hothouse sysclk 480 MHz 999fce4+dirty                                 (boot / HELLO)
bench model "A2" ok
bench pass 1 budget 480000 cycles
bench check samples 24000 max err 241e-9 limit 10000e-9 ok
bench a2lite cycles avg 168369 max 168936 permille avg 350.7 max 351.9
bench gate a2lite max 351.9 permille limit 450 go
bench input cycles avg 1368 max 1381 permille avg 2.8 max 2.8
bench layer 00 k6 d1 cycles avg 6658 max 6718 permille avg 13.8 max 13.9        (… layer 22)
bench head cycles avg 4765 max 4766 permille avg 9.9 max 9.9
bench pass 1 done
```

### Measured (M3, Seed 1.2)

| Stage | Avg cycles | Max permille |
|---|---|---|
| Whole model (`a2lite`) | 168,369 | 351.9 |
| Input (rechannel into layer 0's ring) | 1,368 | 2.8 |
| Layer, kernel 6 (layers 0–13, 16–21) | 6,536–6,658 | 13.8–13.9 |
| Layer, kernel 15 (layers 14, 15) | 12,782–12,806 | 26.8 |
| Layer 22 (kernel 6, no residual) | 5,414 | 11.3 |
| Head | 4,765 | 9.9 |

- **Gate:** go, 351.9 permille against 450. The engine has no data-dependent branch, so the cost does not depend on the signal (max is within 0.4 % of avg).
- **Check:** max error 241e-9 (FMA contraction; the host build is bit-exact). The value has been the same for every engine version: only the order of instructions changed, never the arithmetic.
- **Cost split:** from the kernel-15 minus kernel-6 difference, a tap costs ≈ 690 cycles per block (1.6 cycles per multiply-add), so the 156 taps take ≈ 108 k cycles. The rest of a layer (bias, mixin, activation, head sum, residual) ≈ 2,420 cycles, ≈ 55 k in all. All-in the model runs at 2.0 cycles per multiply-add.
- **Image:** 273,184 bytes; DTCM 115,704 bytes of 128 KB (with the 16 KB stack reserve); ITCM 4.0 KB.

How the engine got here from 570 permille, the measured cost model behind it, and how to check an image for a regression: `docs/M7_PERFORMANCE.md`. Run the bench after any change to the engine, the compiler or its flags: the gate line is the only test that sees a slowdown.
