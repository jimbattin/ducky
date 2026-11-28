# Measurements

What was measured on the pedal and on the bench image: the current figures only. A figure that a later change replaced is not kept here; each milestone's own numbers are in its entry in `docs/HISTORY.md`. Memory use is in `docs/FIRMWARE.md` "Memory".

- **Two units:** the development unit, a Seed 1.2 (PCM3060 codec), and since the firmware audit a Seed 3 (TAC5242 codec); both 64 MB, 48 kHz, 24-sample blocks. A section or row says which. They share the STM32H750, so CPU, flash and switching figures carry from one to the other: the Seed 3's bench came within 10 cycles of the Seed 1.2's on every stage but the head. Codec-dependent figures: the loop's round trip on both ("FX loop"); the loop's crosstalk on the Seed 1.2 only.
- **Load** is the `load` log line's (`docs/FIRMWARE.md` "Log lines"): permille of the block period, average / maximum. The ceiling is 850 (`docs/ROADMAP.md` "Decisions").
- **The builds** are named as they are built now: the xfade build (`APP=xfade`, the default) runs both models and crossfades them, and the weight build (`APP=weight`) blends their weights. Until the crossfade became the default, the weight morph was the default.
- **Before the efficiency pass:** a section marked so holds figures not measured again on the current firmware; its absolute loads are that firmware's, the differences between its rows carry over. A new measurement replaces the section.
- **Elsewhere:** the IR convolver step by step in `docs/FILTERS.md` "Measured (two-level convolver, Seed 1.2, 24 samples)"; the loop's round trip and crosstalk on the Seed 1.2 in `docs/LOOP.md` "Measured (Seed 1.2 / PCM3060)" (the Seed 3's round trip in "FX loop"); the EQ's error and cost in `docs/EQ.md` "5. As built"; the compressors against SPICE in `docs/compressor-spec.md` and `docs/ota-compressor-spec.md` "Agreement with SPICE"; the morph's blends in `docs/MORPH.md` "Measured blends"; the core's cost model in `docs/M7_PERFORMANCE.md`.

## Live load

Seed 1.2, MISO, while playing, compressor in blend, `morph` on Knob 6, 11 to 17 s each:

| Build | Preset | Load avg / max | Underruns |
|---|---|---|---|
| Weight build | Slot 1 → 3 (trained path), no IR | 430 / 434 | 0 |
| Weight build | The Fender model with the 2,048-tap FDMN mix → 5 (straight blend) | 508 / 517 | 0 |
| Xfade build | Slot 1 → 3, no IR | 790 / 798 | 0 |
| Xfade build | The Fender model with the 2,048-tap FDMN mix → 5 | 870 / 880 | 0 |

- **The IR** costs 78 permille live (508 against 430).
- **The xfade build's second engine** and its mix cost 360 permille (790 − 430): the engine is 339 on the bench, and the rest is the two gain ramps and both engines' three rings in AXI SRAM. The load does not depend on the `morph` position.
- **The xfade build with an IR** plays both models with no overrun and no `morph shed`, 20 permille above the 850 ceiling.
- **The IR preset** was uploaded to the empty slot 6 for the measurement and erased after it.
- **The audio callback runs without flush-to-zero:** read in the callback, `FPSCR` is `20000010` and `FPDSCR` 0, while the main loop's `FPSCR` is `21000010`. `dkfw::EnableFlushToZero` sets FZ in the thread's `FPSCR`; an interrupt handler's floating-point context takes its FZ from `FPDSCR`, which nothing sets. The bench runs in the main loop, with FZ. Nothing was changed: the engine flushes its own input (`A2Lite::BeginChunk`), and setting `FPDSCR` would change the callback's arithmetic on subnormals.

Seed 3, compressor in blend, MISO, `morph` at 50 % (on no control), 3–15 s each; the morph preset was uploaded to slots 7 and 8 for the measurement and erased after it:

| Build | Preset | Load avg / max | Underruns |
|---|---|---|---|
| Xfade build | Slot 1, no target, no IR | 435–436 / 439–440 | 0 |
| Weight build | Slot 1, no target, no IR | 427 / 430–431 | 0 |
| Xfade build | The Fender model with `decay.wav` (2,048 taps) → the SSS | 864 / 875 | 0 |
| Weight build | The same (straight blend) | 506 / 514 | 0 |

- **The Seed 3 agrees with the Seed 1.2** within 6 permille on the morph preset with an IR (864 / 875 against 870 / 880; 506 / 514 against 508 / 517); no `morph shed`.

