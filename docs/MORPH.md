# Preset morph

A preset can name another preset as its **morph target**. The global `morph` parameter (0–100 %, on a knob or set from the editor) then moves the pedal from the playing preset's NAM model to the target's: at 0 % the pedal plays the preset's own model, at 100 % the target's. The preset's own IR (or none) and its noise gate stay through the whole sweep.

The pedal's xfade build runs both models and crossfades their outputs ("On the pedal: both models at once" below). A second build, the weight build (`make -C daisy APP=weight`), runs one model instead and blends the preset's model into the target's weight by weight, in between a network on a path between the two: a straight line between the two weight sets, or a **trained path** through up to three waypoint networks, trained on the host so that the sweep follows a crossfade of the two amps; the input calibration and the level match move between the two models' values in dB. A blend costs at most 32 permille of the block period while the knob moves and nothing while it rests, with or without waypoints, where the second model and its mix cost about 360 on every block ("On the pedal: the weight morph"). The host's analysis, its fit and the trained paths below are the weight build's: the xfade build keeps them in the slot as stored and does not use them.

Code: `daisy/src/morph.h` (`dkmorph`), `daisy/src/dsp/a2lite.h` (`A2Lite::Blend`, `ToEngine`), `daisy/src/preset.h` (the slot's morph block and waypoints, `SetMorph`, `ResolveMorph`), `daisy/src/ducky.cpp` (`stepMorph`, `loadPreset`, the link's MORPH), `editor/src/morph/` (the analysis, `save.ts`), `editor/src/preset/morph.ts` (the block, the waypoints, the path file), `tools/morph_train.py` (the trainer). Tests: `tests/morph_test.cpp`, `tests/preset_test.cpp`, `tests/upload_test.cpp`, `editor/src/morph/*.test.ts`, `editor/src/preset/morph.test.ts`, `editor/src/draft.test.ts`, `make morph-check`.

The research behind the design (what was tried, the measurements, what is still open and worth exploring): `docs/morph-findings.md`.

## Using it

```
bun tools/pedal.ts morph 5 2            # slot 5 morphs with slot 2, a straight blend: reads both models, analyses, stores the block in slot 5
bun tools/pedal.ts morph 5 2 --train    # the same along a trained path: trains three waypoints (about ten minutes), stores them in slot 5
bun tools/pedal.ts morph 5 2 --path F   # stores a path file trained earlier (tools/morph_train.py -o F)
bun tools/pedal.ts morph 5 none         # no morph
bun tools/pedal.ts param assign k6 morph && bun tools/pedal.ts param save
bun tools/pedal.ts info 5               # … morph slot 2 fit 88.7 % unaligned 31.4 % waypoints 3
```

In the editor: the Presets tab's **Morph to** menu and its **Morph** box (`docs/EDITOR.md`). The editor stores straight blends; it shows a trained path and keeps it. The xfade build plays any of them as the same crossfade of the two models: a straight blend, a path and their fits change only what the weight build plays.

- **The target is a slot, held by its model.** The block in slot A stores the target's slot number and the CRC of the target's model binary. The pedal morphs with the stored slot while it holds that model, else with the first other slot that does (so a SWAP keeps the pair), else not at all (`morph missing slot N` in the log). B's weights are read from its slot: nothing of B is copied into A. The waypoints of a trained path are stored in A's slot.
- **Replace the target's model and the morph stops** until it is analysed or trained again. The editor analyses again itself when it saves a new model over a target (a straight blend: a trained path was trained for the old model); `tools/pedal.ts upload` does not: run `morph A B` again.
- **A new model in A drops its morph.** `tools/pedal.ts upload` always writes a preset without one. The editor keeps the morph, waypoints included, across a save that keeps A's model (a rename, an IR change), and analyses again after one that replaces it.
- **MORPH stores a straight blend.** Picking a target in the editor, or `morph A B` without `--train`, replaces a trained path.
- **The parameter is global,** like the EQ's: one `morph` position for whichever preset plays. A preset without a target ignores it.
- **The ends are exact:** the parameter's first and last 2 % are the two models alone (`dkmorph::kEndZone`), so a knob that stops short of its travel still reaches both. A trained path does not change them.

## What a blend is

