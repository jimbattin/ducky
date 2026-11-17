# Firmware

The pedal firmware in `daisy/`, plus the host tests and tools that go with it. Every command runs from the repo root.

## Setup (once per clone)

```sh
git submodule update --init --recursive   # libDaisy v9.0.0 and its driver submodules (HAL, CMSIS_5, CMSIS-DSP, USB device)
make -C daisy/libDaisy                    # daisy/libDaisy/build/libdaisy.a
uv sync                                   # .venv with pyusb for tools/usb_log.py
```

This needs `arm-none-eabi-gcc` (tested with 13.3.rel1), `dfu-util`, `uv`, `g++`, GoogleTest visible to `pkg-config gtest_main`, and Bun (tested with 1.4.4) for `editor/` and the `tools/*.ts` tools. `make -C editor` installs the editor's dependencies on first use. `tools/pedal.ts` needs Linux with ALSA and read/write access to `/dev/snd/midiC*D0` (group `audio`, or the seat ACL a desktop session grants).

## Build, test, flash, log

```sh
make test                        # host GoogleTest: build/host/tests (tests/*.cpp); also builds build/host/render and fails when docs/MIDI.md is stale
make midi-doc                    # regenerates docs/MIDI.md from daisy/src/link.h
make -C daisy -j                 # daisy/build/hothouse/ducky.bin; the link prints the memory-usage table
make -C daisy APP=bench -j       # daisy/build/hothouse-bench/ducky_bench.bin ("Bench" below)
make -C daisy APP=latency -j     # daisy/build/hothouse-latency/ducky_latency.bin: the FX loop's round trip (docs/LOOP.md)
tools/flash_when_ready.sh [FILE.bin [ADDR]]   # sends DFU over USB-MIDI, waits for 0483:df11, flashes ADDR (default 0x90040000)
bun tools/preset_bank.ts -o build/bank.bin --slot 1 MODEL.nam --ir IR.wav --slot 2 …   # preset bank ("Preset store")
tools/flash_when_ready.sh build/bank.bin 0x90400000                                   # writes all 8 slots
uv run tools/usb_log.py 10 --identity   # 10 s of log, preceded by the Identity Reply
bun tools/pedal.ts list                 # the pedal over the link while it plays: list, info, upload, erase, rename, swap, gate, param, status ("Link protocol")
make -C editor test check        # editor and converter tests (bun:test), tsc, Biome
make -C editor serve             # the editor with hot reload at http://localhost:3000/ (Chrome; allow MIDI with SysEx)
make -C editor html              # editor/dist/editor.html: the editor as one self-contained file
bun tools/nam_convert.ts models/*.nam   # .nam -> .bin next to each input (docs/NAM_FORMAT.md)
bun tools/ir_convert.ts IR.wav...       # .wav -> IR .bin next to each input (docs/FILTERS.md)
tools/nam_reference.sh           # NAM reference renders (docs/NAM_FORMAT.md "Reference renders")
```

- **Build options:** `daisy/Makefile` takes `BOARD=hothouse` (the only board) and `APP=ducky` (default), `APP=bench` or `APP=latency`. The build directory is `daisy/build/<BOARD>`, or `daisy/build/<BOARD>-bench` and `daisy/build/<BOARD>-latency` for the other two.
- **Presets** are not part of the build: they live in QSPI and are written with a bank image, or slot by slot over the link with `tools/pedal.ts` ("Preset store"). An empty bank plays dry. `MODEL=`/`IR=` (M4/M5) are gone.
- **CMSIS-DSP:** `libdaisy.a` has no CMSIS-DSP, so every build compiles the FFT sources `daisy/cmsis_fft.mk` lists, with its table defines.
- **Optimisation flags:** `-O3 -fno-schedule-insns -fno-schedule-insns2`. With GCC's instruction scheduling off, the order of the DSP's statements is the order of its instructions, which the engine relies on (`docs/M7_PERFORMANCE.md`).
- **Build id:** the build writes `build_id.c` with the short commit hash, plus `+dirty` when tracked files differ. Untracked files do not make it dirty.
- **Flashing:** `flash_when_ready.sh [FILE.bin [ADDR]]` needs no button presses while Ducky is running. `ADDR` is `0x90040000` (firmware, the default) or `0x90400000` (a preset bank); anything else is refused. Both end with `:leave`, so the pedal restarts into Ducky. If the Seed runs other firmware, either send that firmware's own DFU command or hold BOOT, tap RESET, release BOOT. Then run the script.
- **Several Seeds attached:** every Seed enumerates as `0483:5740`. Pick Ducky's by its USB path (`BUS-PORT`, as in `/sys/bus/usb/devices` and `dfu-util -p`; the physical port, so it survives the DFU reset): `uv run tools/usb_log.py 10 --port 3-4`, `DK_USB_PORT=3-4 tools/flash_when_ready.sh` and `bun tools/pedal.ts --port 3-4 …` (or `DK_USB_PORT`). Without it, `usb_log.py` prints `-- usb: N Seeds attached (…); pass --port` and exits 2, `pedal.ts` prints `pedal: N Seeds attached (…); pass --port` and exits 2, and the flasher refuses when more than one `0483:5740`/`0483:df11` is present. `--identity` shows which pedal a path holds (`device 64` is Ducky); `pedal.ts` checks it before every command (`pedal: no Ducky on PATH`).
- **Port conflicts:** `usb_log.py` and `flash_when_ready.sh` claim the USB-MIDI interface through libusb. They detach the kernel driver, which drops the ALSA port while they run (`pedal.ts` then finds `no Seed MIDI port`). A program holding the ALSA port does not block them. It does block `amidi` and `pedal.ts`, which open the ALSA rawmidi device exclusively (`Device or resource busy`, `pedal: /dev/snd/midiC1D0 busy: …`); a browser tab using Web MIDI is a common holder. So `pedal.ts status` and the other `pedal.ts` commands cannot run at the same time.
- **Build warnings:** libDaisy's startup file, newlib's `_read`/`_write`/… stubs and the linker's "RWX segment" all warn on every build. Ducky's own sources build without warnings.
- **USB enumeration:** a SETUP aborts the control transfer before it (USB 2.0 §8.5.3), but ST's HAL leaves that transfer's unsent IN packet in EP0's TxFIFO. If the host re-sends a SETUP because its first ACK was lost, the reply is loaded twice. From then on every control read returns the reply to the request before it, until the next USB reset. The kernel then reads string 2 (the product) as string 0's langid table `04 03 09 04`, which is U+0409 "Љ", so the ALSA and Web MIDI port shows up as `Љ MIDI 1` and the editor's `/daisy/i` port match misses it. This happened on about 1 boot in 6 after a DFU flash. A device-side EP0 trace showed the langid SETUP arriving twice, 260 µs apart. `daisy/src/usb_link.cpp` therefore flushes TxFIFO 0 when a SETUP arrives with `DIEPTSIZ0.PKTCNT` non-zero. `daisy/Makefile` routes libDaisy's `USBD_LL_SetupStage` calls through it with `-Wl,--wrap`, so the libDaisy submodule stays unpatched.

## Source layout

