# Ducky
Ducky is a NAM (Neural Amp Modeler) player for the DaisySeed, hosted in a HotHouse pedal platform. 

It supports 8 presets. Each preset contains a NAM Model, an optional IR block and a noise gate (off by default)

## IO Configuration
- Mono In (Use L channel on the Codec's ADC)
- Dual Mono Out (L + R are identical; just copy L to R)

## Signal chain

L INPUT -> Input trim -> [Preset: NAM Model -> IR] -> EQ (low-cut, 3 bells, high-cut) -> Output volume × [Preset: noise gate gain, detected from L INPUT before the trim] -> Mute -> L/R Out

Presets contain the NAM Model, optional IR and noise gate settings. They do not track Input/Output gain or EQ settings: those are global. A parameter on a control is "What you see is what you get" on the pedal; the editor can set one, and it holds until its control moves

## Controls
- FS1: Mute. LED1 active when not muted; off when muted
- FS2: Next preset (skips empty slots, wraps). LED2 blinks N times for slot N, repeating after 5 s dark
- Knob 1: Input Gain trim, relative (Noon is the model's input calibration from `input_level_dbu`, ±12 dB). Fixed
- Knob 2: Output Volume (Noon is unity). Fixed
- Knobs 3-6 and SW1-SW3 are assignable: each drives one parameter of the table (`daisy/src/params.h`) or none, chosen in the editor's Controls tab or with `tools/pedal.ts param assign`, and kept across power cycles by an explicit save (the settings record). A switch takes only choice parameters. Every control is unassigned by default; the EQ is set from the editor's EQ tab (a draggable response graph and a row per parameter) or `tools/pedal.ts param set`

Roadmap, decisions, and milestones: `docs/ROADMAP.md`
Firmware build/flash/log, source layout, memory map, preset store, settings record, log lines, bench (`APP=bench`): `docs/FIRMWARE.md`
Browser editor (running it, each tab with screenshots, link and sync, drafts and saving, gate audition, the Controls and EQ tabs, source layout, conventions): `docs/EDITOR.md`
USB-MIDI SysEx link (commands, the parameter table, the controls and their pickup rule, upload, slot read-back, status stream, playback rules), generated from `daisy/src/link.h` and `daisy/src/params.h` by `make midi-doc`: `docs/MIDI.md`
NAM A2 format, A2-Lite shape check, weight order, forward pass, Ducky model binary, converter, reference renders, engine: `docs/NAM_FORMAT.md`
IR convolver, cut filters, noise gate, IR binary, `.wav` converter, getting an IR onto the pedal: `docs/FILTERS.md`
The EQ (low-cut, three bells, high-cut: controls and ranges, the model, setting A's reference response, required behaviour, as built and its cost): `docs/EQ.md`
Cortex-M7 float performance (measured cost model, rules for DSP kernels, steering GCC, how to measure). Read before writing or optimising DSP code: `docs/M7_PERFORMANCE.md`
Compressor research, nothing in the firmware (Dinosaural OTC-201 / Aion Convex circuit analysis, VTL5C4 vactrol model, SPICE predictions and netlist, the reduced DSP model with its constants, Python reference implementation and agreement with SPICE, test targets, open questions): `docs/compressor-spec.md`

## Technology
- Supports NAM A2-lite models
- USB-MIDI supports unattended DFU flashing, and slot and parameter management while the pedal plays (`bun tools/pedal.ts list|info|upload|erase|rename|swap|gate|param|status`)
- Presets (NAM Model + optional IR + noise gate) stored in QSPI Flash (8 × 64 KB slots at `0x90400000`, written slot by slot over USB-MIDI by `tools/pedal.ts`, or as a bank image by `tools/preset_bank.ts` over DFU) and parsed into SDRAM at boot and after each slot change; the last slot is remembered in a two-copy state record (`docs/FIRMWARE.md` "Preset store")
- Up to 8 slots for presets, flash space permitting 
- Parameters (the EQ's sixteen) live in one table (`daisy/src/params.h`) that the link sends to hosts; the control assignments and the values are global and saved in a two-copy settings record in QSPI (`docs/FIRMWARE.md` "Settings record")
- Browser editor (Preact, one self-contained `editor/dist/editor.html`; `make -C editor serve|html`): imports `.nam` + `.wav`, manages the 8 slots and each preset's noise gate (sliders auditioned live, a meter), assigns Knobs 3-6 and SW1-SW3 (Controls tab), sets the EQ with a draggable response graph and parameter rows (EQ tab), shows stored slots' metadata (read back over USB-MIDI), the log, CPU load and the controls, and has a DFU button. Follows the conventions of bad_tape (`../bad_tape`)
- Can be flashed as part of Switcharoo (`../switcharoo`) although do not directly reference this in code or docs 
- More than one Seed may be attached: address Ducky's by USB path (`--port`, `DK_USB_PORT`) and check `--identity` reports `device 64` before flashing (`docs/FIRMWARE.md`)

This project lives in a private repo that will one day get its history squashed and become public

## Directories
- `docs/`: Place all documentation here
- `tools/`: Reusable tools go here. 
- `daisy/`: Pedal code
- `editor/`: Editor code (Bun + TS + Preact; `src/App.tsx` and `src/components/` the UI, `src/midi.ts` the Web MIDI link; `src/nam/` is the `.nam` converter shared with `tools/nam_convert.ts`, `src/ir/` the `.wav` IR converter shared with `tools/ir_convert.ts`, `src/preset/` the preset slot and bank builder and the slot metadata reader shared with `tools/preset_bank.ts` and `tools/pedal.ts`, `src/link/` the SysEx link protocol, session and parameter readouts shared with `tools/pedal.ts`)
- `models/`: NAM Models (git-ignored)
- `irs/`: Example cab IRs (commercial York Audio packs; git-ignored). Mono 24-bit PCM WAV, 500 ms, in 44.1k/48k/96k folders, plus Kemper `.kipr` (not supported); macOS `._*` and `.DS_Store` files mixed in
- `tests/`: Host GoogleTest; `tests/fixtures/nam/` holds upstream MIT NAM fixtures, `A2.bin` converted from `A2.nam`, and the committed reference render; `tests/fixtures/ir/` a generated IR (`decay.wav`, `decay.bin`); `tests/fixtures/preset/` a preset built from those two (`A2_decay.bin`)
- `renders/`: Local reference renders and stimuli (git-ignored)

## Reference material
- STM32H7: `../dp4/manuals/an4891-stm32h72x-stm32h73x-and-singlecore-stm32h74x75x-system-architecture-and-performance-stmicroelectronics.pdf`
  - `../dp4/manuals/DDI0403E_e_armv7m_arm.pdf`
  - `../dp4/manuals/DDI0489B_cortex_m7_trm.pdf`
  - `../dp4/manuals/pm0253-stm32f7-series-and-stm32h7-series-cortexm7-processor-programming-manual-stmicroelectronics.pdf`
- NAM website:https://www.neuralampmodeler.com/
- NAM reference implementation: https://github.com/sdatkinson/NeuralAmpModelerCore (`NAM/wavenet/a2_fast.cpp`, `tools/render.cpp`; see `docs/NAM_FORMAT.md`)
- Models: https://www.tone3000.com/
- HotHouse: `../hothouse_pcb` (You can also lok at the `../bad_tape` project as an example)

