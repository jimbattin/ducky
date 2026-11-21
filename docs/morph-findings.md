# Preset morph: research and findings

What was tried while building the preset morph (`docs/MORPH.md`), what it showed, and what is still open. Findings 1–8 (the straight blend and its alignment) were recorded on 2026-11-17; findings 9–18 (trained paths) the same day against the tree that added them; finding 19 (the final paths on the pedal) on 2026-11-18. `docs/MORPH.md` describes the feature as built; this file is the evidence behind its design and the list of things worth trying next.

## Summary

1. **A straight blend of two captures' weights is a usable morph for some pairs and not for others.** Of the six pairs measured, four follow a crossfade of the two amps closely (fit 77–84 %) with a level dip of up to 9 dB in the middle, and two (both with the Mesa capture) collapse in the middle (fit 21–22 %, 9–19 dB down, uncorrelated with either amp).
2. **Aligning the target by matching activations, the first design, makes every real pair worse than not aligning.** It only helps a model that is another's copy in other channel coordinates.
3. **Nothing cheaper than rendering the blend predicts how a pair will morph.** The activation fits, the weight distance and the permutation all fail as predictors. The analysis therefore renders the blends and measures them.
4. **The 12-parameter transform can improve a blend when fitted to the blend's output directly,** but slowly, and only partly for the bad pairs. It is too small a lever: the barrier between two captures lies in the 1,870 weights, not in the 12 numbers of B's symmetries.
5. **Training the path instead works.** Three waypoint networks, trained by gradient descent so the piecewise-linear path follows the crossfade, take the six pairs from fits of 31–86 % to 89–98 % on the stored-fit stimulus, and from 4–75 % to 92–99.9 % on held-out guitar at three playing levels, with the level within 2 dB of the crossfade's and the DC below −47 dB (`docs/MORPH.md` "Measured blends"). The pedal's run-time cost does not change.
6. **What the training sees and how its loss weighs it decide what the path can do.** A path trained on a guitar recording alone fails on synthetic plucks and one trained on plucks alone is 14 points worse on the recording; a path trained at a normalised recording's level sags by up to 9 dB for quiet playing; a loss that scores a constant offset against a quiet passage trains a clean pair worse than no training. The trainer therefore mixes both signals at four levels, weighs quiet excerpts like loud ones, and scores the DC by its size.
7. **The player tried the trained paths on the pedal and approved them** ("great"); what was listened for was not recorded (finding 19). No pair of captures of the same amp was available to test, which is the most promising case.

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
- **Blend error:** the energy of (blend − crossfade) over the crossfade's energy, means removed. 0 is a blend that is the crossfade, 1 what silence scores. **Fit** is 1 − the mean error at 25, 50 and 75 %, held to 0…1: the number the morph block stored when findings 1–8 were recorded.

**The fit's positions.** Findings 1–8 and the probe runs of findings 9–12 measure the fit at 25, 50 and 75 %. The fit the block stores is now measured at the eighths (12.5 … 87.5 %: a three-waypoint path's knots and the middles of its segments), which reads higher for a straight blend because the positions near the ends are easy. The six pairs' unaligned fits, old and new: 2 → 1 80.8 and 83.6 %, 2 → 3 83.5 and 85.7 %, 1 → 3 79.6 and 82.3 %, 4 → 1 76.8 and 80.4 %, 5 → 2 20.8 and 31.4 %, 5 → 4 21.7 and 31.9 %. Findings 13–18 use the eighths.

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
- **Cost:** the blend takes 7,417 cycles a block (31 permille) alone and 13.0 k in the bench's chain; live, a continuous sweep over the link read 502 / 515 permille avg / max against 471 / 475 at rest, 0 underruns (`docs/MEASUREMENTS.md` "Morph", "Bench").
- **The extra 5.6 k cycles in the chain were not investigated.** The blend's 15 KB of weights and the IR convolver's 66 KB of spectra both go through the 16 KB D-cache, which would explain it.

## Trained paths

The path A → W₁ … W_K → B, piecewise linear in weight space, with the K waypoint networks trained so that the blend at every position renders the crossfade of the two models' outputs there. Both ends stay the two models. The crossfade is free training data: both models render any input.