An A2-Lite network (`docs/NAM_FORMAT.md` "Forward pass") is 1,870 weights and a head scale. A straight blend at position t is the network with weights `(1 − t)·A + t·B′`, where A has its head scale folded into its head and B′ is B likewise, re-expressed by the block's transform. The engine keeps running through a blend: its histories are kept, which is sound because the networks along the path share their coordinates.

**The transform.** A network computes the same output when

- its hidden channels (the activations `a`, summed into the head) are permuted and scaled by positive factors, `a′ = G a` with `G[perm[j]][j] = scale[j]` (a leaky ReLU commutes with a positive scale), and
- its residual path (the layer inputs `x`) goes through any invertible 3×3 matrix, `x′ = M x`.

The weights that do it, per layer and tap: `r′ = M r`, `W′ = G W M⁻¹`, `cb′ = G cb`, `m′ = G m`, `L′ = M L G⁻¹`, `lb′ = M lb`; per head tap `H′ = s H G⁻¹` and `hb′ = s hb`, with the head scale `s` folded in. `dkmorph::Apply` (C++, in double) and `applyTransform` (TypeScript) are the two implementations; `tests/fixtures/morph/` pins them together: `A2_shuffled.bin` is `A2.bin` put through such a transform, and `A2_shuffled.morph` the block the analysis finds to bring it back (`tools/make_morph_fixture.ts`).

That is every symmetry the shape has: the head is shared by all 23 layers, so the hidden channels can only be permuted and scaled alike in every layer, and the residual path is one stream. It is twelve numbers and a permutation, which is why aligning two captures of different amps cannot make the straight line between them a good morph (`docs/morph-findings.md`), and why the path has waypoints instead.

**The path.** With K waypoints (0–3) the nodes are A, the waypoints in stored order, and B′: K + 1 straight segments of equal length over the blend position. At position t the segment is `s = min(⌊t·(K + 1)⌋, K)` and the weights are `node_s + f·(node_{s+1} − node_s)` with `f = t·(K + 1) − s` (`dkmorph::SegmentAt`). A waypoint is a whole network: 1,870 weights in A's coordinates, head scale folded in. Three waypoints sit at 25, 50 and 75 %. K = 0 is the straight blend. The level match and the input calibration blend in dB over t, not per segment. `tests/fixtures/morph/A2_path.preset` pins the waypoints' layout across the two languages: its three waypoints are A2 in other coordinates, so the pedal's code renders A2 at every knot of the editor's image and another network between two (`MorphPath.FixturePresetRendersA2AtEveryKnot`).

## The fit

How closely a morph follows a crossfade of the two amps, as one number, 0…1, stored in the block for display:

- **Stimulus** (`editor/src/morph/stimulus.ts`): 2 s of eight Karplus–Strong plucks across the guitar's range, loud (0.5) and quiet (0.125) in turn, from a fixed seed. Each model runs it at its own input calibration.
- **Blend error** at a position p: the blend is rendered as the pedal plays it and compared with the crossfade `(1 − p)·yA + p·yB` of the two models' own level-matched outputs, means removed. The error energy relative to the crossfade's: 0 for a blend that is the crossfade, 1 for silence.
- **Fit** = 1 − the mean blend error at the eighths (12.5 … 87.5 %: the knots of a three-waypoint path and the middles of its segments), held to 0…1.

`blendError` (`editor/src/morph/align.ts`, float64 `A2Net`) and `tools/morph_train.py` compute the same number: `make morph-check` compares the trainer's with the one the editor's code stored in `A2_shuffled.morph`. The crossfade is a stand-in for a good morph and the fit is blind to DC and to anything the stimulus does not excite; the trainer reports more (below).

## The analysis (host): a straight blend

`fitAlignment(a, b)` in `editor/src/morph/align.ts`, run by `saveMorph` (`editor/src/morph/save.ts`) for the editor and `tools/pedal.ts morph`. It reads both models back from the pedal (READ), so it needs no files, and takes two or three seconds.

1. **Candidate alignment:** the permutation and scales that bring B's activations closest to A's on the stimulus, and the matrix that brings B's layer inputs closest to A's (least squares over every layer and sample).
2. **The test.** The fit is measured for the candidate and for the identity (no alignment). **The candidate is kept only when its fit is higher.**
3. **The block** stores the transform kept, its fit and the identity's fit.

