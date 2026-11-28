# Firmware

The pedal firmware in `daisy/`, plus the host tests and tools that go with it. Every command runs from the repo root.

## Getting started

These steps take a new machine and a new Seed to a playing pedal. Every command runs from the repo root. The command-line tools are for Linux; the editor runs in Chrome anywhere.

### 1. Tools

| Tool | For | Tested with |
|---|---|---|
| `git`, `make` | everything | |
| Arm GNU Toolchain (`arm-none-eabi-gcc`), from https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads, its `bin/` on `PATH` | the firmware | 13.3.rel1 |
| `dfu-util` | flashing | 0.11 |
| `lsusb` (usbutils) | the flasher | |
| `uv` | the Python tools: the log and the flasher's DFU command (`tools/usb_log.py`), the morph's path trainer (`tools/morph_train.py`) | 0.12.19 |
| Bun | the editor and `tools/*.ts` | 1.4.4 |
| `g++`, `pkg-config`, GoogleTest (`pkg-config gtest_main`) | the host tests only | |

USB access without root:

```
# /etc/udev/rules.d/70-daisy.rules
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="5740", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0483", ATTRS{idProduct}=="df11", TAG+="uaccess"
```

Then `sudo udevadm control --reload-rules && sudo udevadm trigger`, and replug the Seed. `5740` is the running Seed (USB-MIDI, `usb_log.py`); `df11` is its DFU bootloader. `tools/pedal.ts` also needs Linux with ALSA and read/write access to `/dev/snd/midiC*D0` (group `audio`, or the seat ACL a desktop session grants).

### 2. Clone and set up

```sh
git clone --recursive https://github.com/jimbattin/ducky.git ducky && cd ducky
make -C daisy/libDaisy   # daisy/libDaisy/build/libdaisy.a, once
uv sync                  # .venv with pyusb for tools/usb_log.py
```

In an existing clone, `git submodule update --init --recursive` fetches libDaisy v9.0.0 and its driver submodules (HAL, CMSIS_5, CMSIS-DSP, USB device). `make -C editor` installs the editor's dependencies on first use. The weight build's trained paths need more on the host (PyTorch, fetched by `uv` on the trainer's first run, and a guitar DI recording): `docs/MORPH.md` "Host setup for a trained path".

### 3. Build

```sh
make -C daisy -j   # daisy/build/hothouse-xfade/ducky_xfade.bin; the link prints the memory table
make -C daisy BOARD=burningbungalow -j   # a BurningBungalow's: daisy/build/burningbungalow-xfade/ducky_xfade.bin
```

| `APP=` | Image | `.bin` (HotHouse; `burningbungalow-` for `BOARD=burningbungalow`) | More |
|---|---|---|---|
| `xfade` (the default) | the pedal, the xfade build: a morph preset's two models crossfaded | `daisy/build/hothouse-xfade/ducky_xfade.bin` | `docs/MORPH.md` |
| `weight` | the pedal, the weight build: the weight morph | `daisy/build/hothouse-weight/ducky_weight.bin` | `docs/MORPH.md` "On the pedal: the weight morph" |
| `bench` | the benchmark and device check, no audio | `daisy/build/hothouse-bench/ducky_bench.bin` | "Bench" |
| `latency` | the FX loop's round trip | `daisy/build/hothouse-latency/ducky_latency.bin` | `docs/LOOP.md` |

`make test` runs the host tests; flashing does not need it.

### 4. The Daisy bootloader (a new Seed only)

Ducky is a `BOOT_SRAM` image: the Daisy bootloader, in the STM32's internal flash, copies it from QSPI `0x90040000` into SRAM at every boot. A Seed that never ran a bootloader image needs the bootloader once. To check, tap RESET: the Seed's LED pulsing for about 2 s before the firmware starts is the bootloader. To install it (BOOT and RESET are the buttons on the Seed itself):

1. Hold BOOT, tap RESET, release BOOT: the STM32's ROM DFU (`0483:df11`), which writes internal flash only.
2. `make -C daisy program-boot` writes libDaisy's `dsy_bootloader_v6_4-intdfu-2000ms.bin` to `0x08000000` (libDaisy's core Makefile).
3. The bootloader starts. With no firmware in QSPI it waits in its own DFU mode: go to step 5.

Installing it again is harmless: the presets, the settings and the firmware in QSPI are not touched.

### 5. Flash

```sh
tools/flash_when_ready.sh                                                 # the xfade build
tools/flash_when_ready.sh daisy/build/hothouse-weight/ducky_weight.bin    # the weight build
```

