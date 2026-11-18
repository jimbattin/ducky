# Preset morph: research and findings

What was tried while building the preset morph (`docs/MORPH.md`), what it showed, and what is still open. Recorded on 2026-11-17 against firmware `92043c1`. `docs/MORPH.md` describes the feature as built; this file is the evidence behind its design and the list of things worth trying next.

## Summary

1. **A straight blend of two captures' weights is a usable morph for some pairs and not for others.** Of the six pairs measured, four follow a crossfade of the two amps closely (fit 77–84 %) with a level dip of up to 9 dB in the middle, and two (both with the Mesa capture) collapse in the middle (fit 21–22 %, 9–19 dB down, uncorrelated with either amp).
2. **Aligning the target by matching activations, the first design, makes every real pair worse than not aligning.** It only helps a model that is another's copy in other channel coordinates.
3. **Nothing cheaper than rendering the blend predicts how a pair will morph.** The activation fits, the weight distance and the permutation all fail as predictors. The analysis therefore renders the blends and measures them.
4. **The 12-parameter transform can improve a blend when fitted to the blend's output directly,** but slowly, and only partly for the bad pairs.
5. **Nothing here was judged by ear,** and no pair of captures of the same amp was available to test, which is the most promising case.

## Setup

**Models.** The five presets on the pedal, read back over the link (`PedalSession.readModel`). All are TONE3000 A2 captures with `input_level_dbu` 15. The amp names are read from the preset names, not checked.

| Slot | Preset name | Taken as |
|---|---|---|
| 1 | AATR SX3 Clean BAL3 CAB | Two-Rock SSS, clean |
| 2 | FNDR BFSR VB Edge WRM2 CAB FREE | Fender Super Reverb, edge of breakup |
| 3 | VX TB30 BR Edge0 BAL2 CAB FREE | Vox AC30, edge of breakup |
| 4 | MRSH JC800 Hi Lead2 BAL2 CAB | Marshall JCM800, lead |
| 5 | MES DRECF RD Gain2 BAL O808 CAB | Mesa Dual Rectifier, red channel, behind an OD808 |

Also the committed fixtures: `tests/fixtures/nam/A2.bin` and `tests/fixtures/morph/A2_shuffled.bin`, the same network in other channel coordinates.

**Stimulus.** `morphStimulus()` (`editor/src/morph/stimulus.ts`): 2 s at 48 kHz, eight Karplus–Strong plucks from 82 to 330 Hz, peaks 0.5 and 0.125 in turn. Synthetic; no recorded guitar was used.

**Rendering.** `A2Net` (`editor/src/morph/a2.ts`), the network in float64, checked against NeuralAmpModelerCore's render to 1e-4. A blend at position t is rendered as the pedal plays it: weights `A + t·(B′ − A)` with the head scales folded in, the input calibration and the level match blended in dB.

**Measures,** per blend position:

- **Level:** the AC RMS of the level-matched output, in dB (mean removed).
- **DC:** the output's mean, in dB.
- **Correlation:** between the blend's output and the crossfade `(1 − t)·yA + t·yB` of the two models' own outputs, means removed.
- **Blend error:** the energy of (blend − crossfade) over the crossfade's energy, means removed. 0 is a blend that is the crossfade, 1 what silence scores. **Fit** is 1 − the mean error at 25, 50 and 75 %, held to 0…1: the number the morph block stores.

**Tools.** Throwaway scripts around `editor/src/morph/` (`A2Net`, `applyTransform`, `fitAlignment`, `morphStimulus`) and a Levenberg–Marquardt loop with a numerical Jacobian. They were not kept; every experiment below is specified closely enough to rebuild.

## Findings

### 1. The transform is exact, and a copy in other coordinates aligns perfectly

- Applying a transform (a permutation and positive scales of the hidden channels, an invertible 3×3 matrix on the residual path, the head scale folded in) leaves the network's output unchanged: on slot 2's model the transformed and the original network differed by 5.6e-16 over the stimulus.
- These are all the symmetries the A2-Lite shape has. The head is shared by the 23 layers, so the hidden channels can only be permuted and scaled alike in every layer, and the residual path is one stream: 6 permutations, 3 scales, 9 matrix entries.
- On the fixture (`A2.bin` against its shuffled copy) activation matching recovers the transform: fit 100 % aligned, 4.7 % unaligned, weights recovered within 1e-5 of their peak. The blended engine then renders the reference at every position (`MorphBlend.AlignedBlendRendersTheReference`).

