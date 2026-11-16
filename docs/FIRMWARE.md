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
make -C daisy -j MODEL=models/fender_super_eob_with_cab.bin   # the same with another compiled-in model
make -C daisy -j IR=irs/fdmn_blnd_mix01.bin                   # with a compiled-in cab IR (docs/FILTERS.md)
make -C daisy APP=bench -j       # daisy/build/hothouse-bench/ducky_bench.bin ("Bench" below)
tools/flash_when_ready.sh [FILE.bin]    # sends DFU over USB-MIDI, waits for 0483:df11, flashes 0x90040000
uv run tools/usb_log.py 10 --identity   # 10 s of log, preceded by the Identity Reply
make -C editor test check        # converter tests (bun:test), tsc, Biome
bun tools/nam_convert.ts models/*.nam   # .nam -> .bin next to each input (docs/NAM_FORMAT.md)
bun tools/ir_convert.ts IR.wav...       # .wav -> IR .bin next to each input (docs/FILTERS.md)
tools/nam_reference.sh           # NAM reference renders (docs/NAM_FORMAT.md "Reference renders")
```

- **Build options:** `daisy/Makefile` takes `BOARD=hothouse` (the only board) and `APP=ducky` (default) or `APP=bench`. The build directory is `daisy/build/<BOARD>`, or `daisy/build/<BOARD>-bench` for the bench.
- **Compiled-in model (`APP=ducky`):** `MODEL=FILE.bin` (a format-1 binary, `docs/NAM_FORMAT.md`), relative to the repo root or absolute; default the committed `tests/fixtures/nam/A2.bin`, so a clean clone builds. `ducky.cpp` `.incbin`s it through `DK_MODEL_PATH`. `build/<BOARD>/model_path` records the path, so switching `MODEL` rebuilds `ducky.o` even when the new file is older. A missing file stops the build with `MODEL not found: <path>`. M6 replaces the compiled-in model with the preset store.
- **Compiled-in IR (`APP=ducky`):** `IR=FILE.bin` (a format-1 IR binary, `docs/FILTERS.md`), the same way through `DK_IR_PATH` and `build/<BOARD>/ir_path`; default none, which leaves the IR stage out. The path must not contain spaces (`IR path must not contain spaces`); a missing file stops the build with `IR not found: <path>`.
- **CMSIS-DSP:** `libdaisy.a` has no CMSIS-DSP, so both builds compile the FFT sources `daisy/cmsis_fft.mk` lists, with its table defines.
- **Optimisation flags:** `-O3 -fno-schedule-insns -fno-schedule-insns2`. With GCC's instruction scheduling off, the order of the DSP's statements is the order of its instructions, which the engine relies on (`docs/M7_PERFORMANCE.md`).
- **Build id:** the build writes `build_id.c` with the short commit hash, plus `+dirty` when tracked files differ. Untracked files do not make it dirty.
- **Flashing:** `flash_when_ready.sh [FILE.bin]` needs no button presses while Ducky is running. If the Seed runs other firmware, either send that firmware's own DFU command or hold BOOT, tap RESET, release BOOT. Then run the script.
- **Several Seeds attached:** every Seed enumerates as `0483:5740`. Pick Ducky's by its USB path (`BUS-PORT`, as in `/sys/bus/usb/devices` and `dfu-util -p`; the physical port, so it survives the DFU reset): `uv run tools/usb_log.py 10 --port 3-4` and `DK_USB_PORT=3-4 tools/flash_when_ready.sh`. Without it, `usb_log.py` prints `-- usb: N Seeds attached (…); pass --port` and exits 2, and the flasher refuses when more than one `0483:5740`/`0483:df11` is present. `--identity` shows which pedal a path holds (`device 64` is Ducky).
- **Port conflicts:** `usb_log.py` and `flash_when_ready.sh` claim the USB-MIDI interface through libusb. They detach the kernel driver, which drops the ALSA port while they run. A program holding the ALSA port does not block them. It does block `amidi`, which then fails with "Device or resource busy"; a browser tab using Web MIDI is a common holder.
- **Build warnings:** libDaisy's startup file, newlib's `_read`/`_write`/… stubs and the linker's "RWX segment" all warn on every build. Ducky's own sources build without warnings.

## Source layout

| Path | Contents | Host-portable |
|---|---|---|
| `daisy/src/ducky.cpp` | The pedal: the compiled-in model and IR, `AudioCallback`, the main loop, the log lines. | no |
| `daisy/src/bench.cpp` | `APP=bench`: the A2-Lite and IR device checks and the cycle timing of the engine, the IR, the EQ and the whole chain ("Bench"). | no |
| `daisy/src/board.h` | `dkboard::Board`: the HotHouse pins, knobs, toggles, footswitches. `kBoardId` 1. | no |
| `daisy/src/firmware.h` | `dkfw::Start` (flush-to-zero, board init, MPU) and `kBuildId`. | no |
| `daisy/src/usb_link.h/.cpp` | `dkusb`: USB-MIDI transport, a non-blocking double-buffered send, `Serve` (Identity, HELLO, DFU), `EnterDfu`. | no |
| `daisy/src/link.h` | `dklink`: the SysEx wire format, `TxBuffer`, `SysExAssembler`, `PedalCommand`. The protocol reference. | yes |
| `daisy/src/controls.h` | `dkctl::ToggleFilter` (30 ms toggle hysteresis). | yes |
| `daisy/src/dsp/blocks.h` | `dkdsp::kSampleRate` (48 kHz), `kBlockSize` (48), `kIrTaps` (2048). | yes |
| `daisy/src/dsp/mute.h` | `dkdsp::MuteRamp`: FS1's 5 ms linear mute. | yes |
| `daisy/src/dsp/gain.h` | `dkdsp::InputGain` (Knob 1), `OutputGain` (Knob 2), `LevelMatchGain`, `Smoother` (one-pole), `GainRamp` (per-block linear gain ramp). | yes |
| `daisy/src/dsp/a2lite.h` | `dkdsp::A2Lite`: the A2-Lite network (`docs/NAM_FORMAT.md` "Engine"); `dkdsp::a2` holds the shape and memory layout; `dkdsp::FpHere`. | yes |
| `daisy/src/dsp/convolver.h` | `dkdsp::IrConvolver`: the partitioned FFT IR convolver (`docs/FILTERS.md`). | yes |
| `daisy/src/dsp/filters.h` | `dkdsp::Eq` (`CutFilter` ×2, `MidFilter`), `EdgeBypass`, the knob and toggle maps (`docs/FILTERS.md`). | yes |
| `daisy/src/model.h` | `dkmodel::Parse`: validates and decodes a format-1 model binary; `Crc32`. | yes |
| `daisy/src/ir.h` | `dkir::Parse`: validates and decodes a format-1 IR binary. | yes |
| `daisy/cmsis_fft.mk` | The CMSIS-DSP FFT sources and table defines both builds compile. | |
| `daisy/src/memory.h/.cpp` | `DK_ITCM`, `DK_DTCM`, `DK_DTCM_BSS`, `DK_D2`, `DK_SDRAM` placement and the boot-time TCM copy. | yes (`.h`) |
| `daisy/src/spsc_ring.h` | The lock-free single-producer/single-consumer ring (USB IRQ → main loop). | yes |
| `daisy/STM32H750IB_sram_ducky.lds` | libDaisy's BOOT_SRAM script plus the `.dtcm_data`, `.dtcm_bss`, `.itcm_text`, `.d2_bss`, `.sdram_bss` sections and the 16 KB stack reserve. | |
| `tests/` | GoogleTest for the host-portable headers. Never include `board.h`, `firmware.h` or `usb_link.h` here. | |
| `tools/usb_log.py`, `tools/flash_when_ready.sh` | Log reader with `--port`, `--identity` and `--dfu`, and the flasher (`DK_USB_PORT`). | |
| `editor/src/nam/`, `tools/nam_convert.ts`, `tools/nam_reference.sh`, `tools/make_stimuli.py` | The `.nam` → model binary converter and its CLI, the NAM reference renders, and their stimulus (`docs/NAM_FORMAT.md`). | |
| `editor/src/ir/`, `tools/ir_convert.ts`, `tools/make_ir_fixture.ts` | The `.wav` → IR binary converter and its CLI, and the generator of `tests/fixtures/ir/decay.wav` (`docs/FILTERS.md`). | |
| `editor/src/name.ts` | `sanitiseName`, shared by both converters. | |
| `tools/render.cpp`, `tools/wav.h` | `build/host/render` (`make render`): the engine, then the IR with `--ir`, over a WAV file; `dkwav` WAV reading and float32 writing (host only). | |
| `tests/fixtures/nam/` | Upstream MIT `.nam` and WAV fixtures, `A2.bin` converted from `A2.nam`, and the committed `A2.nam` reference render. | |
| `tests/fixtures/ir/` | `decay.wav` (generated: 44.1 kHz decaying noise, 4,410 frames) and `decay.bin` converted from it. | |

- **Naming:** namespaces are `dkboard`, `dkfw`, `dkusb`, `dklink`, `dkctl`, `dkdsp`, `dkmodel`, `dkir` and (host only) `dkwav`. Macros use the `DK_` prefix, and the board define is `DUCKY_BOARD_HOTHOUSE`.
- **DSP code** goes under `daisy/src/dsp/`, host-portable and covered by `tests/`. The top-level `Makefile` rebuilds the tests and `render` whenever any `daisy/src/*.h`, `daisy/src/dsp/*.h`, `tests/*.h` or `tools/*.h` changes. Tests find fixtures through `DK_REPO_ROOT`, so `build/host/tests` runs from any directory.
- **Before writing or optimising a DSP kernel,** read `docs/M7_PERFORMANCE.md`: what float code costs on this core, measured, and the rules that follow. Host tests cannot see performance; the bench can.

## Threads

- **`AudioCallback`** is the audio DMA interrupt at 48 kHz, in 48-sample blocks (1 ms). It runs from ITCM, and its state, the engine and the EQ included, lives in DTCM; the IR convolver lives in AXI SRAM. In order, it:
  1. reads the controls;
  2. toggles the mute on FS1;
  3. measures the input peak;
  4. runs the chain on the left input, in place in the left output buffer:
     - the input trim: the model's input calibration times Knob 1, smoothed (one-pole, 20 ms) and ramped linearly across the block, −12…+12 dB around the calibration, which is at noon;
     - `A2Lite::Process`, also while muted, so the load stays constant and the model stays warm;
     - `IrConvolver::Process` when an IR is compiled in;
     - `Eq::Process`: low-cut (Knob 5, Knob 6, SW1), the SW3 mid, high-cut (Knob 4, Knob 3, SW2); every section runs every block (`docs/FILTERS.md`);
     - the output gain: Knob 2 smoothed and ramped the same way, times the level match. Knob 2 is `(2k)^3` up to noon (off at fully CCW, about −18 dB at 9 o'clock, unity at noon), then dB-linear to +12 dB;
     - `MuteRamp`;
  5. copies the left output to the right and measures the output peak;
  6. drives the LEDs;
  7. measures the load and the underruns (a callback longer than one block period).
- **Input calibration:** NeuralAmpModelerPlugin's: the input gain at noon is `kInputLevelDbu − input_level_dbu`, so a guitar drives the model as hard as it drove the captured amp. `kInputLevelDbu` is +4.5 dBu, the level at the input jack that reaches 0 dBFS, derived from the datasheets and not measured: the PCM3060 full scale is 0.6 VCC Vpp (VCC 4.5 V), behind the Seed's 3.6 kΩ series resistor into the codec's 10 kΩ input (3.67 Vpp), and the HotHouse input is a unity-gain buffer. A model captured at +15 dBu gets −10.5 dB. A model without `input_level_dbu` is assumed captured at +12 dBu, the plugin's default calibration level (−7.5 dB). Without calibration, noon drove the models about 10 dB harder than at capture, and they sounded distorted.
- **Level match:** `LevelMatchGain` brings the model's A2-Lite `loudness` to −18 dB (NeuralAmpModelerPlugin's normalisation target); a model without `loudness` plays at unity. `loudness` is measured at the capture level, before the input calibration, so a clean model plays a little quieter than a driven one at noon (SSS vs Fender: 1.8 dB, `docs/ROADMAP.md` M4). At boot the input gain starts at the calibration and the output gain at 0, so the output fades in over the knob time constant.
- **Main loop** runs `dkusb::Serve(logBoot)` and the once-a-second log lines. If the compiled-in model or IR does not parse (or the IR's FFT does not initialise), audio never starts and the LEDs stay off; the main loop serves USB (DFU included) and logs the `model` and `ir` lines once a second.
- **Sharing:** the two threads share only relaxed `std::atomic`s, each with one writer:
  - callback → main loop: `gKnob`, `gToggle`, `gFsHeld`, `gMutedPub`, `gUnderruns`, `gPeak`, `gOutPeak`, `gLoadAvg`, `gLoadMax`;
  - main loop → callback: `gPeakReset`, `gLoadReset`.

## Memory

| Region | Size | Holds now | Reserved for |
|---|---|---|---|
| ITCM `0x00000000` | 64 KB | `AudioCallback` (with the engine's other stages, the convolver, the mid and the gains inlined), `A2Lite::RunLayer`, `CutFilter::Process` (8 KB in all) | |
| DTCM `0x20000000` | 128 KB | The engine (99,124 bytes in `.dtcm_bss`: history 90,600, weights 7,480); callback state; the 16 KB stack reserve at the top | |
| AXI SRAM `0x24000000` | 480 KB | Code (CMSIS-DSP's FFT included), `.data`, `.bss` with the IR convolver (68 KB): 244 KB in total | |
| D2 SRAM `0x30008000` | 256 KB | — (the convolver measured slower here than in AXI SRAM, `docs/FILTERS.md`) | |
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
model loudness -24.0 dB level +6.0 dB input +14.8 dBu cal -10.3 dB "A2"                       (boot / HELLO)
ir taps 2048 src 48000 Hz gain -7.2 dB "YA FDMN 412 BLND Mix 01"                              (boot / HELLO)
ctl k 31 22 33 58 25 34 sw 2 0 1 fs 0 0                                                       (every second)
load avg 361 max 363 permille underruns 0 mute off in -84 dBFS out -31 dBFS                   (every second)
```

- **`model` line:** the compiled-in model's `loudness` and `input_level_dbu` (`none` without them), the level-match and input-calibration gains, in tenths of a dB, then its name, last because log lines stop at 120 characters. When the model does not parse it reads `model <status>` (`dkmodel::StatusName`) instead, repeated every second.
- **`ir` line:** the compiled-in IR's tap count, source rate, the converter's unit-energy gain, and its name. `ir none` without an IR (`IR=` unset). When the IR does not parse it reads `ir <status>` (`dkir::StatusName`), or `ir fft` when the convolver's FFT does not initialise, repeated every second with the `model` line.
- **`ctl` line:**
  - `k`: knobs 1–6, in percent.
  - `sw`: SW1–SW3 after the 30 ms filter, as 2 up, 1 middle, 0 down.
  - `fs`: FS1 and FS2 held.
- **`load` line:**
  - `avg` and `max`: libDaisy's `CpuLoadMeter`, in permille of the block period, measured over the last second.
  - `underruns`: the count since boot.
  - `mute`: FS1's state, which is not persisted.
  - `in`: the left-input peak over the last second, floored at −120 dBFS.
  - `out`: the left-output peak over the last second, after the mute, floored at −120 dBFS. It does not reach −120 with no signal: the model has a DC offset at zero input (`A2.bin`: −60 dBFS, before the output gain).

## Measured (M0, Seed 1.2 / PCM3060)

| | |
|---|---|
| Passthrough load | avg 6, max 7 permille |
| Underruns | 0 |
| DTCM | 16.5 KB of 128 KB (including the 16 KB stack reserve) |
| ITCM | 1.2 KB of 64 KB |
| SRAM | 124 KB of 480 KB |
| Image | 96,796 bytes |

## Measured (M4, Seed 1.2 / PCM3060, `A2.bin`)

| | |
|---|---|
| Load | avg 361, max 363 permille |
| Underruns | 0 |
| DTCM | 115,704 bytes of 128 KB (engine 99,124, the 16 KB stack reserve) |
| ITCM | 5,192 bytes of 64 KB |
| SRAM | 148 KB of 480 KB (the parsed `dkmodel::Model` 7.5 KB, the embedded binary 7.5 KB) |
| Image | 113,672 bytes |

## Measured (M5, Seed 1.2 / PCM3060, `A2.bin`, `IR=irs/fdmn_blnd_mix01.bin`)

| | |
|---|---|
| Load | avg 499, max 501 permille over 5 minutes (302 log lines) |
| Load without `IR=` | avg 392, max 393 permille |
| Underruns | 0 |
| DTCM | 115,872 bytes of 128 KB |
| ITCM | 8,072 bytes of 64 KB |
| SRAM | 244 KB of 480 KB (the convolver 68 KB, the parsed `dkir::Ir` 8.3 KB, the embedded binary 8.1 KB) |
| Image | 135,416 bytes |

- **Level:** the IR's unit-energy normalisation makes a cab IR louder than no IR for an amp model: on `A2.bin` with `input.wav` the render's RMS rises 10.7 dB and its peak 11.9 dB (the FDMN mix peaks at +12.2 dB at 129 Hz). On the pedal at the same controls the `out` peak went from −9…−15 dBFS to −9…0 dBFS.

## Bench (M3, M5)

`APP=bench` (`daisy/src/bench.cpp`) has no audio. It compiles in `tests/fixtures/nam/A2.bin`, `tests/fixtures/ir/decay.bin` and a slice of `input.wav` and `A2.input.ref.wav` (`.incbin`; the Makefile rebuilds `bench.o` when they change). It places the engine like the pedal does: the object in DTCM, its stages and the timed blocks in ITCM; the EQ in DTCM and the pedal's convolver (`gIrAxi`) in AXI SRAM, with a second one (`gIrD2`) in D2 SRAM for comparison. Each pass:

1. **Check:** a reset engine runs `input.wav[48000, 72000)` (the file's first second is silence, so a reset engine is already in that state) against the reference render; limit 1e-5.
2. **IR check:** the reset convolver runs `input.wav[48000, 52800)` against a direct convolution computed on the device in double; limit 1e-5.
3. **Model:** `A2Lite::Process` on 48-sample blocks of uniform noise (±0.5), timed with the DWT cycle counter: 200 warm-up blocks, then 2000 measured, interrupts masked inside each block. The M3 gate is the maximum against 450 permille of the block period.
4. **Layers:** the same blocks, each stage (`BeginChunk`, `RunLayer` per layer, `EndChunk`) timed separately.
5. **IR, EQ, chain:** the same way, the convolver from D2 and from AXI SRAM; the EQ at mid-travel knobs, top resonance, SW3 up, with SW1/SW2 flipping between 24 and 12 dB every 10 blocks so slope fades are in the maximum; and the pedal's whole chain (input gain, engine, IR, EQ, output gain, mute). The M5 gate is the chain's maximum against 550 permille.

Passes repeat every 2 s; the DFU command works between stages, so `flash_when_ready.sh` returns to the pedal with no button presses.

```
ducky_bench hothouse sysclk 480 MHz 999fce4+dirty                                 (boot / HELLO)
bench model "A2" ok
bench ir "decay" ok
bench pass 1 budget 480000 cycles
bench check samples 24000 max err 241e-9 limit 10000e-9 ok
bench ir check samples 4800 max err 214e-9 limit 10000e-9 ok
bench a2lite cycles avg 168369 max 168936 permille avg 350.7 max 351.9
bench gate a2lite max 351.9 permille limit 450 go
bench input cycles avg 1368 max 1381 permille avg 2.8 max 2.8
bench layer 00 k6 d1 cycles avg 6658 max 6718 permille avg 13.8 max 13.9        (… layer 22)
bench head cycles avg 4765 max 4766 permille avg 9.9 max 9.9
bench ir d2 cycles avg 64440 max 66076 permille avg 134.2 max 137.6
bench ir axi cycles avg 51735 max 52545 permille avg 107.7 max 109.4
bench eq cycles avg 15135 max 15264 permille avg 31.5 max 31.8
bench chain cycles avg 237192 max 238301 permille avg 494.1 max 496.4
bench gate chain max 496.4 permille limit 550 go
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

### Measured (M5, Seed 1.2)

| Stage | Avg cycles | Max permille |
|---|---|---|
| Whole model (`a2lite`) | 168,561 | 352.4 |
| IR convolver, D2 SRAM (`ir d2`) | 64,440 | 137.4–138.2 |
| IR convolver, AXI SRAM (`ir axi`, the pedal's) | 51,735 | 109.2–109.8 |
| EQ (`eq`) | 15,135 | 31.7–31.8 |
| Whole chain (`chain`) | 237,192 | 496.4 |

- **Gate:** go, 496.4 permille against 550. With the convolver in D2 SRAM the chain measured 524.6.
- **IR check:** max error 214e-9.
- **IR placement:** the convolver's 66 KB of spectra pass through the 16 KB D-cache every block; from AXI SRAM it costs 21 % less than from D2 SRAM, so the pedal keeps it in `.bss`.
- **Image:** bench 299,728 bytes; SRAM 94.6 % (the second convolver and the IR check's buffers).

How the engine got here from 570 permille, the measured cost model behind it, and how to check an image for a regression: `docs/M7_PERFORMANCE.md`. Run the bench after any change to the engine, the compiler or its flags: the gate line is the only test that sees a slowdown.