| Path | Contents | Host-portable |
|---|---|---|
| `daisy/src/ducky.cpp` | The pedal: the boot slot scan, `AudioCallback`, the parameter step (`stepParams`), the preset switch (`servePresets`), the state record (`writeState`), the settings record (`saveSettings`), the link's slot and parameter commands (`onCommand`, `writeSlot`) and replies (`serveReplies`), the main loop, the log lines. | no |
| `daisy/src/bench.cpp` | `APP=bench`: the A2-Lite and IR device checks and the cycle timing of the engine, the IR, the EQ, the noise gate, the compressor and the whole chain ("Bench"). | no |
| `daisy/src/latency.cpp` | `APP=latency`: the FX loop's round trip and the crosstalk between the channels, from clicks on each output captured on both inputs (`docs/LOOP.md` "Measuring the loop"). | no |
| `daisy/src/board.h` | `dkboard::Board`: the HotHouse pins, knobs, toggles, footswitches. `kBoardId` 1, `kKnobs` 6, `kToggles` 3. | no |
| `daisy/src/firmware.h` | `dkfw::Start` (flush-to-zero, board init, MPU) and `kBuildId`. | no |
| `daisy/src/usb_link.h/.cpp` | `dkusb`: USB-MIDI transport, a non-blocking double-buffered send, `Serve` (Identity, HELLO, DFU restart, then other commands to the caller's `onCommand`). Also `__wrap_USBD_LL_SetupStage` (linked with `-Wl,--wrap`), which flushes EP0's TxFIFO when a SETUP arrives while an IN packet is still pending ("USB enumeration"). | no |
| `daisy/src/link.h` | `dklink`: the SysEx wire format, the septet and Pack7 codecs, `TxBuffer` (LOG, SLOT, RESULT, STATUS, DATA, PARAM, VALUES, Identity), `SysExAssembler`, `PedalCommand`, and the protocol tables `kCommands`/`kResults` that `docs/MIDI.md` is generated from. The protocol reference. | yes |
| `daisy/src/upload.h` | `dklink::Upload`: UPLOAD BEGIN/CHUNK staging (CRC per chunk, resends, gaps). | yes |
| `daisy/src/controls.h` | `dkctl::ToggleFilter` (30 ms toggle hysteresis), `dkctl::Blinker` (LED2's N blinks, repeated after 5 s dark). | yes |
| `daisy/src/params.h` | `dkparam`: the parameter table (`kParams`, `ParamDef`, `Units`), the control slots (`Accepts`), what a knob or a toggle gives a parameter (`FromKnob`, `FromToggle`), the wire codes (`ToCode`, `FromCode`). `docs/MIDI.md` "Parameters" is printed from it. | yes |
| `daisy/src/param_store.h` | `dkparam::Store`: the live values and assignments and the arbitration between the controls and the link's SET and ASSIGN (pickup, last control wins). | yes |
| `daisy/src/settings.h` | `dkparam`: the settings record (`BuildSettings`, `SettingsValid`, `NewestSettings`, `DecodeSettings`, `FactorySettings`) and its QSPI sectors ("Settings record"). | yes |
| `daisy/src/dsp/blocks.h` | `dkdsp::kSampleRate` (48 kHz), `kBlockSize` (24), `kBlockRate`, `kBlocksPerMs`, `kIrTaps` (2048); `Peak` (a block's peak) and `FpHere` (`docs/M7_PERFORMANCE.md`). | yes |
| `daisy/src/dsp/mute.h` | `dkdsp::MuteRamp`: FS1's 5 ms linear mute, the preset switch's fade and the loop mode's; `ProcessPair` fades two buffers with one ramp (the output and the send). | yes |
| `daisy/src/dsp/noise_gate.h` | `dkdsp::NoiseGate`, `GateSettings`, `kGateOff`, `GateValid`, `ClampGate`: the per-preset side-chained gate (`docs/FILTERS.md` "Noise gate"). | yes |
| `daisy/src/dsp/gain.h` | `dkdsp::InputGain` (Knob 1), `OutputGain` (Knob 2), `LevelMatchGain`, `Smoother` (one-pole), `GainRamp` (per-block linear gain ramp). | yes |
| `daisy/src/dsp/a2lite.h` | `dkdsp::A2Lite`: the A2-Lite network (`docs/NAM_FORMAT.md` "Engine"); `dkdsp::a2` holds the shape and memory layout. | yes |
| `daisy/src/dsp/convolver.h` | `dkdsp::IrConvolver`: the partitioned FFT IR convolver (`docs/FILTERS.md`). | yes |
| `daisy/src/dsp/filters.h` | The EQ's sections: `dkdsp::CutFilter` (with `EdgeBypass`, the cut maps), `DesignBell` and `Bell` (`docs/FILTERS.md`, `docs/EQ.md`). | yes |
| `daisy/src/dsp/eq.h` | `dkdsp::Eq`: the EQ stage, low-cut, bells 1–3, high-cut, driven by the parameters' values (`docs/EQ.md`). | yes |
| `daisy/src/dsp/compressor.h` | `dkdsp::Compressor`: the optical compressor before the input trim, the reduced OTC-201 model (`docs/compressor-spec.md` "DSP model"), driven by the parameters' values; `gain()` for the meter. | yes |
| `daisy/src/model.h` | `dkmodel::Parse`: validates and decodes a format-2 model binary. | yes |
| `daisy/src/ir.h` | `dkir::Parse`: validates and decodes a format-1 IR binary. | yes |
| `daisy/src/preset.h` | `dkpreset`: the slot format (`Parse`, `PutGate`/`GetGate`/`SetGate` for its gate block), the state record (`StateValid`, `NewestState`, `BuildState`), slot stepping (`NextFilled`, `BootSlot`), the QSPI layout ("Preset store"). | yes |
| `daisy/src/binary.h` | `dkbin`: `Crc32` and the little-endian readers and writer the binaries and the link share. | yes |
| `daisy/cmsis_fft.mk` | The CMSIS-DSP FFT sources and table defines every build compiles. | |
| `daisy/src/memory.h/.cpp` | `DK_ITCM`, `DK_DTCM`, `DK_DTCM_BSS`, `DK_D2`, `DK_SDRAM` placement and the boot-time TCM copy. | yes (`.h`) |
| `daisy/src/spsc_ring.h` | The lock-free single-producer/single-consumer ring (USB IRQ → main loop; the link's parameter edits, main loop → callback). | yes |
| `daisy/STM32H750IB_sram_ducky.lds` | libDaisy's BOOT_SRAM script plus the `.dtcm_data`, `.dtcm_bss`, `.itcm_text`, `.d2_bss`, `.sdram_bss` sections and the 16 KB stack reserve. | |
| `tests/` | GoogleTest for the host-portable headers. Never include `board.h`, `firmware.h` or `usb_link.h` here. | |
| `tools/usb_log.py`, `tools/flash_when_ready.sh` | Log reader with `--port`, `--identity` and `--dfu`, and the flasher (`DK_USB_PORT`). | |
| `tools/gen_midi_doc.cpp` | `build/host/gen_midi_doc`: prints `docs/MIDI.md` from `link.h` and `params.h` (`make midi-doc`; `make test` diffs it). | |
| `editor/src/link/`, `tools/pedal.ts`, `tools/rawmidi.ts`, `tools/slot_source.ts` | The link's TypeScript side (`protocol.ts`: builders, codecs, parsing; `session.ts`: `PedalSession`, the upload with its retries; `params.ts`: a parameter's code as its value and readout, the controls), the CLI, its ALSA rawmidi transport, and the model + IR → slot image loading it shares with `preset_bank.ts`. | |
| `editor/` (UI) | The browser editor; its layout: `docs/EDITOR.md` "Source layout". | |
| `editor/src/preset/info.ts` | `infoRanges`, `slotInfo`: a stored preset's metadata from the bytes READ fetches. | |
| `editor/src/nam/`, `tools/nam_convert.ts`, `tools/nam_reference.sh`, `tools/make_stimuli.py` | The `.nam` → model binary converter and its CLI, the NAM reference renders, and their stimulus (`docs/NAM_FORMAT.md`). | |
| `tools/comp_reference.py` | The compressor's Python reference (`docs/compressor-spec.md` appendix B verbatim) and the generator of `tests/fixtures/comp/` (`uv run tools/comp_reference.py`). | |
| `editor/src/ir/`, `tools/ir_convert.ts`, `tools/make_ir_fixture.ts` | The `.wav` → IR binary converter and its CLI, and the generator of `tests/fixtures/ir/decay.wav` (`docs/FILTERS.md`). | |
| `editor/src/preset/`, `tools/preset_bank.ts`, `tools/make_preset_fixture.ts` | The slot and bank builders (`buildSlot`, `buildBank`), the gate block and its units (`gate.ts`), the bank CLI, and the generator of `tests/fixtures/preset/A2_decay.bin`. | |
| `editor/src/device.ts`, `editor/src/crc32.ts` | `SAMPLE_RATE`, `NAME_MAX`, `sanitiseName`, `writeAscii`; CRC-32: shared by the converters, the preset code and the link. | |
| `tools/render.cpp`, `tools/wav.h` | `build/host/render` (`make render`): the engine, then the IR with `--ir`, over a WAV file; `dkwav` WAV reading and float32 writing (host only). | |
| `tests/fixtures/nam/` | Upstream MIT `.nam` and WAV fixtures, `A2.bin` converted from `A2.nam`, and the committed `A2.nam` reference render. | |
| `tests/fixtures/ir/` | `decay.wav` (generated: 44.1 kHz decaying noise, 4,410 frames) and `decay.bin` converted from it. | |
| `tests/fixtures/preset/` | `A2_decay.bin`: `A2.bin` with `decay.bin`, named `A2 + decay`, built by `editor/src/preset/bank.ts`; both the host tests and the editor tests check it. | |
| `tests/fixtures/comp/` | The compressor's reference fixtures from `tools/comp_reference.py`: inputs `step_30_300.in.wav`, `step_10_1000.in.wav`, `di.in.wav` (synthetic plucked notes, 1.5 s); outputs `step_30_300.s100.ref.wav`, `step_30_300.s50.ref.wav`, `step_10_1000.s100.ref.wav`, `di.ref.wav`; the static curve `static.csv`. `tests/compressor_test.cpp` checks the kernel against them and against the spec's SPICE tables. | |

- **Naming:** namespaces are `dkboard`, `dkfw`, `dkusb`, `dklink`, `dkctl`, `dkparam`, `dkdsp`, `dkbin`, `dkmodel`, `dkir`, `dkpreset` and (host only) `dkwav`. Macros use the `DK_` prefix, and the board define is `DUCKY_BOARD_HOTHOUSE`.
- **DSP code** goes under `daisy/src/dsp/`, host-portable and covered by `tests/`. The top-level `Makefile` rebuilds the tests and `render` whenever any `daisy/src/*.h`, `daisy/src/dsp/*.h`, `tests/*.h` or `tools/*.h` changes. Tests find fixtures through `DK_REPO_ROOT`, so `build/host/tests` runs from any directory.
- **Before writing or optimising a DSP kernel,** read `docs/M7_PERFORMANCE.md`: what float code costs on this core, measured, and the rules that follow. Host tests cannot see performance; the bench can.

## Threads

- **`AudioCallback`** is the audio DMA interrupt at 48 kHz, in 24-sample blocks (0.5 ms). It runs from ITCM, and its state, the engine, the EQ and the compressor included, lives in DTCM; the IR convolver lives in AXI SRAM. Control timings (toggle hysteresis, LED2's blinks) are set in milliseconds and counted in blocks (`dkctl::BlocksForMs`). In order, it:
  1. reads the controls;
  2. toggles the mute on FS1 and counts FS2 presses;
  3. applies the link's queued SETs and ASSIGNs, lets the control slots (Knobs 3–6, SW1–SW3) write their parameters (`dkparam::Store::Update`: pickup and holds, `docs/MIDI.md` "Controls"), and publishes the values, the assignments and the held controls;
  4. steps the preset switch's handshake ("Preset switch" below), then the loop mode (`stepLoop`): when `loop_mode` differs from the routing in use (`gLoop`), both outputs fade out over 5 ms (`gModeFade`), the routing follows at silence, and they fade back in;
  5. measures the input peak (the raw jack, before the compressor), takes new gate settings (`gGateWord`, published by the main loop) and runs the gate's detector on that peak (`NoiseGate::Process`, every block, dry and parked included);
  6. runs the chain, routed by `gLoop` (`docs/LOOP.md`). The send and return levels (`loop_send`, `loop_return`) are smoothed every block in every mode and ramped like the knobs. In MISO, on the left input, into the left output buffer, then in place:
     - `Compressor::Process` on the compressor's four parameters (step 3), from the input buffer into the output buffer (`docs/compressor-spec.md` "DSP model"). `comp_mode` off is a bit-exact bypass after a 20 ms crossfade (the fade out keeps the blend or direct mix it had) and runs nothing; turning it on restarts the side chain from rest;
     - the input trim: the model's input calibration times Knob 1, smoothed (one-pole, 20 ms) and ramped linearly across the block, −12…+12 dB around the calibration, which is at noon;
     - while a switch is parked, silence instead of the engine and the IR; otherwise, with a preset loaded:
       - `A2Lite::Process`, also while muted, so the load stays constant and the model stays warm;
       - `IrConvolver::Process` when the preset has an IR;
     - `Eq::Process` on the EQ's sixteen parameters (step 3): low-cut, bells 1–3, high-cut, after the IR (`docs/EQ.md`). The cuts run every block; a bell runs while its gain is not 0 dB (and for 100 ms after); `eq_enable` off stops the stage;
     - the output gain: Knob 2 smoothed and ramped the same way, times the level match and the gate's gain (so the gate rides the same ramp, `docs/FILTERS.md` "Noise gate"). Knob 2 is `(2k)^3` up to noon (off at fully CCW, about −18 dB at 9 o'clock, unity at noon), then dB-linear to +12 dB.

     In pre-model, the compressor writes the right output buffer, scaled by the send level: the send. The right input, scaled by the return level, goes into the left output buffer, and the rest of the chain runs on it as in MISO, from the input trim on. In post-model, the chain runs as in MISO up to the engine; the engine's output, scaled by the level match, the gate's gain and the send level, goes to the right output buffer (the send). The right input, scaled by the return level, goes into the left output buffer, then the IR, the EQ and Knob 2 (the volume alone). In all three modes:
     - `MuteRamp` (FS1) on the left output, then the preset fade (`gSwapFade`, another `MuteRamp`), which in post-model fades the send with it (`ProcessPair`);
  7. in MISO, copies the left output to the right; fades both by the mode fade; measures both outputs' and the right input's peaks and accumulates the gate's level and largest cut and the compressor's lowest gain for STATUS;
  8. drives LED1 (on unless muted) and LED2 (`Blinker`);
  9. measures the load and the underruns (a callback longer than one block period).
- **Dry:** with no valid slot, the chain is the compressor, the input trim, the EQ, the output gain and the mute, with the calibration and the level match at unity and the gate off; FS2 does nothing. The pedal goes dry at boot with no valid slot, or when the link erases the last one; it leaves dry when a slot is uploaded (`gModelOn` follows the loaded preset through the switch handshake).
- **Input calibration:** NeuralAmpModelerPlugin's: the input gain at noon is `kInputLevelDbu − input_level_dbu`, so a guitar drives the model as hard as it drove the captured amp. `kInputLevelDbu` is +4.5 dBu, the level at the input jack that reaches 0 dBFS, derived from the datasheets and not measured: the PCM3060 full scale is 0.6 VCC Vpp (VCC 4.5 V), behind the Seed's 3.6 kΩ series resistor into the codec's 10 kΩ input (3.67 Vpp), and the HotHouse input is a unity-gain buffer. A model captured at +15 dBu gets −10.5 dB. A model without `input_level_dbu` is assumed captured at +12 dBu, the plugin's default calibration level (−7.5 dB). Without calibration, noon drove the models about 10 dB harder than at capture, and they sounded distorted.
- **Level match:** `LevelMatchGain` brings the model's A2-Lite `loudness` to −18 dB (NeuralAmpModelerPlugin's normalisation target); a model without `loudness` plays at unity. `loudness` is measured at the capture level, before the input calibration, so a clean model plays a little quieter than a driven one at noon (SSS vs Fender: 1.8 dB, `docs/ROADMAP.md` M4). At boot the input gain starts at the calibration and the output gain at 0, so the output fades in over the knob time constant.
- **Main loop** runs `serveReplies` (LIST's SLOTs, DUMP's PARAMs and VALUES, the pending RESULT, STATUS and VALUES every 100 ms with WATCH on), `dkusb::Serve(logBoot, onCommand)`, `servePresets`, the `loop` line when `loop_mode` changes, and the once-a-second log lines. If the IR convolver's FFT does not initialise, audio never starts and the LEDs stay off; the main loop serves USB (DFU included) and logs `ir fft` once a second.
- **Sharing:** the two threads share only `std::atomic`s, each with one writer, and one single-producer single-consumer ring:
  - callback → main loop, relaxed: `gKnob`, `gToggle`, `gFsHeld`, `gMutedPub`, `gUnderruns`, `gPeak`, `gOutPeak`, `gRinPeak`, `gRoutPeak`, `gLoadAvg`, `gLoadMax`, `gFs2Presses`, `gGateLevel`, `gGateGain`, `gCompGain`, `gParamPub`, `gAssignPub`, `gHeldPub`; release/acquire: `gSwapParked`, `gEditsDone`;
  - main loop → callback, relaxed: `gPeakReset`, `gLoadReset`, `gBlinkCount`, `gGateWord` (the 8-byte gate block as two words: a torn pair mixes two valid settings for one block), `gMeterReset` (restarts the gate's and the compressor's meters); release/acquire: `gSwapReq`, `gSwapLoaded` (which publishes the plain `gNextLevel`, `gNextInCal`, `gNextIrOn`, `gNextModelOn`) and `gBlinkReq`; the ring `gEdits` (16 SETs and ASSIGNs).
- **Parameters:** the callback owns `gStore` (`dkparam::Store`, DTCM). SET and ASSIGN push an edit to `gEdits` and answer at once; the next block applies it (within 0.5 ms). DUMP and SAVE first wait until the callback has applied every edit pushed so far and published the result (`awaitEdits`: `gEditsDone` against the count pushed), so they see what the RESULTs before them promised. The step costs 327 cycles a block (1.4 permille), measured with the cycle counter: the knob slots write every block without comparing values, and a write tests one mask for other controls on its parameter (`docs/M7_PERFORMANCE.md`: a float compare and branch costs 6 cycles or more).
- **Gate settings:** `loadPreset` publishes the loaded preset's gate (`kGateOff` when dry) while the callback is parked, so a switch changes the gate in silence; GATE on the playing slot and GATE TRY publish while it plays, and the gate glides to the new settings.
- **Preset switch:** the engine and the convolver have two owners in turn, and the handshake hands them over.
  1. The main loop steps its target slot once per FS2 press (`NextFilled`), restarts LED2's blinks for it, and, when it differs from the loaded slot or a slot change asked for a reload (`gReload`: an upload into the playing slot), raises `gSwapReq`. The target may be dry (-1).
  2. The callback fades out over 5 ms. Once the fade lands on 0 it parks: it writes silence, touches neither the engine nor the convolver nor the gains, and stores the request in `gSwapParked`.
  3. The main loop loads the target (`A2Lite::Load` and `IrConvolver::Load` both reset their state), computes its gains, publishes its gate, and stores the request in `gSwapLoaded`.
  4. The callback takes the gains, the IR flag and the model flag (dry or not) and fades in over 5 ms.
  Presses during a switch move the target, and the next switch follows at once, so fast presses coalesce. 2 s after the last switch, the main loop writes the slot to the state record if it changed.

## Memory

| Region | Size | Holds now | Reserved for |
|---|---|---|---|
| ITCM `0x00000000` | 64 KB | `AudioCallback` (with the engine's other stages, the convolver, the noise gate, the EQ's and the compressor's sample loops, the gains, the loop's send and return ramps, the fades and the parameter store inlined), `A2Lite::RunLayer`, `Eq::Control`, `Compressor::Control`, `Compressor::SideChain` (19,392 bytes in all) | |
| DTCM `0x20000000` | 128 KB | The engine (92,116 bytes in `.dtcm_bss`: history 83,976, weights 7,480); callback state, the parameter store, the EQ and the compressor included; the 16 KB stack reserve at the top: 110,824 bytes in all | |
| AXI SRAM `0x24000000` | 480 KB | Code (CMSIS-DSP's FFT included), `.data`, `.bss` with the IR convolver (68 KB): 238 KB in total | |
| D2 SRAM `0x30008000` | 256 KB | — (the convolver measured slower here than in AXI SRAM, `docs/FILTERS.md`) | |
| SDRAM `0xC0000000` | 64 MB | The 8 parsed presets (`gPresets`, 127,296 bytes; the gear texts stay in flash), the link's upload buffer (16,288), RENAME/SWAP/GATE scratch (2 × 16,288) and COMMIT's parse target (`gCheck`, 15,912): 192,072 bytes in `.sdram_bss` | |
| QSPI `0x90000000` | 8 MB | Image at `0x90040000`; preset slots at `0x90400000`; state records at `0x90480000` ("Preset store"); settings records at `0x90482000` ("Settings record") | |

- **TCM copy:** `.itcm_text` and `.dtcm_data` are copied from their SRAM load image in `.preinit_array` (`memory.cpp`), before any constructor runs; `.dtcm_bss` is zeroed there.
- **No-load sections:** `.dtcm_bss` (`DK_DTCM_BSS`) is `NOLOAD`, so a large all-zero object such as the engine adds nothing to the image; only objects with an all-zero static initialiser belong there. `.d2_bss` and `.sdram_bss` are `NOLOAD` too, but nothing zeroes them; their owner clears them (`gPresets` is valid only where its slot parsed; the upload buffer only up to `Upload::next()`).
- **Template placement:** GCC 13 drops section attributes from template instantiations. A hot template reaches ITCM only when inlined into a `DK_ITCM` caller, so mark it `[[gnu::always_inline]]`. Check placement with `arm-none-eabi-nm -C daisy/build/hothouse/ducky.elf`.
- **MPU** (`firmware.h`):
  - Region 0 is a no-access background over `0x60000000–0xDFFFFFFF`, which covers all of QSPI. This avoids the ES0392 2.7.5 stall on speculative reads of `0x907FFFFF`.
  - Regions 1–3 are libDaisy's: D2 DMA, SDRAM and backup SRAM.
  - Region 4 is the preset store and the settings record, `0x90400000–0x904FFFFF` (1 MB): privileged read-only, write-through cached, execute-never; Bad Tape's region 4, moved and shrunk. The rest of QSPI, `0x907FFFFF` included, stays no-access.

## Preset store

`daisy/src/preset.h` is the reference and the contract with `editor/src/preset/bank.ts`. Tests: `tests/preset_test.cpp`, `editor/src/preset/bank.test.ts`; both check the committed `tests/fixtures/preset/A2_decay.bin`.

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
| 6 | u16 flags: bit 0 has IR; other bits 0 |
| 8 | u32 size: 7,996, or 16,288 with an IR |
| 12 | name[64], NUL-terminated printable ASCII |
| 76 | gate block (8 bytes): u8 flags (bit 0 enabled), u8 threshold (dB below full scale), u8 attack (0.1 ms), u8 hold (ms), u16 release (ms), u8 ratio (tenths), u8 0; `docs/FILTERS.md` "Noise gate" |
| 84 | model binary (7,908 bytes, `docs/NAM_FORMAT.md`) |
| 7992 | IR binary (8,292 bytes, `docs/FILTERS.md`), only with the IR flag |
| size − 4 | CRC-32 of bytes [0, size − 4) |

`dkpreset::Parse` checks, in order: `empty` (the first 4 bytes are 0xFF), `magic`, `version` (the version or an unknown flag), `size`, `crc`, then the embedded binaries with their own parsers (`model`, `ir`). A slot that does not parse is skipped. The gate block is not checked: each field is clamped into its range (`GetGate`), since the CRC already guards the bytes. New presets (the bank tool, an upload) carry the gate off at its defaults.

**State record:** 16 bytes at the start of each state sector: `DKST`, u32 seq, u32 slot (0–7), and the CRC-32 of the first 12 bytes. The valid record with the higher seq is the newest. A write erases and writes the other sector, then reads it back through the memory-mapped QSPI (`state slot N seq S T ms`, or `state write failed`), so a torn write leaves the previous record readable. At boot the pedal plays the saved slot if it parsed, else the first slot that did, else dry.

**Bank tool:** `bun tools/preset_bank.ts -o OUT.bin --slot N MODEL [--ir IR] [--name NAME] …` builds the 512 KB bank (`BANK_BYTES`), slot N (1–8) at `(N − 1) × 64 KB`, every byte not in a preset 0xFF.
- A `.nam` model is converted as `tools/nam_convert.ts` does; any other path is read as a model binary. A `.wav` IR is converted as `tools/ir_convert.ts` does; any other path is read as an IR binary.
- The name defaults to the model's.
- `--ir` and `--name` apply to the last `--slot`. A `.nam` whose gear type contains `cab` with an IR gets a warning.
- Errors write nothing: usage errors exit 2, and rejected files exit 1.
- The bank replaces all 8 slots. dfu-util erases the whole range before writing it, so power pulled mid-download leaves every slot empty (or the first few written): the pedal boots, plays dry or what is left, and the bank can be flashed again. The firmware at `0x90040000` and the state sectors are outside the range.

**Switching cost:** the load is `A2Lite::Load` (138 µs), plus `IrConvolver::Load` (about 860 µs) for a preset with an IR. The audible gap is the 5 ms fade out, the load, and the 5 ms fade in.

**Slot writes (M7):** UPLOAD COMMIT, ERASE, RENAME, SWAP and GATE (`docs/MIDI.md`) go through `writeSlot` in the main loop: erase the slot's 64 KB block, write the image, invalidate the D-cache over the slot, and compare it through the memory-mapped QSPI (an erase checks for 0xFF throughout), then re-parse the slot (`rescan`). COMMIT parses the staged image before it touches flash, so an invalid upload never erases a slot. RENAME, GATE and SWAP copy the slot images into SDRAM scratch first; SWAP writes a, then b. The audio callback reads no QSPI, so audio runs through every write; the main loop blocks for it (137–216 ms per slot, "Measured (M7)").
- **libDaisy quirk:** `QSPIHandle::Erase` over a whole 64 KB block uses the block-erase command and leaves the QSPI in indirect mode, where the next read of the mapped region stopped the pedal (the first ERASE on hardware never answered). `Write` and `EraseSector` return to memory-mapped mode, so an erase without data ends with an `EraseSector` of the slot's first 4 KB.
- **Playback after a slot change:** an upload into the target slot sets `gReload`, so it is reloaded through the usual switch (one fade); an upload while dry plays the new slot; a target slot that stops parsing (erase, failed write) moves to the next filled slot, with LED2's blinks, or to dry, which turns LED2 off. SWAP remaps the playing and target slot numbers without a switch, and the state record follows 2 s later. GATE on the playing slot hands the new gate to the callback without a reload. `docs/MIDI.md` "Slot changes and playback" is the host-facing summary.

## Settings record

`daisy/src/settings.h` is the reference; tests: `tests/settings_test.cpp`. The record holds what is global to the pedal and not part of a preset: which parameter each control slot (Knobs 3–6, SW1–SW3) drives, and every parameter's value (`docs/MIDI.md` "Parameters", "Controls").

| Offset | Field |
|---|---|
| 0 | magic `DKSG` |
| 4 | u32 seq |
| 8 | u8 version (3) |
| 9 | u8 control slots (7) |
| 10 | u8 parameter count N |
| 11 | u8 0 |
| 12 | assign[7]: a parameter id per control slot, or `7F` for none |
| 19 | u8 0 |
| 20 | f32 value[N], by parameter id: a continuous parameter's position 0..1 along its taper, a choice's index |
| 20 + 4N | CRC-32 of bytes [0, 20 + 4N) |

- **Two copies,** as for the state record: each of the two 4 KB sectors holds one record at its start. The valid record with the higher seq is the newest. SAVE (the link's command) erases and writes the other sector, then reads it back through the memory-mapped QSPI (`settings seq S T ms`, or `settings write failed` and RESULT `flash`), so a torn write leaves the previous record readable. Nothing else writes it: ASSIGN and SET change only the running pedal.
- **At boot** the newest record sets the assignments and the values (`settings seq S`); with none, the controls take the factory layout (`kFactoryAssign`, `docs/MIDI.md` "Controls") and every parameter is at its default (`settings factory`). The first audio block then writes every parameter on a control from that control, so a saved value matters only for a parameter on no control.
- **Decoding is defensive:** a stored value is clamped into its range, a choice rounded to an index, and a non-finite value replaced by the default; an assignment its slot does not accept (an unknown id, a continuous parameter on a switch) becomes none.
- **Across firmware versions:** parameter ids are append-only within a version. A record with fewer parameters than the firmware leaves the later ones at their defaults; one with more has the extra values ignored. SAVE always writes the running firmware's count. A table whose ids change bumps the version, and a record of another version is ignored, so the first boot after such a change reads `settings factory`. Version 3 merged M11's `comp_enable` (id 16) and `comp_mode` (id 19) into `comp_mode` (id 16: off, blend, direct, one toggle's three positions), moving `comp_balance` and the loop's three down one (23 parameters). Version 2 was M10's EQ table (16 parameters), M11's compressor (ids 16–20 appended) and M12's loop (ids 21–23 appended, 24 parameters). Version 1 was M9's cuts and 515 Hz mid.

## Link protocol

`daisy/src/link.h` is the reference: the command and result tables (`kCommands`, `kResults`) generate `docs/MIDI.md` (`make midi-doc`; `make test` fails when it is stale), which documents the framing, every command, the parameter table, the controls, the upload and the playback rules. Frames are `F0 7D 64 01 <cmd> … F7` (device byte `0x64`, version `0x01`). M0's HELLO, DFU, LOG and Identity are unchanged; M7 adds LIST, ERASE, RENAME, SWAP, WATCH, UPLOAD BEGIN/CHUNK/COMMIT, and the SLOT, RESULT and STATUS replies; M8 adds READ and its DATA reply (up to 160 bytes of an ok slot's image, copied from the mapped QSPI and sent by `serveReplies` before READ's RESULT); the noise gate adds GATE (rewrite a slot's gate block), GATE TRY (set the running gate only, for the editor's sliders) and STATUS's gate level and cut (30 bytes); M9 adds DUMP (the parameter table as PARAMs, then a VALUES), SET, ASSIGN, SAVE, and a VALUES after every STATUS under WATCH; M11 adds the compressor's parameters (ids 16–20) and STATUS's comp cut (32 bytes: the largest reduction of the compressor's low-frequency gain below Sustain's since the previous STATUS, tenths of a dB, 0 when off). Every request except HELLO and DFU gets exactly one RESULT, last.

**CLI:** `bun tools/pedal.ts [--port BUS-PORT] COMMAND`, slots 1–8:

```
list                                     # one line per slot: N STATUS "NAME" model "M" ir "I"|none, * on the playing one
upload N MODEL [--ir IR] [--name NAME]   # MODEL/IR as preset_bank.ts reads them; prints flash ms, retries, seconds
erase N | rename N NAME | swap A B       # each prints the flash time
gate N on|off [THRESHOLD_DB ATTACK_MS HOLD_MS RELEASE_MS RATIO]  # e.g. gate 1 on -55 0.5 20 100 4; on|off alone keeps the stored settings
info N                                   # READs the preset's metadata: name; model, gear type/make/model, modeled by; loudness, input level; IR taps, source, gain; gate
param list | param get NAME              # each parameter: its name, value and controls (`low_cut_freq    106 Hz        K5`), ` (held)` on a control waiting to move
param set NAME VALUE                     # VALUE in the parameter's unit (Hz, Q, dB), a choice's label or index, or `off`: `param set high_cut_freq 3000`, `param set bell2_gain -3`
param assign CONTROL NAME|none           # CONTROL k3-k6 or sw1-sw3: `param assign k3 bell2_freq`; a switch takes only choice parameters
param save                               # SAVE: the assignments and the values for the next power-up; prints the flash time
status [SECONDS]                         # WATCH: a STATUS line every 100 ms (with `gate in … dBFS cut … dB`, ending `comp cut … dB`) and the LOG lines, for SECONDS (5) or until Ctrl-C
```

- Usage errors exit 2; a refused command (`pedal: RENAME: slot-state`), a rejected file or no reply exits 1.
- The upload resends a CHUNK on no reply (1 s) or `crc` and gives up after 5 tries of one chunk; COMMIT is not resent (`UPLOAD COMMIT: no reply; check with list`).
- Transport: the ALSA rawmidi device of the Seed's sound card (`/dev/snd/midiC<N>D0`, found through `/sys/class/sound/card*/device` with `0483:5740`), opened exclusively and nonblocking; input pending at open (stale replies) is dropped. The protocol code (`editor/src/link/`) is plain TypeScript, shared with the editor, which supplies a Web MIDI transport instead (`editor/src/midi.ts`).
- `pedal.ts` and `usb_log.py` cannot run together ("Port conflicts").
- Unknown commands with this header still log `link: unknown command CC`. Other devices' frames are ignored.

## Editor

The browser editor: `docs/EDITOR.md`.

## Log lines

```
ducky hothouse seed 1.2-pcm3060 sysclk 480 MHz audio 48000 Hz 24 bit block 24 06ea416        (boot / HELLO)
slots ok ok empty ok empty empty empty empty                                                  (boot / HELLO)
preset 1 "FNDR BFSR VB Edge WRM2 CAB FREE"                                                    (boot / HELLO)
model loudness -15.9 dB level -2.1 dB input +15.0 dBu cal -10.5 dB "FNDR BFSR VB Edge WRM2 CAB FREE"  (boot / HELLO)
ir taps 2048 src 48000 Hz gain -15.1 dB "YA FDMN 412 BLND Mix 01"                             (boot / HELLO)
gate on thr -55 dBFS atk 0.5 ms hold 20 ms rel 100 ms ratio 4.0                              (boot / HELLO, after a switch)
settings seq 5                                                                                (boot / HELLO)
layout k3 bell2_freq k4 high_cut_freq k5 low_cut_freq k6 none sw1 low_cut_slope sw2 none sw3 eq_enable  (boot / HELLO)
loop miso                                                                                     (boot / HELLO, when loop_mode changes)
preset 4 "A2" load 138 us                                                                     (after a switch, with model and ir)
state slot 4 seq 12 85 ms                                                                     (2 s after a switch)
upload slot 8 "A2" 167 ms                                                                     (link: UPLOAD COMMIT, then the slots line)
erase slot 1 215 ms                                                                           (link: ERASE, then the slots line)
rename slot 2 "Renamed" 156 ms                                                                (link: RENAME, then the slots line)
gate slot 1 on 147 ms                                                                         (link: GATE, then the slots line)
swap slots 1 8 314 ms                                                                         (link: SWAP, then the slots line)
assign k3 low_cut_slope                                                                       (link: ASSIGN)
settings seq 6 81 ms                                                                          (link: SAVE)
ctl k 31 22 33 58 25 34 sw 2 0 1 fs 0 0                                                       (every second)
load avg 361 max 363 permille underruns 0 mute off in -84 dBFS out -31 dBFS rin -110 dBFS rout -31 dBFS  (every second)
```

- **`slots` line:** each slot's `dkpreset::StatusName`, slots 1–8.
- **`preset` line:** the playing slot (1-based) and its name, with the load time in µs after a switch; `preset none` when dry.
- **`model` line:** the preset's model: `loudness` and `input_level_dbu` (`none` without them), the level-match and input-calibration gains, in tenths of a dB, then its name, last because log lines stop at 120 characters. `model none` when dry.
- **`ir` line:** the preset's IR: tap count, source rate, the converter's normalisation gain (reference band at 0 dB, `docs/FILTERS.md` "Converter"), and its name. `ir none` without an IR or when dry. `ir fft` alone, every second, when the convolver's FFT does not initialise.
- **`gate` line:** the preset's noise gate (`docs/FILTERS.md` "Noise gate"): threshold, attack, hold, release and ratio; `gate off` when it is off or dry.
- **`settings` line:** at boot and on HELLO, the seq of the settings record the pedal started from, or `settings factory` with none; after SAVE, the seq written and the time the erase, write and readback took; `settings write failed` when the readback does not match.
- **`layout` line:** the live assignments: each control slot (`k3`–`k6`, `sw1`–`sw3`) and its parameter's name, or `none`. One parameter per control fits the 120 characters; a layout with long names repeated on several controls is cut.
- **`loop` line:** `loop_mode` as published: `miso`, `pre-model` or `post-model` (`docs/LOOP.md`). Logged at boot and on HELLO after the `layout` line, and by the main loop whenever the value changes, whichever control or SET changed it.
- **`assign` line:** the control and the parameter (or `none`) an ASSIGN put on it. SET logs nothing.
- **`state` line:** the slot and seq written to the state record and the time the erase and write took; `state write failed` when the readback does not match.
- **Slot change lines** (`upload slot`, `erase slot`, `rename slot`, `swap slots`, `gate slot`): the slot(s), 1-based, the new name or the gate's on/off, and the time the flash erase, write and readback took; then the `slots` line. `slot write failed N` when a write or its readback failed (the command answers `flash`). GATE TRY logs nothing.
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
  - `rin`, `rout`: the right input's and the right output's peaks the same way: the loop's return and send in a loop mode, the copy of `out` and an unused input in MISO.

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

- **Level:** the converter normalises an IR to 0 dB over 80 Hz–5 kHz, so a cab IR leaves an amp model's level close to where it was: on `A2.bin` with `input.wav` the FDMN mix adds 2.8 dB RMS, and on the pedal at the same controls the `out` peak was −8…−16 dBFS with the IR and −9…−15 without. The first M5 build normalised to unit energy, which added 10.7 dB and reached 0 dBFS (`docs/FILTERS.md`).

## Measured (M6, Seed 1.2 / PCM3060)

Bank: slot 1 the Fender with the FDMN IR, slot 2 the SSS, slot 4 `A2.nam`.

| | |
|---|---|
| Load, slot with an IR | avg 500, max 503 permille |
| Load, slot without | avg 394 permille |
| Load, dry (empty bank) | avg 42 permille |
| Preset load | 138 µs without an IR, 994 µs with one |
| State write (4 KB erase, 16 B write, readback) | 80–85 ms |
| Underruns | 0 over 16 logged switches and 7 state writes |
| DTCM | 115,896 bytes of 128 KB |
| ITCM | 8,648 bytes of 64 KB |
| SDRAM | 127,232 bytes (the 8 parsed presets) |

- **Switching:** on the pedal, while playing, LED2 blinks the slot number, FS2 skips the empty slot 3 and wraps, and no switch clicks. The saved slot survives a power cycle.
- **Corrupt slot:** a byte flipped in slot 2 logs `slots ok crc empty ok …`; the saved slot 2 falls back to slot 1, and FS2 goes 1 → 4 → 1.
- **Power loss during a bank write:** power was pulled at 84 % of dfu-util's erase pass. The pedal booted with `slots empty …` and played dry, and the next bank flash restored all three slots.

## Measured (M7, Seed 1.2 / PCM3060)

Over the link with `bun tools/pedal.ts`, audio running throughout (input −16…−21 dBFS); bank as in M6 before and after.

| | |
|---|---|
| Upload, wall time (identity check excluded) | 0.3 s with an IR (16,024 bytes, 63 chunks), 0.2 s without (7,732, 31), flash included |
| Upload retries | 0 over 16 uploads |
| Flash time: upload (64 KB erase, write, readback) | 137–173 ms |
| Flash time: erase (64 KB erase, 4 KB sector erase, 0xFF check) | 188–215 ms |
| Flash time: rename | 152–156 ms |
| Flash time: swap (two slot writes) | 301–316 ms |
| Load during the operations | avg 500, max 502–504 permille with an IR, 394/395 without, 42 dry: as in M6 |
| Underruns | 0, across all of the above and a 10 s `status` run (100 STATUS lines, one per 100 ms) |
| DTCM | 115,896 bytes of 128 KB |
| ITCM | 8,664 bytes of 64 KB |
| SRAM | 231,460 bytes of 480 KB |
| SDRAM | 191,208 bytes |
| Image | 132,984 bytes |

- **Playback:** an upload into the playing slot logged one `preset` reload; an upload while dry started playing it; erasing the playing slot moved to the next filled one (1 → 2 → … → 8 as each was erased), and the last erase went dry (`preset none`, `status … slot dry`). SWAP 1 8 with slot 1 playing logged no `preset` line, `list` then marked slot 8 playing, the state record followed (`state slot 8`), and slot 8 played after a restart (a DFU reflash, not a power cycle).
- **Refusals:** `rename` of an empty slot answers `slot-state`, `swap 2 2` `bad-request`, `upload 9` is a usage error (exit 2).
- **Not checked by ear or eye in this run:** the reload's fade, LED2's blinks after an erase, and the STATUS knob/switch fields following moving controls (they matched the `ctl` line at rest).

## Measured (M9, Seed 1.2 / PCM3060)

Preset 4 (no IR), factory layout, audio running throughout; over the link with `bun tools/pedal.ts` and the editor.

| | |
|---|---|
| Parameter step (`stepParams`), DWT cycle counter | 327 cycles a block: the edit queue 37, `Store::Update` 182, publishing 84, the edit count 15 |
| The same, first version | 545: `Update` 376–385 (a loop over the 7 controls at each knob write, float compares), publishing 108 |
| Load | avg 417, max 420–421 permille; 415, 418–419 on the firmware before |
| Underruns | 0, through every command below and a 3 s `status` run (30 STATUS lines) |
| WATCH | 16 STATUS and 15 VALUES in 1.5 s |
| Flash time: SAVE (4 KB sector erase, write, readback) | 78–85 ms |
| DTCM | 109,824 bytes of 128 KB |
| ITCM | 11,984 bytes of 64 KB |
| SRAM | 243,748 bytes of 480 KB |
| SDRAM | 192,072 bytes |
| Image | 145,280 bytes |

- **Boot:** `settings factory` and the factory `layout` line on a pedal that had no record.
- **SET:** `param set high_cut_freq 3000` answered `3.00 kHz`, and `param get` showed `K4 (held)`.
- **ASSIGN:** `param assign k3 low_cut_slope` kept the slope's value and marked K3 held; `param assign sw1 low_cut_freq` was refused by the CLI (`SW1 takes only choice parameters`); `param assign k6 none` left `low_cut_res` on no control.
- **SAVE and restart** (a DFU reflash, not a power cycle): `settings seq 1`, `layout k3 low_cut_slope k4 high_cut_freq k5 low_cut_freq k6 none …`; `low_cut_res`, on no control, came back at the saved Q 1.50; `high_cut_freq` went back to Knob 4's position.
- **Not checked in this run:** moving a held knob or switch (the pickup), a choice parameter on a knob by ear, and a power pull during SAVE.

## Measured (M11, Seed 1.2 / PCM3060)

Slot with a model and no IR, not playing; the compressor's parameters set with `bun tools/pedal.ts param set`.

| | |
|---|---|
| Load, compressor off | 415 permille |
| Load, compressor on | 446 permille |
| Underruns | 0 |
| DTCM | 110,768 bytes of 128 KB |
| ITCM | 17,800 bytes of 64 KB |

- **Host:** the kernel against `tools/comp_reference.py`: max abs diff 3.6e-6, 1.8e-6 and 4.4e-6 on the three steps, 5.4e-7 on the DI; `static.csv` worst 0.0000 dB; the static curve against SPICE worst 0.113 dB; the stage response against the spec's table worst 0.083 dB; lowest step gains 4.44, 0.49 and 5.74 dB (SPICE 4.77, 0.53, 5.87).

## Measured (M12, Seed 1.2 / PCM3060)

Slot with a model and no IR, compressor off, the send patched to the return (`docs/LOOP.md` "Measured"), while playing; the mode set with `bun tools/pedal.ts param set loop_mode …`.

| `loop_mode` | Load avg | Load max |
|---|---|---|
| MISO (before M12: 415 / 419) | 419 | 422 |
| pre-model | 422 | 426 |
| post-model | 423 | 427 |

- **Underruns:** 0 in every mode, across mode changes and level sweeps.
- **Memory:** DTCM 110,824 bytes of 128 KB (110,768 before); ITCM 19,392 bytes of 64 KB (17,800 before); image 170,756 bytes (168,076 before).
- **Settings:** `param set loop_mode post-model` logged `loop post-model`; `param assign sw1 loop_mode` was accepted; after `param save` and a reflash the boot showed `loop post-model`, then SW1, in the middle, took over (`loop pre-model`), as a control does at boot.

## Bench

`APP=bench` (`daisy/src/bench.cpp`) has no audio. Its `chain` is MISO's: the loop modes add only two `GainRamp`s and a two-buffer fade, so their cost is measured live ("Measured (M12)"). It compiles in `tests/fixtures/nam/A2.bin`, `tests/fixtures/ir/decay.bin` and a slice of `input.wav` and `A2.input.ref.wav` (`.incbin`; the Makefile rebuilds `bench.o` when they change). It places the engine like the pedal does: the object in DTCM, its stages and the timed blocks in ITCM; the EQ and the compressor in DTCM and the pedal's convolver (`gIrAxi`) in AXI SRAM, with a second one (`gIrD2`) in D2 SRAM for comparison. The parameter values both read are one array, `gValues`; `eqValues` and `compValues` each write their own ids. Each pass:

1. **Check:** a reset engine runs `input.wav[48000, 72000)` (the file's first second is silence, so a reset engine is already in that state) against the reference render; limit 1e-5.
2. **IR check:** the reset convolver runs `input.wav[48000, 52800)` against a direct convolution computed on the device in double; limit 1e-5.
3. **Model:** `A2Lite::Process` on `kBlockSize` blocks of uniform noise (±0.5), timed with the DWT cycle counter: 200 ms of warm-up blocks, then 2 s measured, interrupts masked inside each block. The M3 gate is the maximum against 450 permille of the block period.
4. **Layers:** the same blocks, each stage (`BeginChunk`, `RunLayer` per layer, `EndChunk`) timed separately.
5. **IR, EQ, noise gate, compressor, chain:** the same way, the convolver from D2 and from AXI SRAM; the EQ at its worst case: enabled, both cuts at mid-travel with the top resonance and their slopes flipping between 24 and 12 dB every 10 blocks (slope fades), every bell flipping between −12 dB at a quarter and +12 dB at three quarters of its frequency range every 10 blocks, so every glide and coefficient ramp runs on every block; the noise gate enabled with the longest hold (100 ms, the longest detector scan), fed each block's input peak; the compressor (`comp`) on in blend, Sustain 0.25 ↔ 1.0, Level −3 ↔ +3 dB and Balance 0.25 ↔ 0.75 flipping every 10 blocks, so every smoother, ramp and `powf` runs on every block, while the noise (0.92 V at the jack) keeps the side chain compressing and clipping; and the pedal's whole chain (the gate, compressor, input gain, engine, IR, EQ, output gain with the gate's gain, mute). The chain gate is its maximum against 850 permille, the load ceiling (`docs/ROADMAP.md` "Decisions"; 550 until the 24-sample blocks). M10's figures: `docs/EQ.md` "As built"; M11's: "Measured (M11)" below.

Passes repeat every 2 s; the DFU command works between stages, so `flash_when_ready.sh` returns to the pedal with no button presses.

A pass with 48-sample blocks (M5); the 24-sample figures are in `docs/FILTERS.md` "Measured":

```
ducky_bench hothouse sysclk 480 MHz 999fce4+dirty                                 (boot / HELLO)
bench model "A2" ok
bench ir "decay" ok
bench pass 1 budget 480000 cycles
bench check samples 24000 max err 241e-9 limit 10000e-9 ok
bench ir check samples 4800 max err 182e-9 limit 10000e-9 ok
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

The compressor (M11) adds `bench comp cycles avg … max … permille avg … max …` after the noise gate's line.

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
- **IR check:** max error 182e-9.
- **IR placement:** the convolver's 66 KB of spectra pass through the 16 KB D-cache every block; from AXI SRAM it costs 21 % less than from D2 SRAM, so the pedal keeps it in `.bss`.
- **Image:** bench 299,728 bytes; SRAM 94.6 % (the second convolver and the IR check's buffers).

### Measured (M11, Seed 1.2, 24 samples)

| Stage | Avg cycles | Max permille |
|---|---|---|
| Compressor (`comp`) | 7,108 | 30.0 |
| Whole chain (`chain`, with the compressor) | 156,559 | 657.4 |

- **Gate:** go, 657.4 permille against 850 (626.9 before the compressor, `docs/FILTERS.md` "Measured").
- **Image:** bench DTCM 110,600 bytes; SRAM 99.09 %.

How the engine got here from 570 permille, the measured cost model behind it, and how to check an image for a regression: `docs/M7_PERFORMANCE.md`. Run the bench after any change to the engine, the compiler or its flags: the gate line is the only test that sees a slowdown.
