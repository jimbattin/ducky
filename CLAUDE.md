# Ducky
Ducky is a NAM (Neural Amp Modeler) player for the DaisySeed, hosted in a HotHouse pedal platform. 

It supports 8 presets. Each preset contains a NAM Model and an optional IR block

## IO Configuration
- Mono In (Use L channel on the Codec's ADC)
- Dual Mono Out (L + R are identical; just copy L to R)

## Signal chain

L INPUT -> [Peset: NAM Model -> IR] -> EQ -> L/R Out 

Presets contain the NAM Model and optional IR. They do not track Input/Output gain or EQ settings - Those are "What you see is what you get" on the pedal

## Controls
- FS1: Mute. LED1 active when not muted; off when muted
- FS2: Next preset (skips empty slots, wraps). LED2 blinks N times for slot N
- Knob 1: Input Gain trim, relative (Noon is the model's input calibration from `input_level_dbu`, ±12 dB)
- Knob 2: Output Volume (Noon is unity)
- Knob 3: High-cut resonance
- Knob 4: High-cut frequency
- Knob 5: Low-cut frequency
- Knob 6: Low-cut resonance
- SW1: low-cut slope (6, 12, 24db)
- SW 2: high-cut slope (6, 12, 24db)
- SW 3: Mid boost/scoop (-7db, bypass, +5db) at 515hz

Roadmap, decisions, and milestones: `docs/ROADMAP.md`
Firmware build/flash/log, source layout, memory map, log lines, bench (`APP=bench`): `docs/FIRMWARE.md`
NAM A2 format, A2-Lite shape check, weight order, forward pass, Ducky model binary, converter, reference renders, engine: `docs/NAM_FORMAT.md`
IR convolver, cut/mid filters, IR binary, `.wav` converter, `IR=` build option: `docs/FILTERS.md`
Cortex-M7 float performance (measured cost model, rules for DSP kernels, steering GCC, how to measure). Read before writing or optimising DSP code: `docs/M7_PERFORMANCE.md`

## Technology
- Supports NAM A2-lite models
- USB-MIDI supports unattended DFU flashing
- NAM Models stored in QSPI Flash memory until loaded into SRAM
- Up to 8 slots for presets, flash space permitting 
- Editor follows conventions of bad_tape (`../bad_tape`)
- Can be flashed as part of Switcharoo (`../switcharoo`) although do not directly reference this in code or docs 
- More than one Seed may be attached: address Ducky's by USB path (`--port`, `DK_USB_PORT`) and check `--identity` reports `device 64` before flashing (`docs/FIRMWARE.md`)

This project lives in a private repo that will one day get its history squashed and become public

## Directories
- `docs/`: Place all documentation here
- `tools/`: Reusable tools go here. 
- `daisy/`: Pedal code
- `editor/`: Editor code (Bun + TS; `src/nam/` is the `.nam` converter shared with `tools/nam_convert.ts`, `src/ir/` the `.wav` IR converter shared with `tools/ir_convert.ts`)
- `models/`: NAM Models (git-ignored)
- `tests/`: Host GoogleTest; `tests/fixtures/nam/` holds upstream MIT NAM fixtures, `A2.bin` converted from `A2.nam`, and the committed reference render; `tests/fixtures/ir/` a generated IR (`decay.wav`, `decay.bin`)
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