### 2. Activation matching makes real pairs worse

The first design fitted the permutation and scales that bring B's activations closest to A's, and the matrix that brings B's layer inputs closest to A's (least squares on uncentred sums over all layers and samples), and always applied the result. Levels in dB, correlation in brackets, at 25 / 50 / 75 %:

| Pair | Permutation picked | Hidden / residual fit | Aligned | Unaligned |
|---|---|---|---|---|
| 2 → 1 | 2 1 0 | 0.26 / 0.74 | −42.2 (0.73) / −50.3 (−0.01) / −46.7 (0.62) | −25.8 (0.96) / −28.6 (0.94) / −25.9 (0.97) |
| 2 → 3 | 0 1 2 | 0.35 / 0.73 | −16.9 (0.79) / −18.2 (0.56) / −21.7 (0.71) | −24.7 (0.96) / −25.1 (0.94) / −22.7 (0.97) |
| 1 → 3 | 1 0 2 | 0.47 / 0.72 | −43.5 (0.61) / −51.1 (0.53) / −40.8 (0.66) | −26.2 (0.96) / −26.9 (0.91) / −22.5 (0.93) |
| 4 → 1 | 1 0 2 | — | −25.4 (0.82) / −30.7 (0.76) / −33.3 (0.73) | −19.4 (0.93) / −20.1 (0.91) / −20.8 (0.94) |
| 5 → 2 | 2 1 0 | 0.32 / 0.48 | −47.5 (0.18) / −55.0 (−0.16) / −47.9 (−0.61) | −32.0 (0.47) / −39.4 (−0.14) / −29.8 (0.77) |
| 5 → 4 | 0 2 1 | 0.48 / 0.59 | −47.8 (0.19) / −57.5 (0.35) / −35.8 (0.81) | −34.2 (0.59) / −37.1 (0.07) / −28.7 (0.68) |

The ends are −20 to −21.5 dB for every model. Aligned, four of the six pairs fall 20–36 dB in the middle and 4 → 1 falls 10 dB; 2 → 3 instead comes out louder, with a DC offset of −10 dB. Unaligned is better in every row.

### 3. Why: the channels of two captures do not correspond

- **Centred, B's activations explain almost none of A's.** With each layer's means removed, the share of A's activation variance explained by B's channels, best scales, was between −0.04 and 0.07 for all six permutations on 5 → 2, 2 → 1, 2 → 3 and 4 → 1. There is no permutation to find.
- **The uncentred fits of finding 2 are fits to DC.** The activations (leaky ReLU outputs) and the residual path carry large constant offsets, which dominate uncentred sums. Fits of 0.3–0.7 looked like partial alignment and were not.
- **Centring does not rescue the method.** Activation matching on centred statistics gave scales of 0.06–0.74 and blends worse than unaligned on all four pairs tried (2 → 1 error at 25 / 50 / 75 %: 4.4 / 6.6 / 1.1 against 0.19 / 0.73 / 0.41 unaligned, uncentred error). Using only its hidden part (matrix = identity) blew up (2 → 3: +30 dB at 50 %); using only its matrix was also worse than unaligned.

### 4. The captures do share a rough common layout

Matching weights instead of activations (minimising `‖A − T(B)‖²` over the scales and the matrix by Levenberg–Marquardt, for each of the 6 permutations, from the identity):

| Pair | Relative distance, identity | Activation-matched | Weight-matched (best permutation) | Other permutations |
|---|---|---|---|---|
| 5 → 2 | 1.284 | 9.388 | 1.176 (0 1 2) | 1.39–1.52 |
| 2 → 1 | 1.212 | 2.787 | 1.098 (0 1 2) | 1.34–1.59 |
| 2 → 3 | 1.312 | 4.075 | 1.205 (0 1 2) | 1.63–1.80 |
| 1 → 3 | 1.520 | 6.973 | 1.421 (0 1 2) | 1.70–1.89 |
| 5 → 4 | 1.534 | 2.951 | 1.363 (0 1 2) | 1.57–1.72 |
| 4 → 1 | 1.157 | 3.720 | 1.041 (0 1 2) | 1.23–1.37 |

