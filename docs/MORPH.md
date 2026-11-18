# Preset morph

A preset can name another preset as its **morph target**. The global `morph` parameter (0–100 %, on a knob or set from the editor) then blends the playing preset's NAM model into the target's, weight by weight: at 0 % the pedal plays the preset's own model, at 100 % the target's, and in between a network whose every weight lies between the two. The preset's own IR (or none) and its noise gate stay through the whole sweep; the input calibration and the level match move between the two models' values in dB.

One model runs, not two: a blend costs 3 % of the block period while the knob moves and nothing while it rests.

Code: `daisy/src/morph.h` (`dkmorph`), `daisy/src/dsp/a2lite.h` (`A2Lite::Blend`, `ToEngine`), `daisy/src/preset.h` (the slot's morph block, `SetMorph`, `ResolveMorph`), `daisy/src/ducky.cpp` (`stepMorph`, `loadPreset`, the link's MORPH), `editor/src/morph/` (the analysis), `editor/src/preset/morph.ts` (the block). Tests: `tests/morph_test.cpp`, `tests/preset_test.cpp`, `editor/src/morph/*.test.ts`, `editor/src/draft.test.ts`.

The research behind the design (what was tried, the measurements, what is still open and worth exploring): `docs/morph-findings.md`.

## Using it

```
bun tools/pedal.ts morph 5 2            # slot 5 morphs with slot 2: reads both models, analyses, stores the result in slot 5
bun tools/pedal.ts morph 5 none         # no morph
bun tools/pedal.ts param assign k6 morph && bun tools/pedal.ts param save
bun tools/pedal.ts info 5               # … morph slot 2 fit 20.8 % unaligned 20.8 %
```

In the editor: the Presets tab's **Morph to** menu and its **Morph** box (`docs/EDITOR.md`).

- **The target is a slot, held by its model.** The block in slot A stores the target's slot number and the CRC of the target's model binary. The pedal morphs with the stored slot while it holds that model, else with the first other slot that does (so a SWAP keeps the pair), else not at all (`morph missing slot N` in the log). B's weights are read from its slot: nothing of B is copied into A.
- **Replace the target's model and the morph stops** until it is analysed again. The editor does that itself when it saves a new model over a target; `tools/pedal.ts upload` does not: run `morph A B` again.
- **An upload over A drops its morph** (an upload never carries the block). The editor analyses again after such a save; the CLI does not.
- **The parameter is global,** like the EQ's: one `morph` position for whichever preset plays. A preset without a target ignores it.
- **The ends are exact:** the parameter's first and last 2 % are the two models alone (`dkmorph::kEndZone`), so a knob that stops short of its travel still reaches both.

## What a blend is

An A2-Lite network (`docs/NAM_FORMAT.md` "Forward pass") is 1,870 weights and a head scale. A blend at position t is the network with weights `(1 − t)·A + t·B′`, where A has its head scale folded into its head and B′ is B likewise, re-expressed by the block's transform. The engine keeps running through a blend: its histories are kept, which is sound because the blended networks share their coordinates.

**The transform.** A network computes the same output when

- its hidden channels (the activations `a`, summed into the head) are permuted and scaled by positive factors, `a′ = G a` with `G[perm[j]][j] = scale[j]` (a leaky ReLU commutes with a positive scale), and
- its residual path (the layer inputs `x`) goes through any invertible 3×3 matrix, `x′ = M x`.

The weights that do it, per layer and tap: `r′ = M r`, `W′ = G W M⁻¹`, `cb′ = G cb`, `m′ = G m`, `L′ = M L G⁻¹`, `lb′ = M lb`; per head tap `H′ = s H G⁻¹` and `hb′ = s hb`, with the head scale `s` folded in. `dkmorph::Apply` (C++, in double) and `applyTransform` (TypeScript) are the two implementations; `tests/fixtures/morph/` pins them together: `A2_shuffled.bin` is `A2.bin` put through such a transform, and `A2_shuffled.morph` the block the analysis finds to bring it back (`tools/make_morph_fixture.ts`).

That is every symmetry the shape has: the head is shared by all 23 layers, so the hidden channels can only be permuted and scaled alike in every layer, and the residual path is one stream.

## The analysis (host)

`fitAlignment(a, b)` in `editor/src/morph/align.ts`, run by `saveMorph` (`editor/src/morph/save.ts`) for the editor and `tools/pedal.ts morph`. It reads both models back from the pedal (READ), so it needs no files, and takes a second or two.

1. **Stimulus** (`stimulus.ts`): 2 s of eight Karplus–Strong plucks across the guitar's range, loud (0.5) and quiet (0.125) in turn, from a fixed seed. Each model runs it at its own input calibration, in a float64 copy of the network (`a2.ts` `A2Net`, checked against the reference render to 1e-4).
2. **Candidate alignment:** the permutation and scales that bring B's activations closest to A's, and the matrix that brings B's layer inputs closest to A's (least squares over every layer and sample).
3. **The test.** For the candidate and for the identity (no alignment), the blends at 25, 50 and 75 % are rendered as the pedal would play them, and compared with the same crossfade of the two models' own outputs (level-matched, means removed). The error energy relative to the crossfade's is the blend error: 0 for a blend that is the crossfade, 1 for silence. **The candidate is kept only when its error is lower than the identity's.**
4. **The block** stores the transform kept, the fit `1 − error` (held to 0…1) and the identity's fit.

