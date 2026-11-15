# Ducky roadmap

A living plan. Milestones are done in order; any of them may be revised, split, or dropped as the project learns.

## Decisions

| Topic | Decision |
|---|---|
| Model format | NAM A2 `.nam` (`SlimmableContainer`, 48 kHz). Only the A2-Lite submodel (`max_value` 0.5) runs. A1 models and A2-Full are rejected. |
| Model shape | A2-Lite is fixed at compile time: 23 layers, 3 channels, bottleneck 3, kernels 6 (layers 15–16: 15), dilations `1 3 7 17 41 101 239 ×2, 1 13, 1 3 7 17 41 101 239`, LeakyReLU 0.01, no gating, FiLM, or groups. The head is a convolution with kernel 16, then `head_scale`. The editor rejects any other shape. Each stored model carries a shape descriptor, and the firmware checks it. |
| Sample rate / block size | 48 kHz and 48 samples (1 ms). No resampling. |
| Input gain (Knob 1) | Relative trim of ±12 dB, 0 dB at noon. Codec full scale feeds the model unchanged. The model's `input_level_dbu` is shown in the editor, not applied, and there is no hardware calibration. |
| Output (Knob 2) | Off … +12 dB, unity at noon. Each preset's output is level-matched using the model's `loudness` metadata. |
| Cut filters | Knob 5 sets the low-cut (high-pass) cutoff, 20–500 Hz. Knob 4 sets the high-cut (low-pass) cutoff, 1.5–20 kHz. Both use a log taper. Turning the low-cut fully counter-clockwise or the high-cut fully clockwise bypasses that filter. Knob 6 sets the low-cut resonance and Knob 3 the high-cut resonance (Q 0.5 to about 2, only on the 12 and 24 dB slopes). SW1 sets the low-cut slope and SW2 the high-cut slope: up 6, middle 12, down 24 dB/oct. |
| Mid (SW3) | Peaking filter at 515 Hz: −7 dB / bypass / +5 dB. |
| IR | Optional, up to 2048 taps (about 43 ms). The editor resamples to 48 kHz, trims, and normalises. It warns when the model's `gear_type` already includes a cab. |
| Preset switching | FS2 steps forward through filled slots only and wraps. Audio fades out, the model is swapped and reset, then audio fades back in (a gap of about 20–30 ms, no crossfade). LED2 blinks N times for slot N. The last slot is remembered across power cycles. |
| Preset persistence | Presets survive switching to another firmware and back. They live in the upper 4 MB of the QSPI flash, outside any firmware image and outside bad_tape's saved values at `0x903FE000`. |
| Hardware | Daisy Seed with 64 MB or more of SDRAM (development unit: Seed 1.2, 64 MB). HotHouse. |
| SysEx | `F0 7D 64 <version> <cmd> … F7`. Device byte `0x64` is unused by sibling projects. Identity Reply and DFU command follow the bad_tape conventions. |
| Editor | Preset manager plus live status: log, CPU load meter, knob/switch states. It does not control the knobs remotely, because the pedal is "what you see is what you get". |
| Model files | `models/*.nam` are git-ignored. |

## Feasibility

| | A2-Lite | A2-Full |
|---|---|---|
| Weights | 1,871 | 12,146 |
| Multiply-adds per second at 48 kHz | ≈ 90 M | ≈ 583 M |
| Estimated load at 480 MHz, 1.2–2 cycles per multiply-add | 22–37 % | 120–200 % |

- Every weight is used about once per sample, so the multiply-add count tracks the weight count. The estimate assumes a hand-written float kernel that keeps each 3×3 tap matrix in registers across a block. This estimate is not yet measured: milestone M3 decides it.
- The model looks back 6,346 samples (132 ms). Its history needs about 76 KB for 3 channels, and its weights 7.5 KB. Both fit in DTCM, the processor's fast internal RAM.
- Reset is cheap: with silent input every layer settles to a constant value per channel. Compute those once at load time and fill the history with them, instead of running 132 ms of silence through the model.
- A direct-form IR costs N multiply-adds per sample. A 2048-tap IR uses uniformly partitioned FFT convolution (CMSIS-DSP `arm_rfft_fast_f32`). The nested `daisy/libDaisy/Drivers/CMSIS-DSP` submodule is currently empty and must be initialised.

## Milestones

```mermaid
graph LR
  M0[M0 Scaffold] --> M2
  M1[M1 Converter + reference outputs] --> M2[M2 Engine on the PC]
  M2 --> M3{M3 Benchmark gate}
  M3 --> M4[M4 Live amp]
  M4 --> M5[M5 IR + filters]
  M5 --> M6[M6 Preset store]
  M6 --> M7[M7 Link protocol]
  M1 --> M8
  M7 --> M8[M8 Editor]
  M8 --> M9[M9 Hardening]
```

### M0: Scaffold, passthrough, DFU
- Same layout as bad_tape:
  - `daisy/Makefile`: `APP_TYPE = BOOT_SRAM`, `BOARD=hothouse`, build id, and `APP=` variants.
  - HotHouse `board.h`.
  - `memory.h` placement macros.
  - Linker script with fast-RAM sections (DTCM, ITCM) and SDRAM sections.
  - Top-level `Makefile` for the PC builds.