**Why the test is there.** Captures of different amps do not correspond channel for channel: on the pedal's five models below, B's channels explain under 10 % of the variance of A's activations whatever the permutation, so the candidate is fitted to noise (and to the channels' DC offsets). Blending with it is far worse than blending unaligned: Fender → Two-Rock at 50 % plays at −50 dB with it, −29 dB without. The candidate wins where a model is another's copy in other coordinates, as the fixture is.

## Host setup for a trained path

A trained path is computed on the host by a command-line tool: there is no server or service to install or keep running, and the pedal needs the host only while a path is trained and stored. A straight blend needs none of this. On a new machine (checked in a clean Debian 12 container, x86-64):

1. **Linux, `uv` and Bun,** with the udev rules and the MIDI port's access of `docs/FIRMWARE.md` "Getting started" step 1, and a clone of this repo. Python is not needed: `uv` fetches its own. The trainer does not use the repo's `.venv` (`uv sync`), and `tools/pedal.ts` runs without the editor's dependencies (`make -C editor`).
2. **The trainer's environment:** `make morph-check`. The first run needs the network and downloads Python, PyTorch (its CPU build) and NumPy into `uv`'s cache, about 1 GB; later runs use the cache. It ends with two lines, `A2.bin on input.wav: within 1.43e-07 of A2.input.ref.wav` and `A2 into A2_shuffled, unaligned: fit 0.181024, the fixture block stores 0.181024`.
3. **A guitar DI recording** at `renders/stimuli/guitar_di.wav` (create the directory; `renders/` is git-ignored): a dry guitar as it would reach the pedal's input, normalised, as a 48 kHz WAV, 16- or 24-bit PCM or 32-bit float; its first channel is read. The figures in this document used 20 s. `tools/make_stimuli.py` writes this repo's own from a recording that is not distributed, so on another machine record one. Without the file the trainer says `on plucks only: … not found`, and the path is worse on real playing ("Train with a DI" below). `pedal.ts morph … --train` reads that path only; `morph_train.py --di FILE` takes another.
4. **The pedal:** the weight build flashed ("On the pedal: the weight morph"), the two presets' models in their slots, and the MIDI port free (the editor's tab closed: `docs/FIRMWARE.md` "Port conflicts").
5. **Train:** `bun tools/pedal.ts morph 5 2 --train` ("Using it").

## The trained path (host)

`tools/morph_train.py`, run under `uv` (PyTorch on the CPU; the first run downloads it: "Host setup for a trained path") by `tools/pedal.ts morph A B --train`, or by hand on two model binaries:

```
uv run tools/morph_train.py A.bin B.bin                 # measure the straight blend
uv run tools/morph_train.py A.bin B.bin -o AB.path      # train three waypoints, measure, write the path file
uv run tools/morph_train.py A.bin B.bin --path AB.path  # measure a path file
make morph-check                                        # its network and fit against tests/fixtures
```