BurningBungalow (a Seed 3), xfade build, MISO, compressor off, slot 1 (no target, no IR), the expression jack read every block: 407 / 411 with PEDAL on none, 408 / 412 with PEDAL on `morph` (writing it every block), 0 underruns, 3–5 s each. Not measured on the BurningBungalow: the morph preset with an IR, the worst case above.

## Morph

The xfade build, Seed 1.2:

- **By ear** (slot 5 → 2, K6 swept by the player): no zipper noise, no dip in level in the middle.
- **The shed** was seen on the image before the efficiency pass, whose two engines did not fit the block with an IR: each of three loads of such a preset counted one underrun and went on at one engine's load, with the link answering throughout.
- **Not captured:** the `morph shed` line itself (a load's shed is over before a log reader attaches, and `usb_log.py` and `pedal.ts` cannot hold the port together). On the Seed 3: a preset with a target and an IR loads in 1,115 µs (`preset 7 … load 1115 us`), and the xfade build on a preset without a target reads 436 / 440 ("Live load").

The weight build, Seed 1.2, before the efficiency pass. Slot 5 (a model, no IR) morphing with slot 2, compressor in blend, MISO, while playing:

| | Load avg | Load max |
|---|---|---|
| `morph` at rest (0 % and 76 %) | 464–471 | 468–475 |
| `morph` swept end to end every 200 ms over the link (a blend on every block) | 502 | 515 |

- **Underruns:** 0, sweeping for 12 s, and across MORPH, SWAP and UPLOAD reloads.
- **Load:** a preset with a morph target loads in about 960 µs (`preset 5 … load 962 us`), one without in 134 µs.
- **Flash:** MORPH took 158–174 ms.
- **Reloads:** `swap 2 7` with slot 5 playing logged `preset 5 … load 962 us` and `morph slot 7 fit 21 21 %`; the swap back, `morph slot 2 …`. An upload of another model over the target logged `morph missing slot 7`.

## FX loop

Seed 1.2, before the efficiency pass. Slot with a model and no IR, compressor off, the send patched to the return (`docs/LOOP.md` "Measured (Seed 1.2 / PCM3060)"), while playing; the mode set with `bun tools/pedal.ts param set loop_mode …`.

| `loop_mode` | Load avg | Load max |
|---|---|---|
| MISO | 419 | 422 |
| pre-model | 422 | 426 |
| post-model | 423 | 427 |

- **Underruns:** 0 in every mode, across mode changes and level sweeps.
- **Settings:** `param set loop_mode post-model` logged `loop post-model`; `param assign sw1 loop_mode` was accepted; after `param save` and a reflash the boot showed `loop post-model`, then SW1, in the middle, took over (`loop pre-model`), as a control does at boot.

Seed 3, the latency image and then the xfade build, the send patched to the return through insert cables, the guitar looper on the input's tip:

| | Seed 3 | Seed 1.2 (`docs/LOOP.md`) |
|---|---|---|
| R Out → R In, the 0.25 FS click | +0.169 FS (+0.1690 to +0.1691 over the 26 trials logged), polarity kept | −0.16 FS, polarity inverted |
| Round trip | 65.72 samples, 1,369 µs, every trial | 82.72 samples, 1,723 µs |
| Pre-model, send and return at 0 dB, compressor off: `rin` against `rout` | equal in 14 of 15 one-second readings (−13 to −18 dBFS), 1 dB low in one | within 1 dB |

- **The input calibration:** the Seed 3's round trip is 0.48 dB above the Seed 1.2's on the click's peak, and 0 dB at the log's 1 dB resolution on the looper. The click's peak also depends on each codec's filters, so the 0.48 dB is within the method's error. The loop measures the DAC and the ADC together; with the Seed 3's datasheet input 0.2 dB from the Seed 1.2's, nothing points to a difference in the input alone. `kInputLevelDbu` stays +4.5 dBu on both units (`docs/FIRMWARE.md` "Input calibration").
- **The round trip** is 2 × 24 + 17.72 samples: the TAC5242's filters add 17 samples less delay than the PCM3060's.

## Compressor

The xfade build, Seed 1.2, slot 6 (a model, no IR), MISO, while playing (input −10 to −18 dBFS); `comp_mode` Direct, Sustain at position 0.29, Balance 50 %, set with `bun tools/pedal.ts param set`.

| | Load avg / max |
|---|---|
| `comp_mode` Off | 763 / 801 |
| OTC | 791 / 798 |
| OTA | 793 / 800 |
| The second holding the switch from OTC to OTA (both run for 20 ms) | 793 / 836 |

- **Underruns:** 0 throughout.
- **The meter:** STATUS `comp cut` read 10.0–12.0 dB with the OTA compressing, and 0.0 with `comp_mode` Off.
- **Readout:** `param get comp_sustain` read `+6.1 dB` with `comp_type` OTC and `29 %` with OTA.
- **Host, OTC:** the kernel against `tools/comp_reference.py`: max abs diff 3.6e-6, 1.8e-6 and 4.4e-6 on the three steps, 5.4e-7 on the DI; `static.csv` worst 0.0000 dB; the static curve against SPICE worst 0.113 dB; the stage response against the spec's table worst 0.083 dB; lowest step gains 4.44, 0.49 and 5.74 dB (SPICE 4.77, 0.53, 5.87).
- **Host, OTA:** the kernel against `tools/ota_reference.py`: max abs diff 4.8e-7, 4.8e-7 and 1.2e-6 on the three steps, 6.0e-7 on the DI; `static.csv` worst 0.00016 mV; the static curve against SPICE worst 0.233 dB.

## Parameters and settings

Seed 1.2 but for SAVE's flash time, measured when the parameter table was built (seven controls, before EXP; the layout of the time had `high_cut_freq` on Knob 4 and `low_cut_freq` on Knob 5). Preset 4 (no IR), audio running throughout; over the link with `bun tools/pedal.ts` and the editor.

| | |
|---|---|
| Parameter step (`stepParams`), DWT cycle counter | 327 cycles a block: the edit queue 37, `Store::Update` 182, publishing 84, the edit count 15 |
| Underruns | 0, through every command below and a 3 s `status` run (30 STATUS lines) |
| WATCH | 16 STATUS and 15 VALUES in 1.5 s |
| Flash time: SAVE (4 KB sector erase, write, readback) | 60–77 ms (Seed 3) |

- **Boot:** `settings factory` and the factory `layout` line on a pedal that had no record.
- **SET:** `param set high_cut_freq 3000` answered `3.00 kHz`, and `param get` showed `K4 (held)`.
- **ASSIGN:** `param assign k3 low_cut_slope` kept the slope's value and marked K3 held; `param assign sw1 low_cut_freq` was refused by the CLI (`SW1 takes only choice parameters`); `param assign k6 none` left `low_cut_res` on no control.
- **SAVE and restart** (a DFU reflash, not a power cycle): `settings seq 1`, `layout k3 low_cut_slope k4 high_cut_freq k5 low_cut_freq k6 none …`; `low_cut_res`, on no control, came back at the saved Q 1.50; `high_cut_freq` went back to Knob 4's position.
- **Not checked in this run:** moving a held knob or switch (the pickup), a choice parameter on a knob by ear, and a power pull during SAVE.

## Link and slot writes

Seed 3, over the link with `bun tools/pedal.ts` on the empty slots 7 and 8, audio running throughout:

| | |
|---|---|
| Upload, wall time (identity check excluded) | 0.2 s with an IR (16,288 bytes) and without (7,996), flash included |
| Upload retries | 0 over 4 uploads |
| Flash time: upload (64 KB erase, write, readback) | 138–151 ms |
| Flash time: erase (64 KB erase, 4 KB sector erase, 0xFF check) | 193–204 ms |
| Flash time: rename, gate, morph | 138–144, 132–146, 145–151 ms |
| Flash time: swap (two slot writes) | 287–288 ms |
| Underruns | 0 |

- **Playback** (Seed 1.2, the earlier run; slot 1 the Fender with the FDMN IR, slot 2 the SSS, slot 4 `A2.nam`): an upload into the playing slot logged one `preset` reload; an upload while dry started playing it; erasing the playing slot moved to the next filled one (1 → 2 → … → 8 as each was erased), and the last erase went dry (`preset none`, `status … slot dry`). SWAP 1 8 with slot 1 playing logged no `preset` line, `list` then marked slot 8 playing, the state record followed (`state slot 8`), and slot 8 played after a restart (a DFU reflash, not a power cycle). On the Seed 3, erasing the playing slot 7 moved to slot 1 (8 empty), and a morph block followed its target through `swap 7 8`.
- **Refusals:** `rename` of an empty slot answers `slot-state`, `swap 2 2` `bad-request`, `upload 9` is a usage error (exit 2).
- **Not checked by ear or eye:** the reload's fade, LED2's blinks after an erase, and the STATUS knob/switch fields following moving controls (they matched the `ctl` line at rest).

## Firmware update

FIRMWARE over the link (`make firmware-check` twice, `pedal.ts firmware`, the editor in headless Chrome; `docs/FIRMWARE.md` "Firmware update"), Seed 3, audio running, slot 1 playing; the images of two builds: xfade 197,560–197,736 bytes, weight 198,104–198,288.

| | |
|---|---|
| Flash time: FIRMWARE COMMIT (4 × 64 KB erase, write, readback) | 700–723 ms, the xfade and the weight build alike |
| `pedal.ts firmware`, wall time (transfer, COMMIT, restart, until the new Identity answers) | 3.7–4.2 s |
| Retries | 0 over 21 updates (17 from `pedal.ts`, 4 from the editor in headless Chrome) |
| Refusals | an incomplete image `incomplete`; a wrong CRC `invalid` 1 (crc); a flipped tag `invalid` 3 (tag); board 2 `invalid` 4 (board); the pedal kept running its image each time |

- **Survives an update:** `pedal.ts list` (names, models, IRs, the playing slot) read the same after every update. In the editor the toolbar showed `LINKED` with the new build after each restart.
- **Load after the update:** the xfade build on slot 1 read 436 / 440 permille, 0 underruns, as in "Live load".
- **Against the Seed 1.2:** its COMMIT took 741–753 ms for the xfade image and 950–961 ms for the weight image, the same four blocks; the Seed 3 shows no such difference. Not explained.
- **Not measured:** underruns during COMMIT itself (the counter restarts with the new image), and a power cut during COMMIT.
- **BurningBungalow:** `BOARD=burningbungalow make firmware-check` passed on a Seed 3 in a BurningBungalow: 6 updates, COMMIT 734–754 ms, 3.7–4.0 s each, 0 retries; the other board's tag (board 1, HotHouse) `invalid` 4 (board). The settings record survived every restart.

## Preset store and switching

Seed 3, switching by MIDI CC 102 over the link (as FS2 switches), while playing:

| | |
|---|---|
| Preset load | 132–141 µs without an IR; 975 µs with a 2,048-tap IR (the two-level convolver); with a morph target and an IR, 1,115 µs (xfade build: a second `A2Lite::Load`) and 1,845 µs (weight build: `dkmorph::Prepare`) |
| State write (4 KB erase, 16 B write, readback) | 64–73 ms |
| Underruns | 0 over about 300 switches |

- **CC 102:** 30 CCs alternating two slots 200, 50 and 20 ms apart gave 29 (the first named the playing slot), 30 and 30 switches; 5 and 1 ms apart, 55 and 39 of 60: CCs that arrive during a switch move its target, as fast FS2 presses do. 64 CCs in one write ended on the last one's slot. The playing slot's own number, an empty slot (`preset cc 5 ignored: slot 6 empty`), a value of 8 (`preset cc 8 ignored`), another CC and another channel than EXP CHANNEL's changed nothing. A CC to slot 8 followed at once by ERASE 8 stayed on the playing slot. The slot a CC chose was the boot slot after a restart, and a saved EXP CHANNEL of 5 still selected after one.
- **In the loop modes:** CC switches in pre-model and post-model read 438 / 442, 0 underruns.
- **Switching with FS2** (Seed 1.2, the earlier run): LED2 blinks the slot number, FS2 skips the empty slot 3 and wraps, and no switch clicks. The saved slot survives a power cycle.
- **Corrupt slot** (Seed 1.2): a byte flipped in slot 2 logs `slots ok crc empty ok …`; the saved slot 2 falls back to slot 1, and FS2 goes 1 → 4 → 1.
- **Power loss during a bank write** (Seed 1.2): power was pulled at 84 % of dfu-util's erase pass. The pedal booted with `slots empty …` and played dry, and the next bank flash restored all three slots.

## Bench

`APP=bench` (`docs/FIRMWARE.md` "Bench"), Seed 3, two builds (the firmware audit and the one after it; the bench's code is the same in both), 12 passes: cycles per block of noise, and the worst pass's maximum in permille of the block period (240,000 cycles). Across the passes the averages moved by at most 12 cycles. The audit's code before and after, run back to back on the Seed 3, differed by at most 0.4 % on any stage (the head), and the Seed 3 matched the Seed 1.2's last run to within 10 cycles on every stage but the head (2,322 there).

| Stage | Avg cycles | Max permille |
|---|---|---|
| Whole model (`a2lite`) | 81,302 | 341.8 |
| Input (`input`) | 551 | 2.3 |
| A kernel-6 layer (`layer 04`) | 3,131 | 13.4 |
| A kernel-15 layer (`layer 14`) | 6,313 | 26.7 |
| The last layer (`layer 22`, no residual) | 2,718 | 11.4 |
| Head (`head`) | 2,331 | 9.7 |
| IR convolver, all in D2 SRAM (`ir d2`) | 22,131 | 107.7 |
| IR convolver, the pedal's placement (`ir axi`) | 18,459 | 82.0 |
| EQ (`eq`) | 14,163 | 59.2 |
| Noise gate (`noise gate`) | 235 | 1.2 |
| Compressor, OTC (`comp`) | 7,111 | 30.8 |
| Compressor, OTA (`comp ota`) | 7,544 | 32.5 |
| Morph (`morph`) | 7,573 | 32.0 |
| Whole chain (`chain`, both compressors on) | 140,888 | 594.5 |

- **Gate:** go on every pass, 594.5 permille at most against 850; the model 341.8 against 450. The engine has no data-dependent branch, so its cost does not depend on the signal. The chain runs both compressors on every block, as the pedal does for 20 ms after a `comp_type` switch; playing, the pedal runs one.
- **Checks:** on every pass the engine 241e-9 against the reference render (FMA contraction; the host build is bit-exact). The value has been the same for every engine version: only the order of instructions changed, never the arithmetic. The IR check 178e-9 against a direct convolution in double.
- **The IR convolver:** step by step in `docs/FILTERS.md` "Measured (two-level convolver, Seed 1.2, 24 samples)".
- **The engine**, whole model, each row adding to the one above (`docs/M7_PERFORMANCE.md` "Case study"):

  | Change | Avg cycles |
  |---|---|
  | Before the efficiency pass | 86,984 |
  | The rings outside the object (`Bind`) | 87,034 |
  | The head sums' history moved by float copies, not `memcpy` (a byte loop the bench's constant block size let GCC call; the pedal's build kept a loop) | 86,492 |
  | The residual's three outputs as straight-line runs, weights pinned | 85,813 |
  | The head sums: 6 loads, then each add with its store | 83,332 |
  | The residual's sums the same way | 81,437 |
  | The input's subnormal flush on the bits | 81,302 |

- **Audit of per-block work** (each bit-exact, host tests unchanged):
  - `Peak` as four running maxima; MISO's right output peak taken from the left's (the two are bit-identical).
  - The mute, the preset fade and the mode fade folded into one multiply while all three rest (none at gain 1); MISO's copy to R Out by `CopyFloats`.
  - `GainRamp` without the per-sample ramp when the gain rests.
  - The noise gate's detector as a deque (`docs/FILTERS.md`).
  - A cut filter's coefficients (a `tanf` and two `powf`), a bell's shape (two `exp2` and a `tan` in double), the compressor's targets and its Sustain conductance (a `powf` each) and the crossfade's `cosf` and `sinf` recomputed only when their inputs change. A parameter on a knob wanders with the ADC, so its own design still runs on most blocks: with a bell's gain on a knob (the factory layout's Knob 3) the bell's shape is what is saved.
  - Every `memcpy`, `memmove` and `std::copy` on the audio path replaced by `CopyFloats` (`docs/M7_PERFORMANCE.md` "Copies"); what is left is the parked block's `std::fill`.
- **The compressor's maximum** is its worst case (settings flipping every 10 blocks, so the cached targets never hold): the audit raised it 0.8 permille while its average fell.
- **The morph in the chain** cost about 13 k cycles, 5 k more than alone (before the efficiency pass; the bench does not time it apart from the chain). Not investigated; the blend reads its 15 KB of weights from AXI SRAM through the same 16 KB D-cache the convolver's spectra stream through. One segment's two weight sets (14,960 bytes) would need about 5 KB more DTCM than the weight build has free (`docs/FIRMWARE.md` "Memory"); `gMorphW`'s four segments do not fit either.
- **Not taken:** CMSIS-DSP's FFT in ITCM (the same on the bench, 1 permille lower live); flush-to-zero for the engine's input (the callback runs without FZ, "Live load"); the FFT's tables in DTCM (3 KB of the 5.8 KB the xfade build has free, for an FFT that measures within 500 cycles of its cost alone).
- **Image:** bench DTCM 120,752 bytes; SRAM 455,876; D2 SRAM 49,760; ITCM 26,024.