Distances are relative to `‖A‖²`; two unrelated weight sets of equal norm would score 2.

- **The identity permutation wins for every pair.** So the captures are not arbitrary relabelings of one another; they sit closer in their own coordinates than in any permuted ones. That they come from a common initialisation or pretraining is a guess, not checked.
- **The optimum is a shrink, not an alignment:** scales of 0.34–0.54 and a matrix near 0.4–0.7 times the identity, which lowers the distance by pulling B towards zero.
- **Its blends are slightly better than unaligned** on the five pairs that dip: 1–5 dB less dip at 25 % (5 → 2: −28.5 against −32.0; 5 → 4: −29.1 against −34.2) and 1–3 dB at 50 %, mixed at 75 %, with the same correlations. 4 → 1, which has no dip, moves 1 dB the other way. Not enough to turn a bad pair into a good one.
- **Weight distance does not predict blend quality:** 5 → 2 (1.284) is bad and 1 → 3 (1.520) is good.

### 5. Fitting the transform to the crossfade works, slowly and partly

The transform's 12 continuous parameters (identity permutation) fitted by Levenberg–Marquardt to minimise the blends' error against the crossfade at 25, 50 and 75 %, on the stimulus's first 24,000 samples, from the identity, at most 25 iterations; then measured on the whole stimulus:

| Pair | Error on the fit's samples | Time | Unaligned level at 25 / 50 / 75 % | Crossfade-matched |
|---|---|---|---|---|
| 2 → 1 | 0.376 → 0.080 | 25 s | −25.8 / −28.6 / −25.9 | −23.0 / −24.0 / −23.5 |
| 5 → 2 | 1.018 → 0.471 | 38 s | −32.0 / −39.4 / −29.8 | −26.4 / −35.8 / −26.0 |

- **2 → 1:** the dip at 50 % goes from 8.6 to 4 dB below slot 2's level, the correlations stay at 0.95–0.99, and the DC offset in the middle falls from about −27 to −50 dB.
- **5 → 2:** the correlation at 50 % goes from −0.14 to 0.52 and the dip from 18 to 15 dB. Better, still a hole.
- **The transforms it finds are not small:** scales 1.2 / 1.8 / 2.7 for 2 → 1, and 0.52 / 0.13 / 0.32 with a far-from-identity matrix for 5 → 2. It reshapes B's internals so that the path between the two is better, with B's own output unchanged.
- **Only the identity permutation, one start and one stimulus slice were tried.** It needs no firmware or block change: the block already carries any such transform.

### 6. The unaligned blends, pair by pair

What the analysis keeps for every real pair. Level in dB with the correlation in brackets; the ends are the models themselves.

| Pair | Fit | 0 % | 10 % | 25 % | 50 % | 75 % | 90 % | 100 % |
|---|---|---|---|---|---|---|---|---|
| 2 → 1 | 80.8 % | −20.0 | −22.7 (0.99) | −25.8 (0.96) | −28.6 (0.94) | −25.9 (0.97) | −22.7 (0.99) | −21.5 |
| 2 → 3 | 83.5 % | −20.0 | −22.3 (0.99) | −24.7 (0.96) | −25.1 (0.94) | −22.7 (0.97) | −21.1 (0.99) | −20.3 |
| 1 → 3 | 79.6 % | −21.5 | −23.6 (0.99) | −26.2 (0.96) | −26.9 (0.91) | −22.5 (0.93) | −20.1 (0.98) | −20.3 |
| 4 → 1 | 76.8 % | −21.1 | −20.0 (0.98) | −19.4 (0.93) | −20.1 (0.91) | −20.8 (0.94) | −21.2 (0.99) | −21.5 |
| 5 → 2 | 20.8 % | −21.3 | −24.8 (0.89) | −32.0 (0.47) | −39.4 (−0.14) | −29.8 (0.77) | −23.8 (0.97) | −20.0 |
| 5 → 4 | 21.7 % | −21.3 | −25.4 (0.90) | −34.2 (0.59) | −37.1 (0.07) | −28.7 (0.68) | −24.0 (0.91) | −21.1 |