**Why the test is there.** Captures of different amps do not correspond channel for channel: on the pedal's five models below, B's channels explain under 10 % of the variance of A's activations whatever the permutation, so the candidate of step 2 is fitted to noise (and to the channels' DC offsets). Blending with it is far worse than blending unaligned: Fender → Two-Rock at 50 % plays at −50 dB with it, −29 dB without. The models do share a rough common layout (matching the weights instead also picks the identity permutation, for every pair tried), so the identity is the better start. The candidate wins where a model is another's copy in other coordinates, as the fixture is.

## Measured blends (the pedal's five models)

Unaligned (what the analysis keeps for all of these), on the stimulus; levels are the AC RMS of the level-matched output. Every position, the correlations and the DC offsets: `docs/morph-findings.md` "The unaligned blends, pair by pair".

| A → B | Fit | Level at 0 % | 25 % | 50 % | 75 % | 100 % |
|---|---|---|---|---|---|---|
| 2 Fender Super Reverb (edge) → 1 Two-Rock SSS (clean) | 80.8 % | −20.0 dB | −25.8 | −28.6 | −25.9 | −21.5 |
| 2 Fender Super Reverb → 3 Vox AC30 (edge) | 83.5 % | −20.0 | −24.7 | −25.1 | −22.7 | −20.3 |
| 1 Two-Rock SSS → 3 Vox AC30 | 79.6 % | −21.5 | −26.2 | −26.9 | −22.5 | −20.3 |
| 4 Marshall JCM800 (lead) → 1 Two-Rock SSS | 76.8 % | −21.1 | −19.4 | −20.1 | −20.8 | −21.5 |
| 5 Mesa Dual Rectifier (red, OD808) → 2 Fender Super Reverb | 20.8 % | −21.3 | −32.0 | −39.4 | −29.8 | −20.0 |
| 5 Mesa Dual Rectifier → 4 Marshall JCM800 | 21.7 % | −21.3 | −34.2 | −37.1 | −28.7 | −21.1 |

- **A fit near 80 %** is a usable morph: the blends track the crossfade (correlation 0.91–0.99 at every position) with a level dip of up to 9 dB in the middle.
- **A fit near 20 %** is not: the middle of the sweep is some other network, 9–19 dB quieter and unlike either amp. Both ends are still exact. The Mesa capture is the odd one out against both it was tried with.
- **The dip is a property of the weight blend,** not of the gains: the level match and the input calibration already move in dB.

Not built, with what a trial showed (`docs/morph-findings.md` has the trials and a longer list of avenues, starting with captures of one amp at several settings, which were not available to test):

- **Fitting the transform to the crossfade directly** (least squares on the blends' outputs, 12 parameters, from the identity): Fender → Two-Rock's dip at 50 % went from 8.6 to 4 dB and the Mesa → Fender's from 18 to 15 dB, for 25–40 s of analysis per pair. The block and the firmware would not change.
- **A make-up gain along the sweep,** measured by the analysis and stored in the block, would level the dip without changing the tone in the middle.
- **The blends' DC offset** in the middle of a sweep (up to −27 dB) is not removed unless the EQ's low-cut is raised.

## On the pedal

- **Load** (`loadPreset`, while the callback is parked): `ResolveMorph` finds the target; `dkmorph::Prepare` folds A's head scale in, applies the block's transform to B, puts both in the engine's weight layout and keeps A and B′ − A (`gMorphW`, 15 KB in AXI SRAM); the two models' level matches and input calibrations go to the callback in dB. A load with a morph takes about 960 µs instead of 140.
- **Every block** (`stepMorph`): the `morph` parameter is smoothed (20 ms, as the knobs are) and mapped to the blend position (`BlendPosition`: the 2 % end zones, linear between). When the position has moved more than 0.001 (or reaches an end), `A2Lite::Blend` writes `A + t·(B′ − A)` into the engine's weights and the level match and input calibration are set to `10^(((1 − t)·dB_A + t·dB_B) / 20)`. The first block after a load blends, so a preset loads at the knob's position.
- **A parked or dry pedal, and a preset without a target,** skip the blend; the smoother still runs.
- **Reloads:** MORPH on the playing slot reloads it (the preset fade). So does anything that changes which slot its block resolves to: an upload over, an erase or a swap of the target (`servePresets` compares `ResolveMorph` with the slot the loaded preset was prepared with).
- **Cost:** `Blend` is 155 islands of 12 loads + 12 loads + 12 multiply-adds + 12 stores (`docs/M7_PERFORMANCE.md`): 7,417 cycles with the two `powf`, 30.9 permille of a block (`docs/FIRMWARE.md` "Bench"). Live, sweeping the parameter continuously over the link: 502 avg / 515 max permille against 471 / 475 at rest, 0 underruns.

## Formats

The morph block, 64 bytes little-endian, in a slot after the model and the IR (`docs/FIRMWARE.md` "Preset store"; MORPH in `docs/MIDI.md` carries it):

| Offset | Field |
|---|---|
| 0 | u8 target slot (0–7) |
| 1 | u8 perm[3]: B's hidden channel j becomes channel perm[j] |
| 4 | u32 the target model binary's CRC field (its offset 7904) |
| 8 | f32 scale[3], each in 1/16…16 |
| 20 | f32 m[9], row-major, `|det| ≥ 1e-6` |
| 56 | f32 fit (0…1) |
| 60 | f32 unaligned fit (0…1); equal to the fit when the identity was kept |

`dkmorph::ParseBlock` rejects a block `Apply` cannot use (the slot then reads `morph`); the fits are for display only.