**Probe** (findings 9–12): a throwaway PyTorch copy of the network (float64, within 2e-7 of the reference render; it reproduced the six unaligned fits of finding 6 to the digit). Adam at 3e-4 with a cosine decay, six excerpts of 8,192 samples a step, each at its own position spread over the path, the error normalised by the signal's mean crossfade power. Training signals: the first 15 s of `renders/stimuli/guitar_di.wav` at gains 1 and 0.35, and where noted 4 s of plucks (other notes and seed than the stimulus). Measured on the stimulus, on the DI's last 5 s (never trained on) and on those 5 s at −12 dB. Fit at 25 / 50 / 75 %.

**Trainer** (findings 13–18): `tools/morph_train.py`, the same network (within 1.4e-7 of the reference render; its fit of the fixture pair equals the editor's to six digits). Three waypoints from the chord, Adam at 1e-3, fit at the eighths, measured as above and at −24 dB.

### 9. Level is about half a good pair's error, and none of a bad pair's

With the best gain at each position (fit from 1 − correlation² instead of the blend error), the four good pairs read 86.5–91.6 % instead of 76.8–83.5 %, and the Mesa pairs 27–28 % instead of 21–22 %. A make-up gain along the sweep (the old avenue 3) would have levelled the good pairs and left the bad ones as they were.

### 10. One trained waypoint fixes a good pair

| Pair | Training | Stimulus: straight → path | DI held out | DI −12 dB | Worst level error on the stimulus |
|---|---|---|---|---|---|
| 2 → 1 | 1 waypoint, 1,500 steps, DI only | 80.8 → 94.7 % | → 98.0 % | → 98.0 % | 8.6 → 1.0 dB |
| 4 → 1 | 1 waypoint, 2,500 steps, DI and plucks | 76.8 → 91.3 % | 59.5 → 90.6 % | 47.6 → 84.6 % | 2.4 → 0.8 dB |

The waypoints end 0.08 and 0.14 of |B − A| away from the chord's midpoint, and the DC in the middle of the sweep falls from about −28 dB to −46 dB or lower: the loss keeps the mean, so level and DC are trained like everything else.

### 11. A bad pair needs waypoints on both sides of its barrier, started on the chord

| Pair | Training (4,000 steps, DI and plucks) | Stimulus | DI held out | DI −12 dB |
|---|---|---|---|---|
| 5 → 2 | 1 waypoint, started at A (1,500 steps against the 50 % crossfade alone, then the path) | 20.8 → 70.2 % | 36.2 → 87.7 % | 18.2 → 80.3 % |
| 5 → 2 | 3 waypoints, started on the chord | 20.8 → 85.5 % | 36.2 → 91.7 % | 18.2 → 86.8 % |
| 5 → 4 | 1 waypoint, started at A | 21.7 → 81.3 % | 13.1 → 70.4 % | 5.9 → 68.8 % |

- **Started at an end, the waypoint stays in that end's basin:** it ends 0.5 of |B − A| from the chord, the first segment is good and the second crosses the barrier (5 → 2: error 0.28 at 50 %, 0.68 at 62.5 %).
- **Three waypoints started on the chord stay near it** (0.12–0.18 of |B − A|) and are better at every position.
- **The chord's midpoint is a poor start for a single network,** though: trained alone against the 50 % crossfade for 1,500 steps, a network started there reached an error of 0.18 on held-out DI and one started at A 0.10. How well one A2-Lite can render a 50/50 mix of two amps at all (the floor of any one-model morph) was not established: the losses were still falling.

### 12. The training signal must cover what is played

- **DI only fails on plucks.** 5 → 2 with one waypoint trained on the DI alone: 82.4 % on held-out DI and 0.5 % on the stimulus (error 1.8 at 50 %). With plucks in the training the same pair's stimulus fit is 70–86 % (finding 11).
- **Plucks only is weaker on a DI.** The trainer on plucks alone (4,000 steps): stimulus 95.6 %, held-out DI 81.2 %, DI −12 dB 70.4 %. The same run with the DI in training: 89.8, 94.9 and 93.8 %. The stored fit, measured on plucks, flatters a plucks-only path.
- **The straight blend depends on the signal too:** 4 → 1 reads 76.8 % on the stimulus, 59.5 % on held-out DI and 47.6 % at −12 dB (at 25 / 50 / 75 %). One synthetic stimulus overstated it.

### 13. The pedal's real input is quieter than the recording, and a path trained loud sags when played quietly

With the first trained path on the pedal (5 → 2; training gains 0, −9 and −18 dB, error normalised by each signal's mean power) and the signal that was at its input (not controlled), the log's output peaks read −12 dBFS at 0 %, −16 to −18 at 50 %, −23 to −24 at 75 % and −20 at 100 %: lower at 75 % than at either end. The input peaked between −16 and −26 dBFS each second; `guitar_di.wav` is normalised (−0.05 dBFS), so that input corresponds to the recording at about −12 to −24 dB. That path on held-out DI, with the level error at 62.5 / 75 / 87.5 %:

| DI level | Straight | Path | Path's level error |
|---|---|---|---|
| 0 dB | 44.4 % | 94.9 % | −0.8 / −0.4 / −0.3 dB |
| −12 dB | 29.5 % | 93.8 % | −0.6 / −1.1 / −0.7 dB |
| −24 dB | 14.7 % | 87.0 % | −0.7 / −2.7 / −3.6 dB |
| −36 dB | 8.1 % | 82.8 % | −1.4 / −5.4 / −8.5 dB |

The sag is in the last segment, towards the clean amp, where a quiet input leaves the crossfade itself quiet and its error small against the loud passages' in the loss.

### 14. Quiet playing is fixed by the loss: each excerpt by its own level, the DC by its size

5 → 2, three waypoints, fit on held-out DI at 0 / −12 / −24 / −36 dB, then on the stimulus:

| Training | Steps | DI 0 / −12 / −24 / −36 dB | Stimulus |
|---|---|---|---|
| Gains 0, −9, −18 dB; error over the signal's mean power (finding 13's path) | 4,000 | 94.9 / 93.8 / 87.0 / 82.8 % | 89.8 % |
| The same | 8,000 | 93.1 / 91.0 / 81.1 / 75.5 % | 87.2 % |
| Gains 0, −9, −18, −28 dB; the same loss | 8,000 | 91.2 / 90.2 / 83.2 / 80.5 % | 84.7 % |
| Gains 0, −9, −18, −28 dB; error over each excerpt's own power (plus 5 % of the signal's mean) | 8,000 | 89.1 / 91.6 / 90.3 / 89.6 % | 82.8 % |
| The same, the error's mean scored apart against the loudest signal's power (the trainer) | 8,000 | 94.7 / 96.6 / 95.7 / 95.7 % | 89.3 % |
| Gains 0, −12, −24, −36 dB and quieter plucks; error over the signal's mean power | 4,000 | 82.3 / 79.8 / 72.0 / 70.0 % | 69.0 % |
| The same signals; error over each excerpt's own power | 4,000 | 80.2 / 78.9 / 77.3 / 80.7 % | 71.2 % |