The script needs no button. If Ducky runs, it sends the link's DFU command over USB-MIDI and waits up to 120 s for the bootloader's DFU device, then runs `dfu-util … -s 0x90040000:leave`, and the pedal restarts into the new image. If the Seed runs other firmware, tap RESET, then press BOOT while the LED pulses (the bootloader stays in DFU), then run the script. Without the script (any OS with `dfu-util`, the Seed in the bootloader's DFU): `make -C daisy program-dfu` (`APP=weight` for the weight build). Later updates of a running Ducky need no `dfu-util`: `bun tools/pedal.ts firmware FILE.bin` or the editor's Firmware button ("Firmware update").

### 6. Check it runs

```sh
uv run tools/usb_log.py 10 --identity
```

It prints the Identity Reply (`device 64` is Ducky), then the boot lines (`ducky hothouse seed … block 24 <build id>`, "Log lines"). A new pedal has no presets and plays dry: load models with the editor (`README.md` "Editor") or `bun tools/pedal.ts upload 1 MODEL.nam --ir CAB.wav`.

## Commands

```sh
make test                        # host GoogleTest: build/host/tests (tests/*.cpp); also builds build/host/render and fails when docs/MIDI.md is stale
make midi-doc                    # regenerates docs/MIDI.md (tools/gen_midi_doc.cpp)
make morph-check                 # the morph path trainer (tools/morph_train.py, under uv) against tests/fixtures: its network and its fit
DK_USB_PORT=3-4 make firmware-check   # the firmware update over USB-MIDI, unattended on the attached pedal (tools/firmware_check.ts, "Firmware update"); BOARD=burningbungalow on a BurningBungalow
make -C daisy -j                 # daisy/build/hothouse-xfade/ducky_xfade.bin: the xfade build
make -C daisy APP=weight -j      # daisy/build/hothouse-weight/ducky_weight.bin: the weight build (docs/MORPH.md)
make -C daisy APP=bench -j       # daisy/build/hothouse-bench/ducky_bench.bin ("Bench" below)
make -C daisy APP=latency -j     # daisy/build/hothouse-latency/ducky_latency.bin: the FX loop's round trip (docs/LOOP.md)
make -C daisy BOARD=burningbungalow APP=… -j   # the same images for a BurningBungalow, in daisy/build/burningbungalow-*/
tools/flash_when_ready.sh [FILE.bin [ADDR]]   # sends DFU over USB-MIDI, waits for 0483:df11, flashes ADDR (default 0x90040000)
mkdir -p build && bun tools/preset_bank.ts -o build/bank.bin --slot 1 MODEL.nam --ir IR.wav --slot 2 …   # preset bank ("Preset store")
tools/flash_when_ready.sh build/bank.bin 0x90400000                                   # writes all 8 slots
uv run tools/usb_log.py 10 --identity   # 10 s of log, preceded by the Identity Reply
bun tools/pedal.ts list                 # the pedal over the link while it plays: list, info, upload, erase, rename, swap, gate, morph, param, status, firmware ("Link protocol")
bun tools/pedal.ts firmware daisy/build/hothouse-xfade/ducky_xfade.bin   # a firmware update over USB-MIDI, no dfu-util ("Firmware update")
make -C editor test check        # editor and converter tests (bun:test), tsc, Biome
make -C editor serve             # the editor with hot reload at http://localhost:3000/ (Chrome; allow MIDI with SysEx)
make -C editor html              # editor/dist/editor.html: the editor as one self-contained file
bun tools/nam_convert.ts models/*.nam   # .nam -> .bin next to each input (docs/NAM_FORMAT.md)
bun tools/ir_convert.ts IR.wav...       # .wav -> IR .bin next to each input (docs/FILTERS.md)
tools/nam_reference.sh           # NAM reference renders (docs/NAM_FORMAT.md "Reference renders")
```

- **Build options:** `daisy/Makefile` takes `BOARD=hothouse` (the default, Cleveland Music Co. HotHouse) or `BOARD=burningbungalow` (the HotHouse fork with an expression pedal jack and a TRS MIDI IN), and `APP=xfade` (default), `APP=weight`, `APP=bench` or `APP=latency`. Each board's image refuses to install on the other (the firmware tag's board, "Firmware update"); presets and settings records are the same on both. The pedal has two builds: the **xfade build** (`APP=xfade`, the default) and the **weight build** (`APP=weight`). Each board and app has its own build directory, `daisy/build/<BOARD>-<APP>`, so no object is shared between them, and its own image name, `ducky_<APP>.bin`, so the two pedal builds can be copied side by side into a release.
- **The morph:** the xfade build (`DK_MORPH_XFADE=1`, `daisy/src/ducky.cpp`'s default) runs a morph preset's model and its target's and `morph` crossfades their outputs (`docs/MORPH.md`; "The xfade build" under "Threads"; `docs/MEASUREMENTS.md` "Live load", "Morph"). The weight build is the same sources built with `-DDK_MORPH_XFADE=0`: one model runs, its weights blended into the target's, straight or along a trained path (`docs/MORPH.md` "On the pedal: the weight morph"). Flash it with `tools/flash_when_ready.sh daisy/build/hothouse-weight/ducky_weight.bin`; `make -C daisy -j && tools/flash_when_ready.sh` returns to the xfade build. Both read the same presets and settings and write nothing the other does not, so the two can be swapped freely. The weight build sets bit 0 of the Identity Reply's flags (`dklink::kIdentityWeightMorph`, `docs/MIDI.md`), so hosts know which morph plays: `usb_log.py --identity` prints `morph weight` or `morph crossfade`, the editor shows the fit and trained paths only for the weight build (`docs/EDITOR.md`), and `pedal.ts morph … --train`/`--path` warns that the xfade build does not play the path. Its boot line ends ` weight` ("Log lines").
- **Presets** are not part of the build: they live in QSPI and are written with a bank image, or slot by slot over the link with `tools/pedal.ts` ("Preset store"). An empty bank plays dry.
- **CMSIS-DSP:** `libdaisy.a` has no CMSIS-DSP, so every build compiles the FFT sources `daisy/cmsis_fft.mk` lists, with its table defines.
- **Optimisation flags:** `-O3 -fno-schedule-insns -fno-schedule-insns2`, except CMSIS-DSP's FFT sources, built with `-O3` and scheduling on (`daisy/Makefile`). With GCC's instruction scheduling off, the order of the DSP's statements is the order of its instructions, which the engine relies on (`docs/M7_PERFORMANCE.md`).
- **Build id:** the build writes `build_id.c` with the short commit hash, plus `+dirty` when tracked files differ. Untracked files do not make it dirty.
- **Flashing:** `ADDR` is `0x90040000` (firmware, the default) or `0x90400000` (a preset bank); anything else is refused, and so is a missing file. Both end with `:leave`, so the pedal restarts into Ducky.
- **Several Seeds attached:** pick Ducky by its USB path (`BUS-PORT`, as in `/sys/bus/usb/devices` and `dfu-util -p`; the physical port, so it survives the DFU reset): `--port 3-4` for `usb_log.py` and `pedal.ts`, `DK_USB_PORT=3-4` for them and the flasher. With several Seeds and no path, `usb_log.py` and `pedal.ts` list them and exit 2 and the flasher refuses. `--identity` shows which path holds Ducky (`device 64`); `pedal.ts` checks it before every command (`pedal: no Ducky on 3-4`).
- **Port conflicts:** `usb_log.py` claims the USB-MIDI interface through libusb, and so does `flash_when_ready.sh`, through `usb_log.py --dfu`, when no DFU device is present. They detach the kernel driver, which drops the ALSA port while they run (`pedal.ts` then finds `no Seed MIDI port`). A program holding the ALSA port does not block them. It does block `amidi` and `pedal.ts`, which open the ALSA rawmidi device exclusively (`Device or resource busy`, `pedal: /dev/snd/midiC1D0 busy: close other MIDI programs (browser Web MIDI, amidi, tools/usb_log.py)`); a browser tab using Web MIDI is a common holder. So `pedal.ts status` and the other `pedal.ts` commands cannot run at the same time.
- **Build warnings:** libDaisy's startup file, newlib's `_read`/`_write`/… stubs and the linker's "RWX segment" all warn on every build. Ducky's own sources build without warnings.

## Source layout

| Path | Contents | Host-portable |
|---|---|---|
| `daisy/src/ducky.cpp` | The pedal: the image tag (`kImageTag`), the boot slot scan, `AudioCallback`, the parameter step (`stepParams`), the preset switch (`servePresets`), the state record (`writeState`), the settings record (`saveSettings`), the link's slot, parameter and firmware commands (`onCommand` and a function per long command) and replies (`serveReplies`), the QSPI writes (`writeFlash`, behind `writeSlot` and `writeFirmware`), the main loop, the log lines. `DK_MORPH_XFADE` picks the morph: 1 (the default, the xfade build) runs both models and crossfades them (`runXfade`, a second engine `gEngineB`), 0 (`APP=weight`, the weight build) the weight morph (`dkmorph::Prepare`, `A2Lite::Blend`). | no |
| `daisy/src/bench.cpp` | `APP=bench`: the A2-Lite and IR device checks and the cycle timing of the engine, the IR, the EQ, the noise gate, the two compressors, the morph's blend and the whole chain ("Bench"). | no |
| `daisy/src/latency.cpp` | `APP=latency`: the FX loop's round trip and the crosstalk between the channels, from clicks on each output captured on both inputs (`docs/LOOP.md` "Measuring the loop"). | no |
| `daisy/src/board.h` | `dkboard::Board`: the HotHouse pins, knobs, toggles, footswitches, and on a BurningBungalow the expression jack (`Pedal()`, the ADC channel after the knobs). `kBoardId` 1 (HotHouse) or 2 (BurningBungalow), `kHasPedal`, `kHasMidiIn`, `kKnobs` 6, `kToggles` 3. | no |
| `daisy/src/firmware.h` | `dkfw::Start` (flush-to-zero, board init, MPU), `kBuildId` and `SeedName` (the Seed's revision and codec for the boot lines). | no |
| `daisy/src/usb_link.h/.cpp` | `dkusb`: USB-MIDI transport, a non-blocking double-buffered send, `Serve` (Identity, HELLO, DFU restart, other commands to `onCommand`, Control Changes to `onControlChange`), `ServeFor` (`Serve`, repeatedly, for a time), `Restart` (into the bootloader without its DFU wait, after a firmware update). Also `__wrap_USBD_LL_SetupStage` (linked with `-Wl,--wrap`), which flushes EP0's TxFIFO when a SETUP arrives while an IN packet is still pending ("USB enumeration"). | no |
| `daisy/src/midi_in.h/.cpp` | `dkmidiin`: a BurningBungalow's TRS MIDI IN, libDaisy's `MidiUartTransport` on USART1 (RX D14), bytes through an IRQ ring to `Serve`, which passes Control Changes to `onControlChange`. | no |
| `daisy/src/midi.h` | `dkmidi`: the Control Change parser (`CcParser`, running status included), `kExpressionCc` (11), `ChannelMatches` for EXP's channel. | yes |
| `daisy/src/link.h` | `dklink`: the SysEx wire format, the septet and Pack7 codecs, `TxBuffer` (LOG, SLOT, RESULT, STATUS, DATA, PARAM, VALUES, Identity), `SysExAssembler`, `PedalCommand`, and the protocol tables `kCommands`/`kResults` that `docs/MIDI.md` is generated from. The protocol reference. | yes |
| `daisy/src/upload.h` | `dklink::Upload` and `dklink::FirmwareUpload`, on their shared `Staging`: UPLOAD and FIRMWARE BEGIN/CHUNK staging (`StageChunk`: CRC per chunk, resends, gaps). | yes |
| `daisy/src/fw_image.h` | `dkfwimg`: a firmware image's tag (`Tag` at `0x400`) and FIRMWARE COMMIT's checks (`CheckImage`), the image's QSPI place and size bounds ("Firmware update"). | yes |
| `daisy/src/controls.h` | `dkctl::ToggleFilter` (30 ms toggle hysteresis), `dkctl::Blinker` (LED2's N blinks, repeated after 5 s dark). | yes |
| `daisy/src/params.h` | `dkparam`: the parameter table (`kParams`, `ParamDef`, `Units`), the control slots (`Accepts`), what a knob or a toggle gives a parameter (`FromKnob`, `FromToggle`), the wire codes (`ToCode`, `FromCode`). `docs/MIDI.md` "Parameters" is printed from it. | yes |
| `daisy/src/param_store.h` | `dkparam::Store`: the live values and assignments and the arbitration between the controls and the link's SET and ASSIGN (pickup, last control wins). | yes |
| `daisy/src/settings.h` | `dkparam`: the settings record (`BuildSettings`, `SettingsValid`, `NewestSettings`, `DecodeSettings`, `FactorySettings`) and its QSPI sectors ("Settings record"). | yes |
| `daisy/src/dsp/blocks.h` | `dkdsp::kSampleRate` (48 kHz), `kBlockSize` (24), `kBlockRate`, `kBlocksPerMs`, `kInvBlockSize`, `kIrTaps` (2048); `Peak` (a block's peak), `FpHere` and `CopyFloats` (`docs/M7_PERFORMANCE.md`); `FinishCrossfade`, the compressors' and the EQ's on/off crossfade over `kCrossfadeSamples`. | yes |
| `daisy/src/dsp/mute.h` | `dkdsp::MuteRamp`: FS1's 5 ms linear mute, the preset switch's fade and the loop mode's; `ProcessPair` fades two buffers with one ramp (the output and the send). | yes |
| `daisy/src/dsp/noise_gate.h` | `dkdsp::NoiseGate`, `GateSettings`, `kGateOff`, `GateValid`, `ClampGate`: the per-preset side-chained gate (`docs/FILTERS.md` "Noise gate"). | yes |
| `daisy/src/dsp/gain.h` | `dkdsp::InputGain` (Knob 1), `OutputGain` (Knob 2), `LevelMatchGain`, `CompBalanceTaper` (both compressors' Balance), `OnePoleAlpha` and `Smoother` (one-pole), `GainRamp` (per-block linear gain ramp). | yes |
| `daisy/src/dsp/a2lite.h` | `dkdsp::A2Lite`: the A2-Lite network (`docs/NAM_FORMAT.md` "Engine"), with `ToEngine` (a weight stream in the engine's layout) and `Blend` (the weights between two sets, for the morph); `dkdsp::a2` holds the shape and memory layout. | yes |
| `daisy/src/dsp/convolver.h` | `dkdsp::IrConvolver`: the two-level partitioned FFT IR convolver (`docs/FILTERS.md`). | yes |
| `daisy/src/dsp/filters.h` | The EQ's sections: `dkdsp::CutFilter` (with `EdgeBypass`, the cut maps), `DesignBell` and `Bell` (`docs/FILTERS.md`, `docs/EQ.md`); `Prewarp` and `OnePoleGain`, which the compressors share. | yes |
| `daisy/src/dsp/eq.h` | `dkdsp::Eq`: the EQ stage, low-cut, bells 1–3, high-cut, driven by the parameters' values (`docs/EQ.md`). | yes |
| `daisy/src/dsp/compressor.h` | `dkdsp::Compressor`: the optical compressor before the input trim, the reduced OTC-201 model (`docs/compressor-spec.md` "DSP model"), driven by the parameters' values while `comp_type` is OTC; `gain()` for the meter. | yes |
| `daisy/src/dsp/ota_compressor.h` | `dkdsp::OtaCompressor`: the OTA compressor after it, the reduced Dyna Comp / Ross model (`docs/ota-compressor-spec.md` "DSP model"), on the same parameters while `comp_type` is OTA; `gain()` for the meter. | yes |
| `daisy/src/model.h` | `dkmodel::Parse`: validates and decodes a format-2 model binary (its CRC field is kept: a morph block names its target's model by it). | yes |
| `daisy/src/morph.h` | `dkmorph`: the preset morph (`docs/MORPH.md`): the morph block (`ParseBlock`, `PutBlock`), the alignment transform applied to a weight stream (`Apply`), the weight sets a blend runs on, two per segment of the path (`Prepare`, `Weights`), the parameter's blend position (`BlendPosition`) and its segment of the path (`SegmentAt`). | yes |
| `daisy/src/ir.h` | `dkir::Parse`: validates and decodes a format-1 IR binary. | yes |
| `daisy/src/preset.h` | `dkpreset`: the slot format (`Parse`, `ImageSize`, `IsImageSize`, `PutGate`/`GetGate`/`SetGate` for its gate block, `SetMorph` for its morph block and waypoints, `ResolveMorph` for the slot a preset morphs with), the state record (`StateValid`, `NewestState`, `BuildState`), slot stepping (`NextFilled`, `BootSlot`), the QSPI layout ("Preset store"). | yes |
| `daisy/src/binary.h` | `dkbin`: `Crc32`, the little-endian readers and writers the binaries and the link share, a CRC-32 trailer's check and seal (`CrcOk`, `SealCrc`) and the newest of two record copies (`NewestCopy`). | yes |
| `daisy/cmsis_fft.mk` | The CMSIS-DSP FFT sources and table defines every build compiles. | |
| `daisy/src/memory.h/.cpp` | `DK_ITCM`, `DK_DTCM`, `DK_DTCM_BSS`, `DK_D2`, `DK_SDRAM` placement and the boot-time TCM copy. | yes (`.h`) |
| `daisy/src/spsc_ring.h` | The lock-free single-producer/single-consumer ring (USB IRQ → main loop; the link's parameter edits, main loop → callback). | yes |
| `daisy/STM32H750IB_sram_ducky.lds` | libDaisy's BOOT_SRAM script plus the `.ducky_image` tag at `0x24000400`, the `.dtcm_data`, `.dtcm_bss`, `.itcm_text`, `.d2_bss`, `.sdram_bss` sections and the 16 KB stack reserve. | |
| `tests/` | GoogleTest for the host-portable headers. Never include `board.h`, `firmware.h` or `usb_link.h` here. | |
| `tools/usb_log.py`, `tools/flash_when_ready.sh` | Log reader with `--port`, `--identity` and `--dfu`, and the flasher (`DK_USB_PORT`). | |
| `tools/firmware_check.ts`, `tools/cdp.ts` | `make firmware-check`: the firmware update checked end to end on the attached pedal, through `pedal.ts` and through the editor in Chrome, driven by a minimal Chrome DevTools Protocol client ("Firmware update"). | |
| `tools/gen_midi_doc.cpp` | `build/host/gen_midi_doc`: prints `docs/MIDI.md` from `link.h`, `params.h`, `fw_image.h`, `midi.h` and `preset.h` (`make midi-doc`; `make test` diffs it). | |
| `editor/src/link/`, `tools/pedal.ts`, `tools/rawmidi.ts`, `tools/slot_source.ts` | The link's TypeScript side (`protocol.ts`: builders, codecs, parsing; `session.ts`: `PedalSession`, the upload and the firmware update with their retries; `firmware.ts`: the host's checks of a firmware image; `params.ts`: a parameter's code as its value and readout, the controls), the CLI, its ALSA rawmidi transport (with `waitForDucky`, the wait for a restarted pedal), and the model + IR → slot image loading it shares with `preset_bank.ts`. | |
| `editor/` (UI) | The browser editor; its layout: `docs/EDITOR.md` "Source layout". | |
| `editor/src/preset/info.ts` | `infoRanges`, `slotInfo`: a stored preset's metadata (its model's CRC and its morph block included) from the bytes READ fetches. | |
| `editor/src/nam/`, `tools/nam_convert.ts`, `tools/nam_reference.sh`, `tools/make_stimuli.py` | The `.nam` → model binary converter and its CLI, the NAM reference renders, and their stimulus (`docs/NAM_FORMAT.md`). | |
| `tools/comp_reference.py` | The compressor's Python reference (`docs/compressor-spec.md` appendix B verbatim) and the generator of `tests/fixtures/comp/` (`uv run tools/comp_reference.py`). | |
| `tools/ota_reference.py` | The OTA compressor's Python reference (`docs/ota-compressor-spec.md` appendix B verbatim) and the generator of `tests/fixtures/ota/` (`uv run tools/ota_reference.py`), on `comp_reference.py`'s stimuli. | |
| `editor/src/ir/`, `tools/ir_convert.ts`, `tools/make_ir_fixture.ts` | The `.wav` → IR binary converter and its CLI, and the generator of `tests/fixtures/ir/decay.wav` (`docs/FILTERS.md`). | |
| `editor/src/preset/`, `tools/preset_bank.ts`, `tools/make_preset_fixture.ts` | The slot and bank builders (`buildSlot`, `buildBank`), the gate block and its units (`gate.ts`), the morph block, a trained path's waypoints, the trainer's path file and the slot a block resolves to (`morph.ts`), the bank CLI, and the generator of `tests/fixtures/preset/A2_decay.bin`. | |
| `editor/src/morph/`, `tools/make_morph_fixture.ts` | The morph's analysis (`docs/MORPH.md`): the network in float64 (`a2.ts`), the stimulus, the alignment fit and its transform (`align.ts`), reading two stored models and writing the block, or storing a trained path (`save.ts`); the generator of `tests/fixtures/morph/`. | |
| `tools/morph_train.py` | The morph's path trainer (`docs/MORPH.md` "The trained path"): PyTorch on the CPU under `uv`; trains a path's waypoints against the crossfade of two model binaries, measures a straight blend or a path, writes the path file `tools/pedal.ts morph N M --path` stores (`--train` runs it). | |
| `editor/src/device.ts`, `editor/src/crc32.ts` | `SAMPLE_RATE`, `NAME_MAX`, `sanitiseName`, `writeAscii`; CRC-32: shared by the converters, the preset code and the link. | |
| `tools/render.cpp`, `tools/wav.h` | `build/host/render` (`make render`): the engine, then the IR with `--ir`, over a WAV file; `dkwav` WAV reading and float32 writing (host only). | |
| `tests/fixtures/nam/` | Upstream MIT `.nam` and WAV fixtures, `A2.bin` converted from `A2.nam`, and the committed `A2.nam` reference render. | |
| `tests/fixtures/ir/` | `decay.wav` (generated: 44.1 kHz decaying noise, 4,410 frames) and `decay.bin` converted from it. | |
| `tests/fixtures/preset/` | `A2_decay.bin`: `A2.bin` with `decay.bin`, named `A2 + decay`, built by `editor/src/preset/bank.ts`; both the host tests and the editor tests check it. | |
| `tests/fixtures/morph/` | `A2_shuffled.bin`: `A2.bin`'s network in other channel coordinates (the same output), and `A2_shuffled.morph`: the morph block the analysis finds to bring it back. `tests/morph_test.cpp`, `tests/preset_test.cpp` and the editor's tests check them, which pins the transform and the block across the two languages. | |
| `tests/fixtures/comp/` | The compressor's reference fixtures from `tools/comp_reference.py`: inputs `step_30_300.in.wav`, `step_10_1000.in.wav`, `di.in.wav` (synthetic plucked notes, 1.5 s); outputs `step_30_300.s100.ref.wav`, `step_30_300.s50.ref.wav`, `step_10_1000.s100.ref.wav`, `di.ref.wav`; the static curve `static.csv`. `tests/compressor_test.cpp` checks the kernel against them and against the spec's SPICE tables. | |
| `tests/fixtures/ota/` | The OTA compressor's reference fixtures from `tools/ota_reference.py`, on `tests/fixtures/comp/`'s inputs: `step_30_300.s100.ref.wav`, `step_30_300.s50.ref.wav`, `step_10_1000.s100.ref.wav`, `di.ref.wav` and `static.csv`. `tests/ota_compressor_test.cpp` checks the kernel against them and against the spec's SPICE tables. | |

- **Naming:** namespaces are `dkboard`, `dkfw`, `dkusb`, `dkmidiin`, `dklink`, `dkctl`, `dkparam`, `dkdsp`, `dkbin`, `dkmodel`, `dkir`, `dkmorph`, `dkpreset` and (host only) `dkwav`. Macros use the `DK_` prefix, and the board defines are `DUCKY_BOARD_HOTHOUSE` and `DUCKY_BOARD_BURNINGBUNGALOW`.
- **DSP code** goes under `daisy/src/dsp/`, host-portable and covered by `tests/`. The top-level `Makefile` rebuilds the tests and `render` whenever any `daisy/src/*.h`, `daisy/src/dsp/*.h`, `tests/*.h` or `tools/*.h` changes. Tests find fixtures through `DK_REPO_ROOT`, so `build/host/tests` runs from any directory.
- **Before writing or optimising a DSP kernel,** read `docs/M7_PERFORMANCE.md`: what float code costs on this core, measured, and the rules that follow. Host tests cannot see performance; the bench can.

## Threads

- **`AudioCallback`** is the audio DMA interrupt at 48 kHz, in 24-sample blocks (0.5 ms). It runs from ITCM, and its state, the engines with their rings, the EQ, the compressor and the IR convolver included, lives in DTCM; the convolver's tail spectra live in AXI SRAM, and in the xfade build so do each engine's three largest rings (the weight build keeps its engine's rings in DTCM, "Memory"). Control timings (toggle hysteresis, LED2's blinks) are set in milliseconds and counted in blocks (`dkctl::BlocksForMs`). In order, it:
  1. reads the controls (on a BurningBungalow the expression jack too, scaled to its travel by `dkctl::PedalTravel`);
  2. toggles the mute on FS1 and counts FS2 presses;
  3. applies the link's queued SETs and ASSIGNs and the expression pedal's CC 11s (USB or MIDI IN), lets the control slots (Knobs 3–6, SW1–SW3, PEDAL) write their parameters (`dkparam::Store::Update`: pickup and holds, `docs/MIDI.md` "Controls"; EXP writes its parameter as each CC is applied), and publishes the values, the assignments and the held controls;
  4. steps the preset switch's handshake ("Preset switch" below), then the loop mode (`stepLoop`): when `loop_mode` differs from the routing in use (`gLoop`), both outputs fade out over 5 ms (`gModeFade`), the routing follows at silence, and they fade back in;
  5. measures the input peak (the raw jack, before the compressor), takes new gate settings (`gGateWord`, published by the main loop) and runs the gate's detector on that peak (`NoiseGate::Process`, every block, dry and parked included);
  6. steps the morph (`stepMorph`, `docs/MORPH.md`): the `morph` parameter is smoothed every block, and for a preset with a morph target the blend position sets the two models' crossfade gains ("The xfade build" below); then runs the chain, routed by `gLoop` (`docs/LOOP.md`). The send and return levels (`loop_send`, `loop_return`) are smoothed every block in every mode and ramped like the knobs. In MISO, on the left input, into the left output buffer, then in place:
     - `Compressor::Process` on the compressor's parameters (step 3), from the input buffer into the output buffer (`docs/compressor-spec.md` "DSP model"), then `OtaCompressor::Process` in place on that buffer (`docs/ota-compressor-spec.md` "DSP model"). Each runs while `comp_mode` is on and `comp_type` picks it; otherwise it is a bit-exact bypass after a 20 ms crossfade (the fade out keeps the blend or direct mix it had) and runs nothing, and turning it on restarts it from rest. A `comp_type` switch runs both for those 20 ms, one fading out while the other fades in;
     - the input trim: the model's input calibration times Knob 1, smoothed (one-pole, 20 ms) and ramped linearly across the block, −12…+12 dB around the calibration, which is at noon;
     - while a switch is parked, silence instead of the engine and the IR; otherwise, with a preset loaded:
       - `A2Lite::Process` (both engines and their mix, `runXfade`, for a preset with a morph target), also while muted, so the load stays constant and the model stays warm;
       - `IrConvolver::Process` when the preset has an IR;
     - `Eq::Process` on the EQ's sixteen parameters (step 3): low-cut, bells 1–3, high-cut, after the IR (`docs/EQ.md`). The cuts run every block; a bell runs while its gain is not 0 dB (and for 100 ms after); `eq_enable` off stops the stage;
     - the output gain: Knob 2 smoothed and ramped the same way, times the level match and the gate's gain (so the gate rides the same ramp, `docs/FILTERS.md` "Noise gate"). Knob 2 is `(2k)^3` up to noon (off at fully CCW, about −18 dB at 9 o'clock, unity at noon), then dB-linear to +12 dB.

     In pre-model, the compressor writes the right output buffer, scaled by the send level: the send. The right input, scaled by the return level, goes into the left output buffer, and the rest of the chain runs on it as in MISO, from the input trim on. In post-model, the chain runs as in MISO up to the engine; the engine's output, scaled by the level match, the gate's gain and the send level, goes to the right output buffer (the send). The right input, scaled by the return level, goes into the left output buffer, then the IR, the EQ and Knob 2 (the volume alone). In all three modes:
     - `MuteRamp` (FS1) on the left output, then the preset fade (`gSwapFade`, another `MuteRamp`), which in post-model fades the send with it (`ProcessPair`);
  7. in MISO, copies the left output to the right; fades both by the mode fade; measures both outputs' and the right input's peaks and accumulates the gate's level and largest cut and the compressor's lowest gain for STATUS;
  8. drives LED1 (on unless muted) and LED2 (`Blinker`);
  9. measures the load and the underruns (a callback longer than one block period).
- **Dry:** with no valid slot, the chain is the compressor, the input trim, the EQ, the output gain and the mute, with the calibration and the level match at unity and the gate off; FS2 does nothing. The pedal goes dry at boot with no valid slot, or when the link erases the last one; it leaves dry when a slot is uploaded (`gModelOn` follows the loaded preset through the switch handshake).
- **Input calibration:** NeuralAmpModelerPlugin's: the input gain at noon is `kInputLevelDbu − input_level_dbu`, so a guitar drives the model as hard as it drove the captured amp. `kInputLevelDbu` is +4.5 dBu, the level at the input jack that reaches 0 dBFS, derived from the datasheets and not measured: the PCM3060 full scale is 0.6 VCC Vpp (VCC 4.5 V), behind the Seed's 3.6 kΩ series resistor into the codec's 10 kΩ input (3.67 Vpp), and the HotHouse input is a unity-gain buffer. The Seed 3 (TAC5242) uses the same value: its datasheet gives 3.6 Vpp at the input pin (+4.3 dBu), and its loop round trip matched the Seed 1.2's within 0.5 dB (`docs/MEASUREMENTS.md` "FX loop"). A model captured at +15 dBu gets −10.5 dB. A model without `input_level_dbu` is assumed captured at +12 dBu, the plugin's default calibration level (−7.5 dB). Without calibration, noon drove the models about 10 dB harder than at capture, and they sounded distorted.
- **Level match:** `LevelMatchGain` brings the model's A2-Lite `loudness` to −18 dB (NeuralAmpModelerPlugin's normalisation target); a model without `loudness` plays at unity. `loudness` is measured at the capture level, before the input calibration, so a clean model plays a little quieter than a driven one at noon (SSS vs Fender: 1.8 dB, `docs/HISTORY.md` M4). At boot the input gain starts at the calibration and the output gain at 0, so the output fades in over the knob time constant.
- **Main loop** runs `serveReplies` (LIST's SLOTs, DUMP's PARAMs and VALUES, the pending RESULT, STATUS and VALUES every 100 ms with WATCH on), `dkusb::Serve(logBoot, onCommand, onControlChange)`, `servePresets`, the `loop` line when `loop_mode` changes, and the once-a-second log lines. If the IR convolver's FFT does not initialise, audio never starts and the LEDs stay off; the main loop serves USB (DFU included) and logs `ir fft` once a second.
- **Sharing:** the two threads share only `std::atomic`s, each with one writer, and one single-producer single-consumer ring:
  - callback → main loop, relaxed: `gKnob`, `gToggle`, `gFsHeld`, `gMutedPub`, `gUnderruns`, `gPeak`, `gOutPeak`, `gRinPeak`, `gRoutPeak`, `gLoadAvg`, `gLoadMax`, `gFs2Presses`, `gGateLevel`, `gGateGain`, `gCompGain`, `gParamPub`, `gAssignPub`, `gHeldPub`, `gShedCount` (not in the weight build); release/acquire: `gSwapParked`, `gEditsDone`;
  - main loop → callback, relaxed: `gPeakReset`, `gLoadReset`, `gBlinkCount`, `gGateWord` (the 8-byte gate block as two words: a torn pair mixes two valid settings for one block), `gMeterReset` (restarts the gate's and the compressor's meters); release/acquire: `gSwapReq`, `gSwapLoaded` (which publishes the plain `gNextLevel`, `gNextInCal`, `gNextIrOn`, `gNextModelOn`, and the morph's `gNextMorphOn`, `gNextLevelB` and `gNextCalRatioB`; the weight build: `gNextLevelDb`, `gNextInCalDb` and weights `gMorphW`, which the callback reads only while not parked, in place of the last two) and `gBlinkReq`; the ring `gEdits` (16 SETs and ASSIGNs).
- **Parameters:** the callback owns `gStore` (`dkparam::Store`, DTCM). SET and ASSIGN push an edit to `gEdits` and answer at once; the next block applies it (within 0.5 ms). DUMP and SAVE first wait until the callback has applied every edit pushed so far and published the result (`awaitEdits`: `gEditsDone` against the count pushed), so they see what the RESULTs before them promised. Its cost is in `docs/MEASUREMENTS.md` "Parameters and settings"; the knob slots write every block without comparing values, and a write tests one mask for other controls on its parameter (`docs/M7_PERFORMANCE.md`: a float compare and branch costs 6 cycles or more).
- **Gate settings:** `loadPreset` publishes the loaded preset's gate (`kGateOff` when dry) while the callback is parked, so a switch changes the gate in silence; GATE on the playing slot and GATE TRY publish while it plays, and the gate glides to the new settings.
- **Preset switch:** the engine and the convolver have two owners in turn, and the handshake hands them over.
  1. The main loop steps its target slot once per FS2 press (`NextFilled`), restarts LED2's blinks for it, and, when it differs from the loaded slot or a slot change asked for a reload (`gReload`: an upload into the playing slot), raises `gSwapReq`. The target may be dry (-1).
  2. The callback fades out over 5 ms. Once the fade lands on 0 it parks: it writes silence, touches neither the engine nor the convolver nor the gains, and stores the request in `gSwapParked`.
  3. The main loop loads the target (`A2Lite::Load` and `IrConvolver::Load` both reset their state; a morph target's model into the second engine), computes its gains, publishes its gate, and stores the request in `gSwapLoaded`.
  4. The callback takes the gains, the IR flag, the model flag (dry or not) and the morph's and fades in over 5 ms; its first block takes the `morph` parameter's position.
  Presses during a switch move the target, and the next switch follows at once, so fast presses coalesce. 2 s after the last switch, the main loop writes the slot to the state record if it changed.
- **The xfade build** (the default, `DK_MORPH_XFADE=1`): the morph, all in `ducky.cpp`:
  - **Load:** `loadPreset` loads the morph target's model into a second engine, `gEngineB` (DTCM, like the first: "Memory"), and hands over the target's level match and its input calibration over the preset's (`gNextLevelB`, `gNextCalRatioB`, published by `gSwapLoaded`).
  - **Step 6:** `stepMorph` turns the blend position t (`dkmorph::BlendPosition`: the 2 % end zones) into two gains, `level_A · cos(πt²/2)` and `level_B · sin(πt²/2)`: equal power, the target fading in slowly. For a preset with a target `runXfade` replaces `A2Lite::Process`: engine B's input is engine A's times the calibration ratio, both engines run (on every block, at every position), each output is ramped to its gain across the block (`GainRamp`) and the two are summed. The output gain, and post-model's send, then carry no level match. A preset without a target runs engine A alone; a dry or parked pedal runs neither.
  - **Step 9:** an overrun while both engines run sets `gShed`: engine B stops and engine A plays alone at its own level match (a step in level) until the next preset load. Without it a load above 100 % starves the main loop, USB and the DFU command included.
  - **Main loop:** logs `morph shed` when `gShedCount` (callback → main loop, relaxed) has moved.
- **The weight build** (`APP=weight`, `DK_MORPH_XFADE=0`) has one engine and differs in two places:
  - **Load:** `loadPreset` prepares the morph weights when the preset has a morph target (`dkmorph::Prepare` into `gMorphW`) instead of loading a second engine, and hands over both models' level matches and input calibrations in dB (`gNextLevelDb`, `gNextInCalDb`).
  - **Step 6:** for a preset with a morph target, a position that has moved blends the engine's weights (`A2Lite::Blend`) and sets the level match and the input calibration between the two models'; the first block after a load blends. There is no shed.

## Memory

| Region | Size | Holds now | Reserved for |
|---|---|---|---|
| ITCM `0x00000000` | 64 KB | `AudioCallback` (with the engines' other stages and their mix, the noise gate, the EQ's sample loops, the gains, the loop's send and return ramps, the fades and the parameter store inlined), `A2Lite::RunLayer`, `IrConvolver::Process`, `Eq::Control`, the compressors' `Control`, `SideChain` and `RunChunk` (`Compressor`, `OtaCompressor`) (28,552 bytes in all) | |
| DTCM `0x20000000` | 128 KB | Both engines (`gEngine`, `gEngineB`: 8,232 bytes each in `.dtcm_bss`, weights 7,480) and their rings but for the three of dilation 239 (`gNearA`, `gNearB`, 39,912 each, bound by `A2Lite::Bind`); the IR convolver's object (8,800: the head's spectra and the working buffers); callback state, the parameter store, the EQ and both compressors included; the 16 KB stack reserve at the top: 125,120 bytes in all (5.8 KB free) | |
| AXI SRAM `0x24000000` | 480 KB | Code (CMSIS-DSP's FFT included), `.data`, `.bss` with the IR convolver's tail spectra (`gIrPool`, 40,960 bytes, 32-byte aligned) and each engine's three rings of dilation 239 (layers 6, 13 and 22; `gFarA`, `gFarB`, 44,064 bytes each): 350 KB (358,276 bytes) with the TCMs' load images and the image tag at `0x24000400` | |
| D2 SRAM `0x30008000` | 256 KB | — (the convolver measured slower here than in AXI SRAM, `docs/FILTERS.md`) | |
| SDRAM `0xC0000000` | 64 MB | The 8 parsed presets (`gPresets`, 307,456 bytes, each with room for 3 waypoints; the gear texts stay in flash), the link's upload buffer (38,792), RENAME/SWAP/GATE/MORPH scratch (2 × 38,792), COMMIT's parse target (`gCheck`, 38,432) and FIRMWARE's staging (`gFwBuf`, 491,520): 953,784 bytes in `.sdram_bss` | |
| QSPI `0x90000000` | 8 MB | Image at `0x90040000` (480 KB at most, "Firmware update"); preset slots at `0x90400000`; state records at `0x90480000` ("Preset store"); settings records at `0x90482000` ("Settings record") | |

- **TCM copy:** `.itcm_text` and `.dtcm_data` are copied from their SRAM load image in `.preinit_array` (`memory.cpp`), before any constructor runs; `.dtcm_bss` is zeroed there.
- **No-load sections:** `.dtcm_bss` (`DK_DTCM_BSS`) is `NOLOAD`, so a large all-zero object such as the engine adds nothing to the image; only objects with an all-zero static initialiser belong there. `.d2_bss` and `.sdram_bss` are `NOLOAD` too, but nothing zeroes them; their owner clears them (`gPresets` is valid only where its slot parsed; the upload buffer only up to `Upload::next()`).
- **The rings' split:** the three rings of dilation 239 are the largest and the ones read most sparsely, so moving them out of DTCM costs the fewest cache misses per byte. The placement changes no output bit (`A2Lite.SplitRingsMatchWhole`). Image 197,736 bytes.
- **The weight build** (`APP=weight`) has one engine, with all its rings in DTCM (`gHist`, 83,976 bytes): DTCM 120,912 bytes. In AXI SRAM's `.bss` it has the morph's weight sets in the rings' place, two for each of a path's four segments (`gMorphW`, 59,844 bytes; a blend reads one segment's 15 KB): AXI SRAM 330,564 bytes. In SDRAM, the morph's scratch (`gMorphScratch`, 7,480): 961,264 bytes. ITCM 28,312 (the blend inlined in `AudioCallback`); image 198,288 bytes.
- **Template placement:** GCC 13 drops section attributes from template instantiations. A hot template reaches ITCM only when inlined into a `DK_ITCM` caller, so mark it `[[gnu::always_inline]]`. Check placement with `arm-none-eabi-nm -C daisy/build/hothouse-xfade/ducky_xfade.elf`.
- **MPU** (`firmware.h`):
  - Region 0 is a no-access background over `0x60000000–0xDFFFFFFF`, which covers all of QSPI. This avoids the ES0392 2.7.5 stall on speculative reads of `0x907FFFFF`.
  - Regions 1–3 are libDaisy's: D2 DMA, SDRAM and backup SRAM.
  - Region 4 is the preset store and the settings record, `0x90400000–0x904FFFFF` (1 MB): privileged read-only, write-through cached, execute-never; Bad Tape's region 4, moved and shrunk.
  - Region 5 is the firmware image, `0x90040000–0x900BFFFF` (128 KB subregions 2–5 of the first 1 MB), with region 4's attributes: FIRMWARE COMMIT reads its write back through it. The rest of QSPI, `0x907FFFFF` included, stays no-access.

## Preset store

`daisy/src/preset.h` is the reference and the contract with `editor/src/preset/bank.ts` (and `morph.ts` for the morph block and the waypoints). Tests: `tests/preset_test.cpp`, `editor/src/preset/bank.test.ts`; both check the committed `tests/fixtures/preset/A2_decay.bin`.

| QSPI | Size | Holds |
|---|---|---|
| `0x90400000 + k × 0x10000` | 64 KB each (one erase block) | Slot k (0–7; slot k+1 on the pedal and in the logs) |
| `0x90480000`, `0x90481000` | 4 KB each (one erase sector) | State records A and B |
| `0x90482000`, `0x90483000` | 4 KB each (one erase sector) | Settings records A and B ("Settings record") |

The Daisy bootloader's DFU layout lists `0x90400000/60*64Kg`, so dfu-util writes the bank there. The pedal itself writes the state and settings sectors, and single slots on the link's commands ("Slot writes").

**Slot (format 3),** little-endian; erased flash (0xFF) follows it. A format-2 slot (from before the noise gate) or a format-1 slot (from before the model binary's gear texts) parses as `version`; upload it again:

| Offset | Field |
|---|---|
| 0 | magic `DKPS` |
| 4 | u16 version (3) |
| 6 | u16 flags: bit 0 has IR, bit 1 has a morph block, bits 2–3 K, the morph's waypoint count (0–3; 0 without a morph block); other bits 0 |
| 8 | u32 size: 7,996, or 16,288 with an IR; 64 more with a morph block, and 7,480 more for each waypoint (38,792 at most) |
| 12 | name[64], NUL-terminated printable ASCII |
| 76 | gate block (8 bytes): u8 flags (bit 0 enabled), u8 threshold (dB below full scale), u8 attack (0.1 ms), u8 hold (ms), u16 release (ms), u8 ratio (tenths), u8 0; `docs/FILTERS.md` "Noise gate" |
| 84 | model binary (7,908 bytes, `docs/NAM_FORMAT.md`) |
| 7992 | IR binary (8,292 bytes, `docs/FILTERS.md`), only with the IR flag |
| 7992, or 16284 with an IR | morph block (64 bytes, `docs/MORPH.md` "Formats"), only with the morph flag |
| after the morph block | K waypoints, 7,480 bytes each: 1,870 f32, a network's weight stream in the model binary's weight order with the head scale folded into the head (head scale 1), in the preset's own model's coordinates |
| size − 4 | CRC-32 of bytes [0, size − 4) |

`dkpreset::Parse` checks, in order: `empty` (the first 4 bytes are 0xFF), `magic`, `version` (the version, an unknown flag, or a waypoint count without the morph flag), `size` (the size field is not the one its flags give), `crc`, then the embedded binaries with their own parsers (`model`, `ir`), the morph block (`morph`: a target slot, a permutation, scales in range, an invertible matrix) and the waypoints (`morph`: every float finite). A slot that does not parse is skipped. The gate block is not checked: each field is clamped into its range (`GetGate`), since the CRC already guards the bytes. The bank tool's presets carry the gate off at its defaults and no morph block. The link's MORPH sets or removes the block of a stored preset and removes its waypoints (`SetMorph`); an upload may carry a morph block and waypoints, and is refused when its block targets the slot it is uploaded to (`docs/MIDI.md`).

**Morph target:** a preset with a morph block morphs with the slot `ResolveMorph` finds: the block's target while that slot is ok, is not the preset's own and holds the model whose CRC the block stores; else the first other ok slot holding that model; else none (`docs/MORPH.md`).

**Morph path** (the weight build; the xfade build keeps the block and the waypoints as stored and does not use them): the waypoints make the morph a path of K + 1 straight segments of equal length over the blend position: from the preset's model to the first waypoint, from waypoint to waypoint in stored order, and from the last to the target's model in the preset's coordinates (the block's transform). `Parse` copies them into the parsed preset in SDRAM, and `dkmorph::Prepare` lays each segment's start and step out for the engine (`gMorphW`) at the preset's load; the callback finds the position's segment (`SegmentAt`) and blends that one, so a path costs a block what a straight blend does. Without waypoints the path is the one straight segment.

**State record:** 16 bytes at the start of each state sector: `DKST`, u32 seq, u32 slot (0–7), and the CRC-32 of the first 12 bytes. The valid record with the higher seq is the newest. A write erases and writes the other sector, then reads it back through the memory-mapped QSPI (`state slot N seq S T ms`, or `state write failed`), so a torn write leaves the previous record readable. At boot the pedal plays the saved slot if it parsed, else the first slot that did, else dry.

**Bank tool:** `bun tools/preset_bank.ts -o OUT.bin --slot N MODEL [--ir IR] [--name NAME] …` builds the 512 KB bank (`BANK_BYTES`), slot N (1–8) at `(N − 1) × 64 KB`, every byte not in a preset 0xFF.
- A `.nam` model is converted as `tools/nam_convert.ts` does; any other path is read as a model binary. A `.wav` IR is converted as `tools/ir_convert.ts` does; any other path is read as an IR binary.
- The name defaults to the model's.
- `--ir` and `--name` apply to the last `--slot`. A `.nam` whose gear type contains `cab` with an IR gets a warning.
- Errors write nothing: usage errors exit 2, and rejected files exit 1.
- The bank replaces all 8 slots. dfu-util erases the whole range before writing it, so power pulled mid-download leaves every slot empty (or the first few written): the pedal boots, plays dry or what is left, and the bank can be flashed again. The firmware at `0x90040000` and the state sectors are outside the range.

**Switching cost:** the load is `A2Lite::Load` (132–141 µs), plus `IrConvolver::Load` for a preset with an IR (about 835 µs: the load with a 2,048-tap IR measured 975 µs), plus, for one with a morph target, a second `A2Lite::Load` (with an IR, 1,115 µs in all) or, in the weight build, `dkmorph::Prepare` (about 870 µs for a straight blend: 1,845 µs in all with an IR; about 1,200 µs with three waypoints, measured before the two-level convolver). Figures from `docs/MEASUREMENTS.md` "Preset store and switching". The audible gap is the 5 ms fade out, the load, and the 5 ms fade in.

**Slot writes:** UPLOAD COMMIT, ERASE, RENAME, SWAP, GATE and MORPH (`docs/MIDI.md`) go through `writeSlot` (`writeFlash` over the slot) in the main loop: erase the slot's 64 KB block, write the image, invalidate the D-cache over the slot, and compare it through the memory-mapped QSPI (an erase checks for 0xFF throughout), then re-parse the slot (`rescan`). The state and settings records and FIRMWARE COMMIT use the same `writeFlash`. COMMIT parses the staged image before it touches flash, so an invalid upload never erases a slot. RENAME, GATE, MORPH and SWAP copy the slot images into SDRAM scratch first (a rename, a gate change and a swap keep the morph block and the waypoints); SWAP writes a, then b. The audio callback reads no QSPI, so audio runs through every write; the main loop blocks for it (132–204 ms per slot, `docs/MEASUREMENTS.md` "Link and slot writes"; 180–185 ms for a 30,500-byte preset with three waypoints).
- **libDaisy quirk:** `QSPIHandle::Erase` over a whole 64 KB block uses the block-erase command and leaves the QSPI in indirect mode, where the next read of the mapped region stopped the pedal (the first ERASE on hardware never answered). `Write` and `EraseSector` return to memory-mapped mode, so an erase without data ends with an `EraseSector` of the slot's first 4 KB.
- **Playback after a slot change:** an upload into the target slot sets `gReload`, so it is reloaded through the usual switch (one fade); an upload while dry plays the new slot; a target slot that stops parsing (erase, failed write) moves to the next filled slot, with LED2's blinks, or to dry, which turns LED2 off. SWAP remaps the playing and target slot numbers without a switch, and the state record follows 2 s later. GATE on the playing slot hands the new gate to the callback without a reload. MORPH on the playing slot reloads it, and so does any slot change after which the playing preset's morph block resolves to another slot than the one it was loaded with (`servePresets`: an upload over, an erase or a swap of its target). `docs/MIDI.md` "Slot changes and playback" is the host-facing summary.

## Settings record

`daisy/src/settings.h` is the reference; tests: `tests/settings_test.cpp`. The record holds what is global to the pedal and not part of a preset: which parameter each control slot (Knobs 3–6, SW1–SW3, EXP, PEDAL) drives, the MIDI channels EXP's CC 11 and the preset CC 102 are taken from on USB and on a BurningBungalow's MIDI IN, and every parameter's value (`docs/MIDI.md` "Parameters", "Controls").

| Offset | Field |
|---|---|
| 0 | magic `DKSG` |
| 4 | u32 seq |
| 8 | u8 version (4) |
| 9 | u8 control slots (9) |
| 10 | u8 parameter count N |
| 11 | u8 expression channel (USB): 0 omni, 1–16 |
| 12 | u8 MIDI IN channel (a BurningBungalow's): 0 omni, 1–16 |
| 13 | assign[9]: a parameter id per control slot, or `7F` for none |
| 22 | f32 value[N], by parameter id: a continuous parameter's position 0..1 along its taper, a choice's index |
| 22 + 4N | CRC-32 of bytes [0, 22 + 4N) |

- **Two copies,** as for the state record: each of the two 4 KB sectors holds one record at its start. The valid record with the higher seq is the newest. SAVE (the link's command) erases and writes the other sector, then reads it back through the memory-mapped QSPI (`settings seq S T ms`, or `settings write failed` and RESULT `flash`), so a torn write leaves the previous record readable. Nothing else writes it: ASSIGN and SET change only the running pedal.
- **At boot** the newest record sets the assignments and the values (`settings seq S`); with none, the controls take the factory layout (`kFactoryAssign`, `docs/MIDI.md` "Controls") and every parameter is at its default (`settings factory`). The first audio block then writes every parameter on a control from that control, so a saved value matters only for a parameter on no control.
- **Decoding is defensive:** a stored value is clamped into its range, a choice rounded to an index, and a non-finite value replaced by the default; an assignment its slot does not accept (an unknown id, a continuous parameter on a switch) becomes none.
- **Across firmware versions:** parameter ids are append-only within a version. A record with fewer parameters than the firmware leaves the later ones at their defaults; one with more has the extra values ignored. SAVE always writes the running firmware's count. A table whose ids change bumps the version, and a record of another version is ignored, so the first boot after such a change reads `settings factory`. Version 3 merged `comp_enable` (id 16) and `comp_mode` (id 19) into `comp_mode` (id 16: off, blend, direct, one toggle's three positions), moving `comp_balance` and the loop's three down one (23 parameters). Earlier versions: `docs/HISTORY.md`.
- **Morph:** `morph` (id 23) is appended, 24 parameters; the version stays 3, so a 23-value record leaves it at its default, 0 %.
- **Compressor type:** `comp_type` (id 24) is appended, 25 parameters; the version stays 3, so a 24-value record leaves it at its default, OTC.
- **EXP:** the eighth control slot (the expression pedal over MIDI, CC 11) took version 3's spare header bytes: byte 11 its channel, byte 19 its assignment, byte 9 reading 8; a 7-slot version-3 record still decodes, with EXP at the factory layout's (none) and the channel omni. A channel past 16 decodes as omni.
- **PEDAL and MIDI IN (version 4):** the ninth control slot (a BurningBungalow's expression pedal jack) and MIDI IN's channel grow the header to 22 bytes. A version-3 record (7 or 8 slots: magic, seq, version, slots, N, the expression channel at 11, assign[slots] from 12, values from 20) still decodes, with PEDAL at the factory layout's (none) and MIDI IN omni; the first SAVE after the update writes version 4. The same record serves both boards: a HotHouse drops a saved PEDAL assignment at boot (`main`), so its SAVE writes PEDAL as none, and keeps MIDI IN's channel as it read it.

## Link protocol

`daisy/src/link.h` is the reference: the command and result tables (`kCommands`, `kResults`) generate `docs/MIDI.md` (`make midi-doc`; `make test` fails when it is stale), which documents the framing, every command, the parameter table, the controls, the upload and the playback rules. Frames are `F0 7D 64 01 <cmd> … F7` (device byte `0x64`, version `0x01`). Every request except HELLO and DFU gets exactly one RESULT, last. The commands, by what they serve:

- **Boot and log:** HELLO, DFU, LOG and Identity.
- **Firmware:** FIRMWARE BEGIN/CHUNK/COMMIT (`0x1B`–`0x1D`): a new image staged in SDRAM, checked, written to `0x90040000`, then a restart into it ("Firmware update").
- **Slots:** LIST, ERASE, RENAME, SWAP and UPLOAD BEGIN/CHUNK/COMMIT, with the SLOT and RESULT replies; READ and its DATA reply (up to 160 bytes of an ok slot's image, copied from the mapped QSPI and sent by `serveReplies` before READ's RESULT).
- **Status:** WATCH and the STATUS reply (34 bytes), which carries the gate's level and cut, the comp cut (the largest reduction since the previous STATUS of the OTC's low-frequency gain below Sustain's, or of the OTA's bias current below its value at rest, tenths of a dB, 0 when off) and a BurningBungalow's expression jack position (permille of travel, 0 on a HotHouse).
- **Noise gate:** GATE (rewrite a slot's gate block) and GATE TRY (set the running gate only, for the editor's sliders).
- **Parameters:** DUMP (the parameter table as PARAMs, then a VALUES), SET, ASSIGN, SAVE, and a VALUES after every STATUS under WATCH.
- **Morph:** MORPH (`0x19`: rewrite or remove a slot's morph block, `docs/MORPH.md`), the `morph` parameter (id 23) and the slot status `morph`.
- **Expression pedal and presets:** EXP CHANNEL (`0x1A`), control 7 (`exp`) in ASSIGN, the channel and an eighth assignment in VALUES (whose held bits are a u14), and two MIDI messages besides SysEx, both on EXP CHANNEL's channel: Control Change 11 on the Seed's USB MIDI port moves EXP, and Control Change 102 with value 0–7 switches to slot 1–8 (`daisy/src/midi.h` `CcParser` takes every byte `dkusb::Serve` reads, alongside the SysEx assembler; `onUsbControlChange` queues EXP's position for the callback, or retargets the preset switch as FS2 does).
- **BurningBungalow:** control 8 (`pedal`, PEDAL, the expression pedal jack) in ASSIGN and a ninth assignment in VALUES; MIDI IN CHANNEL (`0x1E`) and its channel in VALUES. The TRS MIDI IN takes the same two Control Changes on MIDI IN CHANNEL's channel (`daisy/src/midi_in.cpp`: libDaisy's `MidiUartTransport` on USART1 RX D14 at 31250 baud, its DMA bytes through a ring to `dkmidiin::Serve` in the main loop, after `dkusb::Serve`; a UART error restarts reception). On a HotHouse, ASSIGN of control 8 and MIDI IN CHANNEL answer `bad-request`.

The Identity Reply's first version byte carries flags: bit 0 set by the weight build (`APP=weight`), clear in the xfade build. Firmware from before it sends 0, so a host reads it as the crossfade.

- **USB enumeration:** about 1 boot in 6 after a DFU flash, the port showed up as `Љ MIDI 1`. ST's HAL leaves an aborted control transfer's unsent IN packet in EP0's TxFIFO, so after a re-sent SETUP every control read returns the reply before its own. `__wrap_USBD_LL_SetupStage` in `daisy/src/usb_link.cpp` flushes TxFIFO 0 when a SETUP finds an IN packet pending, linked with `-Wl,--wrap` so libDaisy stays unpatched; the full trace is in its comment.

**CLI:** `bun tools/pedal.ts [--port BUS-PORT] COMMAND`, slots 1–8:

```
list                                     # one line per slot: N STATUS "NAME" model "M" ir "I"|none, * on the playing one
upload N MODEL [--ir IR] [--name NAME]   # MODEL/IR as preset_bank.ts reads them; prints flash ms, retries, seconds
erase N | rename N NAME | swap A B       # each prints the flash time
gate N on|off [THRESHOLD_DB ATTACK_MS HOLD_MS RELEASE_MS RATIO]  # e.g. gate 1 on -55 0.5 20 100 4; on|off alone keeps the stored settings
morph N M|none                           # slot N morphs with slot M: READs both models, analyses (docs/MORPH.md), MORPH; prints the fit and the flash time
info N                                   # READs the preset's metadata: name; model, gear type/make/model, modeled by; loudness, input level; IR taps, source, gain; gate; morph target and fit
param list | param get NAME              # each parameter: its name, value and controls (`low_cut_freq    106 Hz        K5`), ` (held)` on a control waiting to move
param set NAME VALUE                     # VALUE in the parameter's unit (Hz, Q, dB; comp_sustain in % while comp_type is OTA), a choice's label or index, or `off`: `param set high_cut_freq 3000`, `param set bell2_gain -3`
param assign CONTROL NAME|none           # CONTROL k3-k6, sw1-sw3, exp or pedal (a BurningBungalow's): `param assign k3 bell2_freq`; a switch takes only choice parameters
param channel omni|1-16                  # EXP CHANNEL: the MIDI channel exp's CC 11 and the preset CC 102 are taken from on USB (omni: any)
param midi-in omni|1-16                  # MIDI IN CHANNEL: the same for a BurningBungalow's MIDI IN jack
param save                               # SAVE: the assignments, the expression and MIDI IN channels and the values for the next power-up; prints the flash time
status [SECONDS]                         # WATCH: a STATUS line every 100 ms (with `gate in … dBFS cut … dB`, ending `comp cut … dB`) and the LOG lines, for SECONDS (5) or until Ctrl-C
firmware FILE.bin                        # FIRMWARE: writes the image and restarts the pedal into it; prints the flash time, retries and seconds until it is back, with its build and morph
```

- Usage errors exit 2; a refused command (`pedal: RENAME: slot-state`), a rejected file or no reply exits 1.
- The upload resends a CHUNK on no reply (1 s) or `crc` and gives up after 5 tries of one chunk; COMMIT is not resent (`UPLOAD COMMIT: no reply; check with list`).
- Transport: the ALSA rawmidi device of the Seed's sound card (`/dev/snd/midiC<N>D0`, found through `/sys/class/sound/card*/device` with `0483:5740`), opened exclusively and nonblocking; input pending at open (stale replies) is dropped. The protocol code (`editor/src/link/`) is plain TypeScript, shared with the editor, which supplies a Web MIDI transport instead (`editor/src/midi.ts`).
- `pedal.ts` and `usb_log.py` cannot run together ("Port conflicts").
- Unknown commands with this header still log `link: unknown command CC`. Other devices' frames are ignored.

## Firmware update

A running Ducky takes a new image over its link: `bun tools/pedal.ts firmware FILE.bin`, or the editor's **Firmware…** button (`docs/EDITOR.md`). The steps, the image's tag and the checks are in `docs/MIDI.md` "Firmware update"; `daisy/src/fw_image.h` is the reference.

- **Why it is safe to rewrite the running image:** the image is `BOOT_SRAM`. The Daisy bootloader copies it from QSPI `0x90040000` into AXI SRAM at boot, so the running code never executes from the blocks COMMIT erases. Until the restart the old image runs; a failed write (`flash`) leaves it running, and COMMIT can be resent.
- **The tag:** a 16-byte `dkfwimg::Tag` at offset `0x400` of the `.bin` (`.ducky_image` in the linker script, which asserts the vector table ends before it): `DKFW`, version, board, the Identity flags. Only `ducky.cpp` defines it, so the xfade and weight builds carry it and the bench and latency images do not. Every tagged image serves FIRMWARE, so an update never installs an image that cannot be updated the same way. Images built before the tag existed are refused: such a pedal takes its first tagged image over DFU (`flash_when_ready.sh`).
- **Checks:** the host refuses a file of the wrong size, without an SRAM vector table or without the tag (`editor/src/link/firmware.ts`); the pedal checks the whole image's CRC (BEGIN's), the vector table, the tag and the board before it erases anything, and answers `invalid` with the failed check.
- **Write:** the 64 KB blocks the image covers are erased, the image written and read back through MPU region 5 ("Memory"), 0.70–0.72 s for a 198 KB image on the Seed 3 (`docs/MEASUREMENTS.md` "Firmware update"); audio keeps running, the main loop blocks.
- **Restart:** after COMMIT's RESULT has gone out, the main loop writes a pending state record, logs `firmware restart`, sends for ~100 ms and resets into the bootloader with `DAISY_SKIP_TIMEOUT`, which loads the new image without its DFU wait. Presets, the state record and the settings record are untouched; the pedal comes back on USB with the new Identity (a whole `pedal.ts firmware` run, transfer to Identity, takes 3.7–4.2 s), and the editor re-syncs on its boot line.
- **Check on hardware:** `DK_USB_PORT=3-4 make firmware-check` (`tools/firmware_check.ts`), unattended: it builds the xfade, weight and bench images and the editor, flashes the xfade build over DFU, updates weight → xfade twice with `pedal.ts`, checks the host's and the pedal's refusals, then in Chrome (headless, through `tools/cdp.ts`) uploads a `.nam` + `.wav` preset to the highest empty slot and updates weight → xfade, and finally erases that slot and compares `pedal.ts list` with the first. It needs the pedal's MIDI port free and an empty slot; when a self-written image does not come back it reflashes over DFU if the bootloader waits in DFU, else says how to recover by hand.

## Log lines

```
ducky hothouse seed 1.2-pcm3060 sysclk 480 MHz audio 48000 Hz 24 bit block 24 06ea416        (boot / HELLO)
slots ok ok empty ok empty empty empty empty                                                  (boot / HELLO)
preset 1 "FNDR BFSR VB Edge WRM2 CAB FREE"                                                    (boot / HELLO)
model loudness -15.9 dB level -2.1 dB input +15.0 dBu cal -10.5 dB "FNDR BFSR VB Edge WRM2 CAB FREE"  (boot / HELLO)
ir taps 2048 src 48000 Hz gain -15.1 dB "YA FDMN 412 BLND Mix 01"                             (boot / HELLO)
gate on thr -55 dBFS atk 0.5 ms hold 20 ms rel 100 ms ratio 4.0                              (boot / HELLO, after a switch)
morph slot 2 xfade "FNDR BFSR VB Edge WRM2 CAB FREE"                                         (boot / HELLO, after a switch)
settings seq 5                                                                                (boot / HELLO)
layout k3 bell2_gain k4 low_cut_freq k5 high_cut_freq k6 comp_balance sw1 low_cut_slope sw2 comp_mode sw3 comp_level  (boot / HELLO)
exp none channel omni                                                                         (boot / HELLO)
pedal none midi in channel omni                                                               (boot / HELLO, a BurningBungalow only)
loop miso                                                                                     (boot / HELLO, when loop_mode changes)
preset 4 "A2" load 138 us                                                                     (after a switch, with model and ir)
state slot 4 seq 12 85 ms                                                                     (2 s after a switch)
upload slot 8 "A2" 167 ms                                                                     (link: UPLOAD COMMIT, then the slots line)
erase slot 1 215 ms                                                                           (link: ERASE, then the slots line)
rename slot 2 "Renamed" 156 ms                                                                (link: RENAME, then the slots line)
gate slot 1 on 147 ms                                                                         (link: GATE, then the slots line)
morph slot 5 -> 2 161 ms                                                                      (link: MORPH, then the slots line)
swap slots 1 8 314 ms                                                                         (link: SWAP, then the slots line)
assign k3 low_cut_slope                                                                       (link: ASSIGN)
exp channel 2                                                                                 (link: EXP CHANNEL)
midi in channel 2                                                                             (link: MIDI IN CHANNEL)
settings seq 6 81 ms                                                                          (link: SAVE)
firmware written 198232 bytes 747 ms                                                          (link: FIRMWARE COMMIT)
firmware restart                                                                              (after FIRMWARE COMMIT's RESULT)
ctl k 31 22 33 58 25 34 sw 2 0 1 fs 0 0 exp 64 pedal 41                                       (every second)
load avg 361 max 363 permille underruns 0 mute off in -84 dBFS out -31 dBFS rin -110 dBFS rout -31 dBFS  (every second)
```

- **`slots` line:** each slot's `dkpreset::StatusName`, slots 1–8.
- **`preset` line:** the playing slot (1-based) and its name, with the load time in µs after a switch; `preset none` when dry.
- **`model` line:** the preset's model: `loudness` and `input_level_dbu` (`none` without them), the level-match and input-calibration gains, in tenths of a dB, then its name, last because log lines stop at 120 characters. `model none` when dry.
- **`ir` line:** the preset's IR: tap count, source rate, the converter's normalisation gain (reference band at 0 dB, `docs/FILTERS.md` "Converter"), and its name. `ir none` without an IR or when dry. `ir fft` alone, every second, when the convolver's FFT does not initialise.
- **`gate` line:** the preset's noise gate (`docs/FILTERS.md` "Noise gate"): threshold, attack, hold, release and ratio; `gate off` when it is off or dry.
- **`morph` line:** the slot the preset morphs with (1-based) and that preset's name, `morph slot 2 xfade "FNDR BFSR VB Edge WRM2 CAB FREE"`; `morph missing slot N` when no slot holds the model the block was analysed with (N, 1-based: the block's target); `morph none` without a block or when dry. `morph shed` when an overrun stopped the second engine ("Threads").
- **The weight build** (`APP=weight`): the boot line ends ` weight`, after the build id; the `morph` line for a target reads `morph slot 2 fit 21 21 % waypoints 0 "FNDR BFSR VB Edge WRM2 CAB FREE"`: its block's fit and unaligned fit in percent (`docs/MORPH.md`) and its path's waypoint count (0: a straight blend) before the name; no `morph shed`.
- **`settings` line:** at boot and on HELLO, the seq of the settings record the pedal started from, or `settings factory` with none; after SAVE, the seq written and the time the erase, write and readback took; `settings write failed` when the readback does not match.
- **`layout` line:** the live assignments: each knob and switch slot (`k3`–`k6`, `sw1`–`sw3`) and its parameter's name, or `none`. One parameter per control fits the 120 characters; a layout with long names repeated on several controls is cut.
- **`exp` line:** after the `layout` line, EXP's parameter (or `none`) and the channel its CC 11 (and the preset CC 102) is taken from on USB (`omni` or 1–16). EXP CHANNEL logs `exp channel omni|N`.
- **`pedal` line:** a BurningBungalow only, after the `exp` line: PEDAL's parameter (or `none`) and MIDI IN's channel (`omni` or 1–16). MIDI IN CHANNEL logs `midi in channel omni|N`.
- **`loop` line:** `loop_mode` as published: `miso`, `pre-model` or `post-model` (`docs/LOOP.md`). Logged at boot and on HELLO after the `exp` (or `pedal`) line, and by the main loop whenever the value changes, whichever control or SET changed it.
- **`assign` line:** the control and the parameter (or `none`) an ASSIGN put on it. SET logs nothing.
- **`state` line:** the slot and seq written to the state record and the time the erase and write took; `state write failed` when the readback does not match.
- **`preset cc` line:** a CC 102 that selects nothing: `preset cc V ignored` for a value of 8 or more, `preset cc V ignored: slot N STATUS` for a slot that is not ok. One that selects a slot logs the switch's `preset` line.
- **Slot change lines** (`upload slot`, `erase slot`, `rename slot`, `swap slots`, `gate slot`, `morph slot`): the slot(s), 1-based, the new name, the gate's on/off or the morph's target (`-> M`, or `off`), and the time the flash erase, write and readback took; then the `slots` line. `slot write failed N` when a write or its readback failed (the command answers `flash`). GATE TRY logs nothing.
- **`firmware` lines:** `firmware written N bytes T ms` after FIRMWARE COMMIT's erase, write and readback, then `firmware restart` as the pedal resets into the new image; `firmware rejected CHECK` (`crc`, `vectors`, `tag`, `board`) when the image fails a check; `firmware write failed` when the write or its readback failed (the command answers `flash`).
- **`ctl` line:**
  - `k`: knobs 1–6, in percent.
  - `sw`: SW1–SW3 after the 30 ms filter, as 2 up, 1 middle, 0 down.
  - `fs`: FS1 and FS2 held.
  - `exp`: the last CC 11 value taken (0–127, from USB or MIDI IN), `-` before the first since boot.
  - `pedal`: a BurningBungalow's expression jack, in percent of its travel (`controls.h` `PedalTravel`); `-` on a HotHouse.
- **`load` line:**
  - `avg` and `max`: libDaisy's `CpuLoadMeter`, in permille of the block period, measured over the last second.
  - `underruns`: the count since boot.
  - `mute`: FS1's state, which is not persisted.
  - `in`: the left-input peak over the last second, floored at −120 dBFS.
  - `out`: the left-output peak over the last second, after the mute, floored at −120 dBFS. It does not reach −120 with no signal: the model has a DC offset at zero input (`A2.bin`: −60 dBFS, before the output gain).
  - `rin`, `rout`: the right input's and the right output's peaks the same way: the loop's return and send in a loop mode, the copy of `out` and an unused input in MISO.

## Bench

`APP=bench` (`daisy/src/bench.cpp`) has no audio. Its `chain` is MISO's: the loop modes add only two `GainRamp`s and a two-buffer fade, so their cost is measured live (`docs/MEASUREMENTS.md` "FX loop"). It compiles in `tests/fixtures/nam/A2.bin`, `tests/fixtures/ir/decay.bin` and a slice of `input.wav` and `A2.input.ref.wav` (`.incbin`; the Makefile rebuilds `bench.o` when they change). It places the engine like the pedal does: the object and its rings in DTCM, its stages and the timed blocks in ITCM; the EQ, both compressors and the pedal's convolver (`gIrAxi`) in DTCM with the convolver's tail spectra in AXI SRAM (`gIrPoolAxi`; the stage keeps the name `ir axi`), and a second convolver wholly in D2 SRAM (`gIrD2`, `gIrPoolD2`) for comparison. The EQ and the OTC read one array of parameter values, `gValues`, where `eqValues` and `compValues` each write their own ids; the OTA reads its own, `gOtaValues` (`otaValues`), since its `comp_type` differs. Each pass:

1. **Check:** a reset engine runs `input.wav[48000, 72000)` (the file's first second is silence, so a reset engine is already in that state) against the reference render; limit 1e-5.
2. **IR check:** the reset convolver runs `input.wav[48000, 52800)` against a direct convolution computed on the device in double; limit 1e-5.
3. **Model:** `A2Lite::Process` on `kBlockSize` blocks of uniform noise (±0.5), timed with the DWT cycle counter: 200 ms of warm-up blocks, then 2 s measured, interrupts masked inside each block. The model's gate is the maximum against 450 permille of the block period.
4. **Layers:** the same blocks, each stage (`BeginChunk`, `RunLayer` per layer, `EndChunk`) timed separately.
5. **IR, EQ, noise gate, compressors, chain:** the same way, the convolver wholly in D2 SRAM and placed as the pedal's; the EQ at its worst case: enabled, both cuts at mid-travel with the top resonance and their slopes flipping between 24 and 12 dB every 10 blocks (slope fades), every bell flipping between −12 dB at a quarter and +12 dB at three quarters of its frequency range every 10 blocks, so every glide and coefficient ramp runs on every block; the noise gate enabled with the longest hold (100 ms, the detector's largest window), fed each block's input peak; the compressor (`comp`) on in blend, Sustain 0.25 ↔ 1.0, Level −3 ↔ +3 dB and Balance 0.25 ↔ 0.75 flipping every 10 blocks, so every smoother, ramp and `powf` runs on every block, while the noise (0.92 V at the jack) keeps the side chain compressing and clipping; the OTA compressor (`comp ota`) the same on `comp_type` OTA, from its own values array; and the pedal's whole chain (the gate, both compressors fully on in series as during a `comp_type` switch, input gain, engine, IR, EQ, output gain with the gate's gain, mute). The chain gate is its maximum against 850 permille, the load ceiling (`docs/ROADMAP.md` "Decisions").
6. **Morph:** `A2Lite::Blend` on every block, at positions 0 and 1 in turn, with the two `powf` of the pedal's morph step: a knob that never rests. Both weight sets are the model's own (`Prepare` with the identity and no waypoints), so the engine goes on computing it; they are the one segment a blend reads, in AXI SRAM as on the pedal (the pedal's `gMorphW` holds four, which the bench's SRAM has no room for). The chain runs the same step before the compressor. After the chain the engine is loaded again: a blend leaves the head scale folded into the head, and the next pass's check wants the model as loaded.

Passes repeat every 2 s; the DFU command works between stages, so `flash_when_ready.sh` returns to the pedal with no button presses.

A pass's log (the figures over many passes are in `docs/MEASUREMENTS.md` "Bench"):

```
ducky_bench hothouse sysclk 480 MHz 8663115+dirty                                 (boot / HELLO)
bench model "A2" ok
bench ir "decay" ok
bench pass 2 budget 240000 cycles
bench check samples 24000 max err 241e-9 limit 10000e-9 ok
bench ir check samples 4800 max err 178e-9 limit 10000e-9 ok
bench a2lite cycles avg 81302 max 82020 permille avg 338.7 max 341.7
bench gate a2lite max 341.7 permille limit 450 go
bench input cycles avg 551 max 565 permille avg 2.2 max 2.3
bench layer 00 k6 d1 cycles avg 3239 max 3306 permille avg 13.4 max 13.7         (… layer 22)
bench head cycles avg 2322 max 2328 permille avg 9.6 max 9.7
bench ir d2 cycles avg 22102 max 25745 permille avg 92.0 max 107.2
bench ir axi cycles avg 18456 max 19636 permille avg 76.9 max 81.8
bench eq cycles avg 14163 max 14224 permille avg 59.0 max 59.2
bench noise gate cycles avg 235 max 308 permille avg 0.9 max 1.2
bench comp cycles avg 7105 max 7380 permille avg 29.6 max 30.7
bench comp ota cycles avg 7540 max 7802 permille avg 31.4 max 32.5
bench morph cycles avg 7588 max 7703 permille avg 31.6 max 32.0
bench chain cycles avg 140914 max 142625 permille avg 587.1 max 594.2
bench gate chain max 594.2 permille limit 850 go
bench pass 2 done
```

Put a new run's figures in `docs/MEASUREMENTS.md` "Bench", in place of the ones they replace.

How the engine got here from 570 permille, the measured cost model behind it, and how to check an image for a regression: `docs/M7_PERFORMANCE.md`. Run the bench after any change to the engine, the compiler or its flags: the gate line is the only test that sees a slowdown.