- **The fit is symmetric as far as tried:** 2 → 5 also reads 20.8 %.
- **Gain alone does not decide it:** the Marshall lead capture morphs into the clean Two-Rock with no dip at all (within 2 dB of the ends throughout), while the Mesa fails against both a clean-ish and a high-gain partner.
- **The fixture model against the Mesa** (A2 in slot 7, briefly): fit 7.8 %.
- **The first design's readouts did not track any of this.** For the identity transform the hidden / residual fits read 0.00 / 0.00 for 2 → 1 and 2 → 3 (good morphs) and 0.32 / 0.23 for 5 → 4 (a bad one). That is why the block now stores the blend fit.

### 7. The middle of a sweep carries a DC offset

The models' own outputs have means of −61 to −80 dB on the stimulus. Unaligned blends have much more: 2 → 1 reaches −26.8 dB at 50 % and 5 → 2 −28.8 dB, as large as the signal there or larger. The fit is blind to it by construction (means are removed). On the pedal the only high-pass after the model is the EQ's low-cut, which is bypassed at the bottom of its range (the default); raised, it removes the offset (this pedal's K4 holds it at 162 Hz).

Not measured: whether the offset moving as the knob turns is audible as a thump. It is the first thing to listen for.

### 8. On the pedal

- **The collapse is real on the device.** With slot 5 playing and nothing plugged in (input noise −77 dBFS), the output noise read −55 dBFS at `morph` 0 % and −93 dBFS at 76 % on 5 → 2.
- **Cost:** the blend takes 7,417 cycles a block (31 permille) alone and 13.0 k in the bench's chain; live, a continuous sweep over the link read 502 / 515 permille avg / max against 471 / 475 at rest, 0 underruns (`docs/FIRMWARE.md` "Measured (preset morph)", "Bench").
- **The extra 5.6 k cycles in the chain were not investigated.** The blend's 15 KB of weights and the IR convolver's 66 KB of spectra both go through the 16 KB D-cache, which would explain it.

## What was built from this

- The analysis renders the blends for the activation-matched transform and for the identity and keeps the one with the lower blend error (`fitAlignment`). On all real pairs that is the identity; on the fixture, the alignment.
- The block's two readouts are the blend fit and the unaligned fit, shown by the pedal's log, `tools/pedal.ts` and the editor.
- The parameter's first and last 2 % are the two models alone.

## Limits of this record

- **No listening.** Every number is from a float64 render of a synthetic stimulus, or from the pedal's meters. The by-ear checks (the ends against FS2, the sweep, clicks) have not been done.
- **Six pairs of five captures,** all from one source and all `amp_cab`. No two captures of the same amp at different settings, no pedals alone, no models without a cab.
- **One stimulus,** at each model's own input calibration and Knob 1 at noon. Harder or softer playing was not tried, nor a recorded DI.
- **The crossfade is a stand-in for "a good morph".** A blend that differs from the crossfade may still sound good; one that matches it is no better than running both amps in parallel.
- **Zipper noise** from the blend's steps (the position moves at most once a block, in steps over 0.001) was not measured.
- **A preset loaded with `morph` away from 0 %** starts with the histories of its own model at rest while the weights are the blend's. The network needs up to 132 ms of signal to flush them, behind a 5 ms fade-in. Not measured, and probably worst where the blend's DC is largest.

## Avenues to explore

Roughly by expected value for the effort.

1. **Captures of one amp at several settings.** Clean to crunch to lead on the same amp, or the same amp with and without a boost, is what a morph knob is most naturally for, and such captures may share far more than captures of different amps do. Untested for want of models: measure the fit (`bun tools/pedal.ts morph A B` prints it) on such a set before investing in anything below.
2. **Listen.** Sweep the good pairs and a bad one. If the 80 % pairs sound right apart from the dip, a make-up gain (3) may be all that is needed; if the DC thumps, fix that first (4).
3. **A make-up gain along the sweep.** The analysis already renders the blends: store their levels at a few positions in the block and have `stepMorph` add the correction to the level match it already blends in dB. It levels the dip (up to 9 dB on good pairs) and costs nothing at run time. It does not change the tone in the middle and cannot rescue a 20 % pair. Needs room in the block: the two fit readouts are display only, or the block can grow behind a new flag.
4. **The DC offset.** Either a one-pole DC blocker after the engine while a morph target is loaded, or a head-bias correction per position measured by the analysis (the head's bias is one weight, already blended). Measure first how much of the mid-sweep offset is constant and how much follows the signal.
5. **Crossfade-matched transforms** (finding 5), as an option of the analysis. Open questions: all six permutations and several starts; the whole stimulus against a slice; whether penalising DC and level separately helps; whether the transforms it finds generalise to other playing (it fits 12 parameters to 72,000 samples, so probably, but check on a recorded DI); the time, which a worker or WebAssembly would cut. Start from the weight-matched transform of finding 4, which is cheap (no rendering) and already a little better than the identity.
6. **Settle the histories at a load.** In `loadPreset`, blend to the current position and then `Reset()` while the callback is parked, so a preset loaded mid-sweep starts from its blended network's own rest state. Small, and removes the last limit above.
7. **A curved path.** Linear paths between independently trained networks cross high-error regions; low-error curved paths usually exist (mode connectivity). Fit one waypoint network W so that A → W → B, as two linear segments or a quadratic Bézier, follows the crossfade. `Blend` already runs `a + t·d` and would take two segments at the same cost; the block would carry 1,870 more floats (7.5 KB; a slot has 48 KB free). Needs gradients through the network on the host, which the float64 `A2Net` does not give: an autodiff or a hand-written backward pass for 1,870 parameters, or training in Python outside the editor.
8. **Repairing the blend's statistics.** Interpolated networks are known to lose activation variance, which fits the level dips here. Rescaling each layer's pre-activations at a few positions so their variance matches the interpolation of the two models' is a correction, not a symmetry, so it changes the weights per position: store it as waypoints (7) or as per-layer gains folded into the conv weights. Cheaper to fit than a waypoint, since it needs statistics, not gradients.
9. **Stored intermediate models.** Train one or two captures-in-between against the crossfade (or against real in-between amp settings) and morph piecewise through them. The most faithful and the most work; needs a trainer.
10. **Running both models and crossfading the outputs** is the reference this record measures against. It does not fit today: the model costs 363 permille, so two of them with the IR, the EQ and the compressor at their worst exceed the 850 ceiling (657 + 363). Without an IR it would be near 835 live. It becomes an option only if the engine gets cheaper (`docs/M7_PERFORMANCE.md` "Not taken, and not measured").
11. **A better predictor than a render.** None was found (finding 3, 4, 6), and the render takes about a second, so this matters only if a host must rank many pairs.
12. **Cache placement.** If the chain's ceiling ever needs the 5.6 k cycles of finding 8, try `gMorphW` in DTCM (14,960 bytes; about 20 KB is free) and measure the bench's chain again.

## Background reading

From memory, not consulted during this work; check before citing.

- Ainsworth, Hayase, Srinivasa, "Git Re-Basin: Merging Models modulo Permutation Symmetries" (2022): activation matching and weight matching to align networks before interpolating; findings 2 and 4 are those two methods on this shape.
- Entezari et al., "The Role of Permutation Invariance in Linear Mode Connectivity of Neural Networks" (2021): the conjecture that independently trained networks are linearly connected once permuted.
- Jordan et al., "REPAIR: REnormalizing Permuted Activations for Interpolation Repair" (2022): the variance collapse of interpolated networks and its correction; avenue 8.
- Garipov et al., "Loss Surfaces, Mode Connectivity, and Fast Ensembling of DNNs", and Draxler et al., "Essentially No Barriers in Neural Network Energy Landscape" (both 2018): curved low-loss paths between trained networks; avenue 7.