- **Weighing each excerpt by its own level makes the path even across 36 dB of playing level** (89–92 % in the fourth row, 95–97 % in the fifth, against 83–95 % in the first).
- **The DC must be scored by its size, not against the excerpt.** A straight blend's offset is about −27 dB whatever is played (finding 7), 20 dB above the music in a quiet excerpt of the −28 dB signal, so in the fourth row's loss it swamps a clean pair's: 2 → 1 started at a loss of 27 and ended with a path worse than the straight blend on the stimulus (76.7 against 83.6 %) and level with it on DI (75–76 against 72–75 %). With the error's mean taken out and scored against the loudest signal's mean power, the same pair starts at 0.30 and ends at 98.3 % (DI 99.0 / 99.6 / 99.8 / 99.7 %), and 5 → 2 gains 5–7 points at every level (the fifth row). In that row 5 → 2's level error at −24 dB is within 0.9 dB at every eighth, and at −36 dB within 0.2 dB up to 75 % and −3.2 dB at 87.5 %.
- **It needs the steps:** at 4,000 the per-excerpt loss had not caught up (the last row).
- **More steps do not help the other loss:** the first two rows differ only in their length and the longer run is worse on held-out DI by 2–7 points, far more than separates two trainings of one configuration: the fifth row trained again, on four threads instead of one, read 94.4 / 96.2 / 95.0 % (−36 dB not measured) and 88.7 %. 15 s of one recording is little, and nothing here was repeated with another seed.
- **A learning rate of 1e-3** reached a lower training loss than the probe's 3e-4 in the same number of steps and was kept; no other was tried.