- USB-MIDI link: Identity Reply, HELLO, LOG, DFU command. `tools/flash_when_ready.sh`.
- Audio: left input copied to both outputs at 48 kHz / 48 samples. FS1 mutes with a ramp, and LED1 follows. A status log line reports CPU load, underruns, and knob states.
- **Done when:** the pedal flashes over USB-MIDI DFU with no button presses, passthrough is clean, and the Identity Reply parses. Board revision and codec are confirmed and recorded.

### M1: Model converter and reference outputs
- The `.nam` → binary converter is written in TypeScript and shared by the editor and a Bun command-line tool.
- Binary layout: magic, format version, shape descriptor, weights (float32 LE), `head_scale`, `loudness`, `input_level_dbu`, name, CRC-32.
- Reference outputs: run the official NeuralAmpModelerCore on a fixed set of guitar DI recordings and keep the results as test fixtures.
- **Done when:** both development models convert, and models with a non-Lite shape are rejected with a named reason.

### M2: A2-Lite engine on the PC
- Portable C++ that does not include libDaisy: block processing and constant-fill reset.
- `host/render IN.wav OUT.wav --model X.bin`.
- GoogleTest against the reference outputs.
- **Done when:** the maximum error against the reference is about 1e-5, output is the same at any block size, and there are no NaNs or denormals.

### M3: Benchmark gate (go / no-go)
- `APP=bench`: the model is compiled in, and the DWT cycle counter times each layer.
- Optimise: tap matrices held in registers, ITCM code, DTCM history, flush-to-zero.
- **Done when:** model load is ≤ 45 % at a 48-sample block, and device output matches the PC within tolerance.
- **If it fails:** try fixed-point or SIMD variants, a larger block, or ask NAM upstream for a smaller shape.

### M4: Live amp
- The engine runs in the audio callback with one compiled-in model.
- Knobs 1 and 2 are smoothed, and output is level-matched with `loudness`.
- **Done when:** playable, with no zipper noise and no denormal spikes after long silence.

### M5: IR and filters
- Partitioned FFT convolver for IRs up to 2048 taps.
- Cut filters:
  - 6 dB/oct is first-order, so its resonance knob does nothing.
  - 12 dB/oct is one biquad with Q from the resonance knob.
  - 24 dB/oct is two cascaded biquads: a Butterworth stage, then a stage whose Q comes from the resonance knob.
  - The extreme knob position bypasses the filter.
- Mid peaking filter for SW3.
- Spec in `docs/FILTERS.md`; tests on the PC (frequency response, stability under knob sweeps, no clicks when switching slopes).
- **Done when:** the whole chain stays within budget on hardware, measured by `APP=bench` and the live load meter.

### M6: Preset store and switching
- Flash region in the upper 4 MB, clear of the last flash addresses (chip erratum `ES0392 2.7.5`). 8 slots, each holding a header, model, optional IR, and CRC. A small two-copy state record holds the last slot.
- Presets load into RAM. FS2 skips empty slots and wraps; audio fades out, the model is reset, and audio fades back in. LED2 blinks N times.
- The audio callback never reads QSPI, so flash writes cannot stall audio.
- Test data: the development models are written to flash by a command-line tool.
- **Done when:** switching while playing is clean, corrupt slots are skipped, and the pedal recovers if power is pulled during a write.

### M7: Link protocol
- Preset upload in chunks, with an acknowledgement and CRC per chunk. List, erase, rename, and reorder slots.
- Status stream: CPU load (average and maximum), underruns, current slot, knob and switch states, and log lines.
- `docs/MIDI.md` is generated from the protocol header. A command-line tool uploads presets and dumps status.
- **Done when:** all 8 slots can be managed from the command line while audio runs, with no underruns.

### M8: Editor
- Bun + TypeScript + Preact, built to a single HTML file (`editor/dist/editor.html`).
- Imports `.nam` files (checked with the M1 converter) and WAV IRs (resampled, trimmed, normalised).
- Manages 8 slots and shows model metadata, including `input_level_dbu` and gear type.
- Panel showing the log, CPU load meter, and knob/switch states. DFU button.
- **Done when:** a full preset round trip works from the browser, and the editor works when hosted inside a multi-firmware launcher.

### M9: Hardening and release prep
- Stress tests: fast FS2 presses, uploading while playing, power loss during writes, rapid knob sweeps.
- Docs: `README.md`, and `daisy/README.md` with the memory map, measured performance, and log lines.
- Licence review before the history is squashed.

## Risks

- **CPU:** the generic NAM core is built on Eigen, which has no float SIMD on Cortex-M7, so it is assumed too slow. The custom kernel is the plan, and M3 is the gate.
- **Locked model shape:** if NAM revises A2-Lite, the firmware needs an update. The shape descriptor plus the editor's check keep unsupported models from playing incorrectly.
- **QSPI coexistence:** another firmware's image could grow into the preset region. The largest sibling image is about 2.2 MB, and the presets start above 4 MB.