- **What it trains.** The waypoints start on the straight line (at 25, 50 and 75 % of it) and are moved by gradient descent (Adam, 8,000 steps, a cosine learning-rate decay from 1e-3) so that the path's blend at every position renders the crossfade of the two models' outputs at that position. Each step renders six 171 ms excerpts, each at its own position, spread evenly over the path. The two ends are not trained: they stay the two models.
- **The loss,** per excerpt, has two parts. The error with its mean removed, over the excerpt's own crossfade's AC energy (plus 5 % of its signal's mean, so silence does not divide by zero): quiet passages weigh nearly as much as loud ones, and a level error counts as an error. And the error's mean squared, over the loudest signal's mean energy: a DC offset counts by its size, whatever is played. The path therefore needs neither a make-up gain nor a DC correction.
- **Signals.** Synthetic plucks (single notes and chords, other notes and noise than the fit's stimulus, at four levels) and, when it exists, a guitar DI recording (`--di`, default `renders/stimuli/guitar_di.wav`: "Host setup for a trained path" step 3): its first three quarters at four gains from 0 to −28 dB, a normalised recording down to a quiet guitar at the pedal's input. Its last quarter is never trained on.
- **Train with a DI.** A path trained on the plucks alone is still far better than the straight blend on real playing, but clearly worse than one that saw a DI, and its stored fit flatters it, since the fit's stimulus is plucks too (Mesa → Fender, with an earlier loss: 81 % on held-out DI against 95 % with the DI in training, while the stored fit read 96 %; `docs/morph-findings.md` finding 12).
- **What it reports,** for the straight blend and the path, on the fit's stimulus and on the DI's held-out quarter at 0, −12 and −24 dB: the fit, and over the fifteen sixteenths of the sweep the worst blend error, the worst level error (the blend's AC RMS against the crossfade's), the worst DC and the worst shape error (1 − correlation², what a perfect gain would leave).
- **The path file** it writes carries the two models' CRCs, the path's fit, the straight blend's fit and the waypoints. `saveMorphPath` (`editor/src/morph/save.ts`) checks the CRCs against the two slots, reads slot A back, and uploads its preset again with a block (the identity transform, the two fits) and the waypoints.
- **`--train` keeps the better blend.** It runs the analysis too, and stores the analysis' straight blend when that fits at least as well as the path: a model that is another's copy in other coordinates aligns exactly, which no training improves on.
- **Time:** 8 to 11 minutes for a pair on a four-core laptop (i7-1185G7), all but a minute of it the 8,000 steps; storing the path takes 0.4 s. Several pairs train faster side by side, one thread each (`OMP_NUM_THREADS=1`): four took 23.5 minutes together. `--steps` shortens a run; fewer than 8,000 left an earlier loss short of its result, and the final one was not tried with fewer. Where the time goes (the gradient of the convolutions, half of every step) and what did not help: `docs/morph-findings.md` finding 16.

## Measured blends (the pedal's five models)

The fit of the straight unaligned blend (what the analysis keeps for all of these) and of a trained path (three waypoints, the trainer's defaults), on the fit's stimulus and on the held-out quarter of the guitar DI at three levels:

| A → B | Stimulus (the stored fit): straight → path | DI: straight → path | DI −12 dB | DI −24 dB |
|---|---|---|---|---|
| 2 Fender Super Reverb (edge) → 1 Two-Rock SSS (clean) | 83.6 → 98.3 % | 75.2 → 99.0 % | 71.5 → 99.6 % | 74.2 → 99.8 % |
| 2 Fender Super Reverb → 3 Vox AC30 (edge) | 85.7 → 98.2 % | 75.2 → 99.0 % | 64.4 → 99.6 % | 59.6 → 99.9 % |
| 1 Two-Rock SSS → 3 Vox AC30 | 82.3 → 96.9 % | 64.5 → 98.4 % | 57.1 → 99.1 % | 49.9 → 99.6 % |
| 4 Marshall JCM800 (lead) → 1 Two-Rock SSS | 80.4 → 96.1 % | 64.7 → 96.8 % | 54.1 → 97.9 % | 34.1 → 98.0 % |
| 5 Mesa Dual Rectifier (red, OD808) → 2 Fender Super Reverb | 31.4 → 88.7 % | 44.4 → 94.4 % | 29.5 → 96.2 % | 14.7 → 95.0 % |
| 5 Mesa Dual Rectifier → 4 Marshall JCM800 | 31.9 → 93.1 % | 21.6 → 92.0 % | 10.8 → 95.0 % | 4.2 → 96.9 % |

The worst level error (the blend's level against the crossfade's, over the fifteen sixteenths of the sweep and the four signals) and the worst DC:

| A → B | Level, straight | Level, path | DC, straight | DC, path |
|---|---|---|---|---|
| 2 → 1 | −9.9 dB | −0.8 dB | −27 dB | −50 dB |
| 2 → 3 | −12.3 dB | −0.4 dB | −30 dB | −59 dB |
| 1 → 3 | −14.3 dB | −1.0 dB | −30 dB | −51 dB |
| 4 → 1 | −17.1 dB | −1.9 dB | −29 dB | −47 dB |
| 5 → 2 | −31.3 dB | −1.7 dB | −27 dB | −49 dB |
| 5 → 4 | −36.5 dB | −0.7 dB | −37 dB | −48 dB |

- **A straight blend** of two of the cleaner captures follows the crossfade on the stimulus (fit 80–86 %) with a level dip of up to 5 dB, and less well on real playing, worse the quieter it is (fit 34–75 %, dips of 4–17 dB). With the Mesa capture the middle of the sweep is some other network, 13–36 dB down and unlike either amp. Both ends are exact. The dip is a property of the weight blend, not of the gains: the level match and the input calibration already move in dB.
- **A trained path** follows the crossfade within 4 % of its energy for the four cleaner pairs (fit 96–99.9 %) and within 12 % for the Mesa pairs (89–97 %), at every level tried, with the level within 2 dB and no DC to speak of.
- **Two trainings of one pair agree within a point:** 5 → 2 trained again on one thread instead of four read 89.3 % and 94.7 / 96.6 / 95.7 %.
- **The crossfade is quieter in the middle itself** (two amps' outputs are only partly in phase: up to 4 dB for 5 → 2), and a path that follows it is too.
- **By ear,** the player tried the paths on the pedal and approved them, without recording detail. On the pedal the 5 → 2 path's level along the sweep reads as its render's ("On the pedal: the weight morph"). The paths of this table are local files (the models are not committed).

Not built (`docs/morph-findings.md` has the measurements behind the design and a longer list of avenues, starting with listening and with captures of one amp at several settings, which were not available to test):

- **Training in the editor.** It needs gradients through the network, which the editor's float64 `A2Net` does not give.
- **Real captures as the targets between the ends** (a capture of the amp at a middle setting as what a waypoint renders) and **a crossfade target that does not dip.**

## On the pedal: both models at once (the xfade build)

The playing preset's model and its target's both run, and `morph` crossfades their outputs (equal power on the square of the blend position; the crossfade a trained path follows is the linear one). Code: `daisy/src/ducky.cpp` under `DK_MORPH_XFADE` (`stepMorph`, `runXfade`, `loadPreset`); `docs/FIRMWARE.md` "Threads". It is `APP=xfade` (`daisy/build/hothouse-xfade/ducky_xfade.bin`); before it became the default it was an experiment image, kept beside the weight morph so the two could be compared by flashing either.

- **The pairing is the preset's morph target** (`morph N M`, the editor's Morph to), resolved as for the weight morph. The block's transform and fits and the slot's waypoints are not used and stay as stored, so the weight build finds its paths again.
- **The mix is equal power on t²:** model A's output times `level_A · cos(πt²/2)` plus model B's times `level_B · sin(πt²/2)`, t the blend position (the same 2 % end zones, so the ends are each model alone). Squaring t makes the target fade in slowly: B is −36 dB at 10 % of the travel, −20 dB at 25 %, −8 dB at 50 %, −2 dB at 75 % (A −0.7 dB at 50 %, −4 dB at 75 %, −11 dB at 90 %); it replaced `cos(πt/2)`/`sin(πt/2)`, where B was already −8 dB at 25 %. Each model keeps its own level match and its own input calibration: B's input is A's (Knob 1 times A's calibration) times `cal_B / cal_A`. The IR, the gate and the loop routing are the playing preset's.
- **Both engines run on every block** of a preset with a target, at every position: the load is constant, and B's history is current when the knob leaves an end.
- **It fits the block, with an IR too, above the 850 ceiling.** Both engines live in DTCM but for each one's three largest rings (`docs/FIRMWARE.md` "Memory"). Slot 1 → 3 without an IR reads 790 permille avg, 798 max with the compressor in blend (430 for the weight morph); the Fender model with a 2,048-tap IR reads 870, 880; no underruns (`docs/MEASUREMENTS.md` "Live load"). Before that pass the second engine was wholly in AXI SRAM: 885 without an IR, and an overrun with one. An overrun stops model B until the next preset load: model A plays on alone at its own level (a step in level), and the log says `morph shed` (`docs/MEASUREMENTS.md` "Morph").
- **By ear** on slot 5 → 2: the sweep goes from the Mesa to the Fender with no zipper noise and no dip in the middle.

## On the pedal: the weight morph (`APP=weight`)

```
make -C daisy APP=weight -j
tools/flash_when_ready.sh daisy/build/hothouse-weight/ducky_weight.bin
make -C daisy -j && tools/flash_when_ready.sh      # back to the xfade build, both models at once
```

- **Load** (`loadPreset`, while the callback is parked): `ResolveMorph` finds the target; `dkmorph::Prepare` folds A's head scale in, applies the block's transform to B, puts every node in the engine's weight layout and keeps, per segment, its first node and the step to its second (`gMorphW`, room for four segments: 60 KB in AXI SRAM); the two models' level matches and input calibrations go to the callback in dB. The time it takes: `docs/FIRMWARE.md` "Switching cost".
- **Every block** (`stepMorph`): the `morph` parameter is smoothed (20 ms, as the knobs are) and mapped to the blend position (`BlendPosition`: the 2 % end zones, linear between). When the position has moved more than 0.001 (or reaches an end), `SegmentAt` finds its segment and `A2Lite::Blend` writes `node_s + f·(node_{s+1} − node_s)` into the engine's weights; the level match and input calibration are set to `10^(((1 − t)·dB_A + t·dB_B) / 20)`. The first block after a load blends, so a preset loads at the knob's position.
- **A parked or dry pedal, and a preset without a target,** skip the blend; the smoother still runs.
- **Reloads:** MORPH on the playing slot reloads it (the preset fade), and so does the upload that stores a trained path. So does anything that changes which slot its block resolves to: an upload over, an erase or a swap of the target (`servePresets` compares `ResolveMorph` with the slot the loaded preset was prepared with).
- **Cost:** `Blend` is 155 islands of 12 loads + 12 loads + 12 multiply-adds + 12 stores (`docs/M7_PERFORMANCE.md`). A path blends one segment, the same 15 KB of weights. Bench and live figures: `docs/MEASUREMENTS.md` "Bench" (`morph`) and "Morph".
- **What the pedal plays is what the trainer measured.** A slot's waypoints read back byte for byte the path file's, and a slot read back with three waypoints, prepared and rendered by the firmware's code on the host, is within 1.2e-5 of the trainer's network at every eighth of the sweep (2 s of DI). With a steady signal at the input (peaks of −18 dBFS) and slot 5 → 2 playing, the output's peak along the trained path falls from the Mesa's level to the Fender's as the render says it should, within 2.5 dB at every eighth (−6.8 dB at 50 %), where the straight blend falls 29 dB in the middle (`docs/morph-findings.md` finding 19).
- **On the device:** `--train` and `--path`, the link's commands on a slot with waypoints (rename, gate, swap, an upload over the target, `morph N none`) and the editor's handling of one have all been run against the pedal (finding 19). The player tried the stored paths and approved them; the listening checks one by one are in `docs/morph-findings.md` "To do on the pedal".

## Formats

The morph block, 64 bytes little-endian, in a slot after the model and the IR (`docs/FIRMWARE.md` "Preset store"; MORPH in `docs/MIDI.md` carries it):

| Offset | Field |
|---|---|
| 0 | u8 target slot (0–7) |
| 1 | u8 perm[3]: B's hidden channel j becomes channel perm[j] |
| 4 | u32 the target model binary's CRC field (its offset 7904) |
| 8 | f32 scale[3], each in 1/16…16 |
| 20 | f32 m[9], row-major, `|det| ≥ 1e-6` |
| 56 | f32 fit (0…1): of the blend the weight build plays, straight or along the path |
| 60 | f32 unaligned fit (0…1): of the straight blend without alignment; equal to the fit when that is what the weight build plays |

`dkmorph::ParseBlock` rejects a block `Apply` cannot use (the slot then reads `morph`); the fits are for display only. A trained path's block holds the identity transform.

**The waypoints** follow the block: K of them (the slot's flag bits 2–3, 0–3), 7,480 bytes each: 1,870 little-endian f32, a network's weight stream in the model binary's weight order with the head scale folded into the head, in A's coordinates (what `Apply` gives A itself). A float that is not finite makes the slot read `morph`. A slot with three waypoints is 30,500 bytes, 38,792 with an IR, of its 65,536. MORPH never carries waypoints: they arrive in an UPLOAD of the whole preset, and MORPH removes them.

**The path file** (`tools/morph_train.py -o`, `decodePathFile` in `editor/src/preset/morph.ts`), little-endian:

| Offset | Field |
|---|---|
| 0 | `"DKMP"` |
| 4 | u32 version 1 |
| 8 | u32 K, the waypoint count (1–3) |
| 12 | u32 model A's CRC field |
| 16 | u32 model B's CRC field |
| 20 | f32 the path's fit (0…1) |
| 24 | f32 the straight unaligned blend's fit (0…1) |
| 28 | K waypoints, as in the slot |