### 15. The crossfade itself is quieter in the middle

The level of the target, not of the blend: 5 → 2's crossfade on held-out DI reads −21.9 / −24.6 / −25.3 / −23.2 / −20.4 dB at 0 / 25 / 50 / 75 / 100 %. Two amps' outputs are only partly in phase, so their linear mix loses up to 4 dB against the ends. A path that follows the crossfade perfectly inherits that dip. At −24 dB the two ends are 18 dB apart (−25.4 and −43.6 dB): a high-gain capture hardly follows the input level and an edge-of-breakup one does, so the sweep's loudness depends on how hard the guitar is played, for any morph of such a pair.

### 16. Training time is the backward pass, 8,000 times

Measured on an i7-1185G7 (four cores), float32, PyTorch 2.15 on the CPU.

- **The work of a step:** six excerpts of 8,192 + 6,346 samples, each through its own network (its blend position's weights): every intermediate tensor is 6 networks × 3 channels × 14,538 samples, about 1 MB. 23 layers in series, each a dilated 3 → 3 convolution of 6 or 15 taps (one grouped `conv1d` for the six networks), a mix-in, a leaky ReLU, the head sum and a 3 × 3 residual product, then the head's convolution: about 1,700 multiply-adds a sample, 150 million a step going forward. It is vectorised along time and narrow everywhere else; there is no loop over samples and no branching.
- **A step of the trainer's own loop takes 54 ms** on four threads with the machine otherwise idle (120 steps): drawing the excerpts 0.5 ms, forward 15.1 ms, backward 37.5 ms, clipping and Adam 0.4 ms. The forward pass without gradients takes 10 ms.
- **Half of it is the convolutions' gradient.** A profile of five forward and backward passes: `convolution_backward` 50.4 % of the CPU time, the forward convolutions 14.4 %, and the rest spread over the elementwise operations and their gradients (adds 6.9 %, `bmm` 5.0 %, multiplies 2.8 %, nothing else over 2.5 %). The framework's bookkeeping calls take microseconds each.
- **Threads:** 103 / 65 / 49 / 77 ms a step (forward and backward) on 1 / 2 / 4 / 8 threads; the eight are hyperthreads.
- **A whole run takes 8 to 11 minutes:** 8,000 steps in 618 s the first time (77 ms a step, then the measurements: 11 minutes) and in 432 s the next day (54 ms a step, as measured fresh: 7 min 46 s for `tools/pedal.ts morph 4 1 --train`, reading the models and storing the path included). The two models' outputs on the 66 s of training signal take 6.4 s. The slow run followed an hour of other training on the same laptop; why it was slower was not established.
- **No gain from:** MKL-DNN off (93 ms against 56); one blend position a step, so that the six excerpts share their weights and the convolutions are plain ones (70 ms); more excerpts a step (9.1 and 8.2 ms an excerpt at 12 and 24, 9.3 at 6); a per-tap `bmm` formulation of the convolution (the same time as `conv1d`). float64 takes twice as long.
- **Excerpt length matters little:** 44 % of an 8,192-sample excerpt is receptive-field context that produces no loss. Per scored sample: 1,026 ns at 6 × 8,192, 981 ns at 3 × 16,384, 804 ns at 2 × 32,768, 1,158 ns at 1 × 65,536.
- **Several pairs train faster side by side:** four at once, one thread each (`OMP_NUM_THREADS=1`), took 1,401–1,415 s for their 8,000 steps, so 5.9 minutes a pair against 8 to 11 alone.

### 17. The stored path is the trained path

Slot 5 read back from the pedal with its three waypoints (finding 13's path, stored with `tools/pedal.ts morph 5 2 --path`), parsed, prepared and blended by the firmware's code on the host, is within 1.2e-5 of the trainer's network at each of the nine eighths of the sweep, on 2 s of DI (after 7,200 samples of silence: a blend keeps the histories of the network loaded before it).

### 18. On the pedal

- **Load:** 1,336 µs for a preset with three waypoints (962 with a straight morph, 134 without one).
- **Storing a path:** 0.4 s over the link for the 30,500-byte image, 180–185 ms of it flash.
- **Cost while sweeping** (the parameter driven up and down every 2 s over the link, slot 5 without an IR): up to 478 avg / 521 max permille against 464 / 468 at rest, 0 underruns. The blend reads one segment's two weight sets, as the straight blend does.

### 19. The final paths on the pedal

2026-11-18, the trained-paths firmware, slot 5 playing without an IR, the EQ's low-cut at 162 Hz. The signal at the input was not ours to control, but steady: its peaks averaged −18.4 dBFS over every reading below (−16 to −21 second by second).

- **Stored:** `morph 5 2 --path renders/morph/5_2.path` (`trained path, 3 waypoints, fit 88.7 % (straight 31.4 %)`, 0.4–0.6 s, 184–188 ms of flash); the slot's waypoints read back byte for byte the file's; the log reads `morph slot 2 fit 89 31 % waypoints 3 "FNDR …"`. Slots 2 → 1 and 1 → 3 were stored the same way.
- **The level along the sweep.** The log's `out` peak (the mean of 8 one-second readings for the path, 5 for the straight blend) with `morph` resting at each eighth, relative to 0 % (−11.0 dBFS), beside the trainer's network on the held-out DI scaled to input peaks of −18 dBFS (the mean of its one-second peaks):

| Position | 12.5 | 25 | 37.5 | 50 | 62.5 | 75 | 87.5 | 100 % |
|---|---|---|---|---|---|---|---|---|
| Pedal, straight blend (`morph 5 2`) | −6.0 | −13.6 | −23.2 | −29.0 | −19.8 | −15.6 | −10.6 | −6.6 dB |
| Pedal, trained path | −1.3 | −3.3 | −5.1 | −6.8 | −9.5 | −10.0 | −9.9 | −7.5 dB |
| Render, trained path | −1.6 | −3.0 | −4.4 | −5.5 | −7.0 | −8.5 | −10.6 | −8.3 dB |
| Render, crossfade | −1.1 | −2.4 | −3.7 | −5.3 | −6.7 | −8.2 | −9.4 | −8.3 dB |

  The path takes the output from the Mesa's level to the Fender's along the crossfade's slope, within 2.5 dB of the render on another signal and through the pedal's EQ, where the straight blend falls 29 dB in the middle. The path's lowest point lies 2.5 dB under the Fender's end at 75 %; the crossfade's own peaks do the same (1.1 dB under at 87.5 %, finding 15). The render's straight blend falls less (−14.6 dB at 50 %) because its peaks include the blend's DC offset, which the pedal's low-cut removes.
- **Finding 13's sag is gone:** that path read −23 to −24 dBFS at 75 % against −20 at 100 % on a like input; this one −21.0 against −18.5, the crossfade's shape.
- **`--train` end to end:** `bun tools/pedal.ts morph 4 1 --train` read both models, trained (stimulus 80.4 → 95.8 %, held-out DI 64.7 / 54.1 / 34.1 → 96.6 / 97.9 / 97.8 %), stored the path (`trained path, 3 waypoints, fit 95.8 % (straight 80.4 %), flash 167 ms`) and removed its temporary directory, in 7 min 46 s. The MIDI port stayed open and unread for the seven minutes of training and the store after it went through.
- **The link with waypoints in a slot,** on the playing slot 5: `rename` and back, `gate 5 on` and `off` kept the path (`info 5` unchanged, the waypoints byte for byte the file's afterwards); `swap 2 7` reloaded slot 5 as `morph slot 7 … waypoints 3` and the swap back as `morph slot 2 …`; `swap 5 8` moved the preset with its path and back; another model uploaded over slot 2 logged `morph missing slot 2` and the same model binary uploaded again brought `morph slot 2 fit 89 31 % waypoints 3` back without storing anything in slot 5; `morph 5 none` left a 7,996-byte image and `morph none`.
- **The editor** (headless Chromium, MIDI with SysEx granted, on two scratch slots holding slot 2's and slot 1's models with the 2 → 1 path): the Presets tab read `Trained path, 3 waypoints: fit 98.3 % (83.6 % as a straight blend)` (and slot 5's and slot 1's likewise); a rename saved there kept the path, byte for byte; **Morph to** another slot saved a straight blend (`Saved slot 6; slot 6 morphs with slot 3 (fit 85.7 %)`); a new model saved over the target analysed again by itself as a straight blend (`Saved slot 7; slot 6 morphs with slot 7 (fit 100.0 %)`: the new model was the preset's own). An IR change was not tried.
- **Cost:** sweeping the parameter over the link along the path, up to 482 avg / 520 max permille against 464 / 468 at rest, 0 underruns. The bench: `morph` 7,556 cycles avg, 32.0 permille max (7,417 and 31.4 before the paths); `chain` 169,322 cycles, 709.4 permille max against the 850 gate (169,569 and 710.9); `check` 241e-9.
- **By ear:** the player tried the paths on the pedal as left below and reported "great". Nothing more specific was recorded: which slots, how loud, the path against the straight blend, the knots, zipper noise and a load with the knob mid-travel are still open as checks ("To do on the pedal").

## What was built from this

- The analysis renders the blends for the activation-matched transform and for the identity and keeps the one with the higher fit (`fitAlignment`). On all real pairs that is the identity; on the fixture, the alignment.
- The block's two readouts are the fit of what the pedal plays and the unaligned straight blend's fit, shown by the pedal's log, `tools/pedal.ts` and the editor. The fit is measured at the eighths.
- The parameter's first and last 2 % are the two models alone.
- A slot holds up to three waypoints behind its morph block, and the pedal blends the segment the position lies in. `tools/morph_train.py` trains them (finding 14's fifth row is its configuration) and reports the fit, the level, the DC and the shape error on the stimulus and on held-out DI at three levels; `tools/pedal.ts morph A B --train` runs it and stores the result.

## Limits of this record

- **Little listening.** Every number is from a render or from the pedal's meters (finding 19: the final path's level along the sweep on the pedal agrees with its render). The player tried the paths and approved them, without detail; the by-ear checks one by one (the ends against FS2, the path against the straight blend, clicks, the knots) are not recorded.
- **Six pairs of five captures,** all from one source and all `amp_cab`. No two captures of the same amp at different settings, no pedals alone, no models without a cab.
- **One guitar recording,** 15 s of it for training and 5 s held out, and one seed: differences of a few points between training runs are not established (finding 14).
- **The crossfade is a stand-in for "a good morph".** A blend that differs from the crossfade may still sound good; one that matches it is no better than running both amps in parallel, and is quieter in the middle (finding 15).
- **The floor is unknown:** how closely one A2-Lite can render a mix of two amps, and so how much of what remains (2–11 % of the crossfade's energy on the stimulus, more for the Mesa pairs than for the others) is the path's and how much the network's.
- **Zipper noise** from the blend's steps (the position moves at most once a block, in steps over 0.001) was not measured, nor whether it differs along a path whose segments are longer than the chord's.
- **A preset loaded with `morph` away from 0 %** starts with the histories of its own model at rest while the weights are the blend's. The network needs up to 132 ms of signal to flush them, behind a 5 ms fade-in. Not measured.

## To do on the pedal

Everything that can be read from the pedal's meters and the link is in finding 19, with the player's verdict on a first try ("great", no detail). What is left needs ears, check by check. Record the results here as findings and in `docs/HISTORY.md` "Morph: trained paths".

**As left** (2026-11-18, before the crossfade became the default image). The pedal has been flashed since (`docs/HISTORY.md` "Expression pedal") and its slots hold other models (`docs/images/editor/presets.png`): read `bun tools/pedal.ts list` and `uv run tools/usb_log.py 1 --identity` before relying on any of this.

- **Firmware:** the weight morph (then the default image, now `APP=weight`) with the efficiency pass (`docs/MEASUREMENTS.md` "Live load"), flashed back after that pass's measurements of both images. Slot 1 plays: slot 5 is four FS2 presses on. Slot 6, used for those measurements, is empty again.
- **Slots:** 1–5 as in "Setup", 6–8 empty. Trained paths, three waypoints each: slot 5 → 2 (Mesa → Fender, `renders/morph/5_2.path`, fit 88.7 %), slot 2 → 1 (Fender → Two-Rock, `2_1.path`, 98.3 %), slot 1 → 3 (Two-Rock → Vox, `1_3.path`, 96.9 %), slot 4 → 1 (Marshall → Two-Rock, trained on the pedal by `--train`, 95.8 %; not a file). Slot 3 has no morph.
- **Controls:** K6 is `morph` (saved). The pedal has restarted since the last link SET, so nothing is held: `morph` follows K6.
- **The trained paths** of "Measured blends" are local files on the machine of this record (`renders/` is git-ignored): `renders/morph/A_B.path` for 2_1, 2_3, 1_3, 4_1, 5_2 and 5_4. Each names its two models by CRC (slot 1 `5077bd17`, 2 `7fe982f8`, 3 `c7145f5d`, 4 `f2f3b82e`, 5 `81ce3da5`), and `--path` refuses a file whose models are not the slots'. Without the files, a pair trains again in 8 to 11 minutes. `renders/slot_backup/slotN.bin` hold the five models as read before this work.

**Still to do:**

1. **Listen, check by check** (a first try was approved without detail), K6 by hand, loud and quiet, on slot 5 (the hard pair) and on slots 2, 1 and 4 (the cleaner ones): the path against the straight blend (`bun tools/pedal.ts morph 5 2` stores the straight one, `morph 5 2 --path renders/morph/5_2.path` the path again); the two ends against FS2; whether the knots at 25, 50 and 75 % are heard as steps or changes of character; zipper noise in a fast sweep; a thump from DC; a preset loaded with the knob mid-travel.
2. **The meters at a quiet level and with a known input.** Finding 19's input was steady but not ours, at one level. Play `renders/stimuli/guitar_di.wav` (or a looper) into the input at a normal and a quiet level and read the log's `out` peak at each eighth; the same file through the trainer's network is the comparison.
3. **The editor's screenshots** show no slot with a trained path (the Morph to row's trained-path line).

Open on the host, no pedal needed: whether the trainer needs its 8,000 steps with the final loss (finding 14 measured an earlier loss at 4,000). Train one cleaner pair and one Mesa pair at 2,000 and 4,000 steps (`--steps`), side by side.

## Avenues to explore

Roughly by expected value for the effort.

1. **Listen closely.** A first try on the pedal was approved (finding 19); sweep a trained path of a good pair and of a Mesa pair, loud and quiet, against the straight blend, check by check ("To do on the pedal"). What is worth doing below depends on what is heard.
2. **Captures of one amp at several settings.** Clean to crunch to lead on the same amp, or the same amp with and without a boost, is what a morph knob is most naturally for. Untested for want of models: measure the straight fit (`bun tools/pedal.ts morph A B` prints it) and train a path on such a set.
3. **Real captures as the targets in between.** The trainer's target at a knot can be any model's output, not only the crossfade: with a capture of the amp at a middle setting, train the path A → W → B so that W renders that capture. One model still runs, and the middle of the sweep is an amp, not a mix of two.
4. **More and other training signal.** A second recording, other pickups and playing, and a check with another seed (finding 14). The trainer takes any 48 kHz WAV (`--di`).
5. **A target that does not dip.** An equal-loudness crossfade (the linear mix scaled to the interpolated level of the two ends) removes finding 15's dip from what the path is trained to follow.
6. **Faster training** (finding 16). First, fewer steps: whether 8,000 are needed with the final loss is not measured ("To do on the pedal"), and L-BFGS or Gauss–Newton on a fixed batch may need far fewer than Adam for 5,610 parameters. Then the backward pass, 70 % of a step and half of it PyTorch's convolution gradient at a width it is not built for: a hand-written forward and backward for this network should bring a step near two or three times the forward's 10 ms (an estimate), and in a Web Worker or WebAssembly it would bring the trainer into the editor. A GPU was not available to try. Nothing cheaper inside PyTorch helped.
7. **More waypoints.** A slot has room for five (the format's flag for three); the last segment is the weak one for quiet playing (finding 14), so try unequal knots (more near the end with the more dynamic amp) before more of them.
8. **Settle the histories at a load.** In `loadPreset`, blend to the current position and then `Reset()` while the callback is parked, so a preset loaded mid-sweep starts from its blended network's own rest state.
9. **Running both models and crossfading the outputs** is the reference this record measures against. Built as an experiment (then `make -C daisy APP=xfade`; equal-power mix, the second engine in AXI SRAM) and measured on the pedal, load avg / max permille:

   | Configuration | Weight morph | Both models |
   |---|---|---|
   | 5 → 2, no IR, compressor Off | | 854 / 861 |
   | 5 → 2, no IR, compressor Blend or Direct | 464 / 468 | 885 / 892 |
   | Fender + 2,048-tap IR → 5, compressor Off | | overruns; 622 / 626 once shed to one model |
   | Fender + 2,048-tap IR → 5, compressor Blend | | overruns; 653 / 659 once shed to one model |

   The load is the same at 0, 50 and 100 %. The second engine and the mix cost 421 permille against the first engine's 363 in DTCM. Without an IR it plays with no overrun, above the 850 ceiling; with an IR each of three loads overran once and shed (one model with the IR reads 189 permille more than without, so about 1,040 to 1,070 is asked for: an estimate, the guard stops it being measured). A fast sweep had no zipper noise and no level dip in the middle by ear.

   **Since the efficiency pass it fits with an IR** (`docs/MEASUREMENTS.md` "Live load"): both engines in DTCM but for their three largest rings, an IR convolver at 78 permille instead of 187, and an engine at 339 instead of 363. Slot 1 → 3 without an IR 790 / 798 (the weight morph 430 / 434); the Fender model with a 2,048-tap IR → 5: 870 / 880, no underrun, no shed, where the image before overran (max 1,078) and shed. That is 20 permille above the 850 ceiling. What is left to take: the engines' rings in AXI SRAM are read without prefetch (`docs/M7_PERFORMANCE.md` "Cached data").

   **It is now the pedal's default image** (`make -C daisy`; `docs/FIRMWARE.md` "Build options"), and the weight morph of this record is the weight build, `make -C daisy APP=weight`. The trained paths and the checks in "To do on the pedal" need that image flashed.
10. **Cache placement.** If the chain's ceiling ever needs the 5.6 k cycles of finding 8, one segment's two weight sets fit DTCM (14,960 bytes; about 10 KB is free (`docs/FIRMWARE.md` "Memory")): copy the segment in use there when the position crosses a knot, and measure the bench's chain again.

Superseded by the trained path: a make-up gain along the sweep and a DC correction (both are in its loss), crossfade-matched transforms (finding 5: the same objective with 12 parameters instead of 5,610), repairing the blend's statistics, and a better predictor than a render.

## Background reading

From memory, not consulted during this work; check before citing.

- Ainsworth, Hayase, Srinivasa, "Git Re-Basin: Merging Models modulo Permutation Symmetries" (2022): activation matching and weight matching to align networks before interpolating; findings 2 and 4 are those two methods on this shape.
- Entezari et al., "The Role of Permutation Invariance in Linear Mode Connectivity of Neural Networks" (2021): the conjecture that independently trained networks are linearly connected once permuted.
- Jordan et al., "REPAIR: REnormalizing Permuted Activations for Interpolation Repair" (2022): the variance collapse of interpolated networks and its correction; not tried here, the trained path's loss covers it.
- Garipov et al., "Loss Surfaces, Mode Connectivity, and Fast Ensembling of DNNs", and Draxler et al., "Essentially No Barriers in Neural Network Energy Landscape" (both 2018): low-loss paths between trained networks found by training the path's bend, each step at a random position along it; findings 10–14 are that method with the crossfade as the loss.
