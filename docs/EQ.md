# Ducky: EQ specification (low-cut, three bells, high-cut)

This is the specification for the pedal's EQ stage: its controls and ranges, the exact linear model that defines its curves, a reference response, and the behaviour an implementation must show. The bells and the stage's behaviour follow bad_tape's parametric EQ (`../bad_tape/docs/EQ.md`), restated here at 48 kHz; the cut filters are Ducky's own (M5, `docs/FILTERS.md` "Filters").

Status: built (M10, `daisy/src/dsp/eq.h`). The EQ sits after the IR, before the output volume. It is on by default, and at the defaults it is the identity.

## 1. Controls

Five bands in series: a low-cut, three bells and a high-cut. The EQ has no fixed control on the pedal: its parameters are set from the editor's EQ tab or `tools/pedal.ts param set`, or by any of Knobs 3–6 and SW1–SW3 assigned to one (a switch takes only a choice). They are global (not per preset) and saved with the settings record (`docs/FIRMWARE.md` "Settings record"). Ids, tapers and default codes: `docs/MIDI.md` "Parameters".

|Id|Name|Unit / values|Min|Max|Default|Taper|
|---|---|---|---|---|---|---|
|0|`eq_enable`|0 off, 1 on|0|1|1|choice|
|1|`low_cut_freq`|Hz|20|500|20 (off)|log, off at min|
|2|`low_cut_res`|Q|0.5|2|1|log|
|3|`low_cut_slope`|0 24, 1 12, 2 6 dB/oct|0|2|12 dB/oct|choice|
|4|`bell1_gain`|dB|−12|12|0|linear|
|5|`bell1_freq`|Hz|40|800|250|log|
|6|`bell1_q`|Q|0.4|8|1|log|
|7|`bell2_gain`|dB|−12|12|0|linear|
|8|`bell2_freq`|Hz|200|5000|1000|log|
|9|`bell2_q`|Q|0.4|8|1|log|
|10|`bell3_gain`|dB|−12|12|0|linear|
|11|`bell3_freq`|Hz|800|12000|3000|log|
|12|`bell3_q`|Q|0.4|8|1|log|
|13|`high_cut_freq`|Hz|1500|20000|20000 (off)|log, off at max|
|14|`high_cut_res`|Q|0.5|2|1|log|
|15|`high_cut_slope`|0 24, 1 12, 2 6 dB/oct|0|2|12 dB/oct|choice|

A continuous parameter's value is its position 0..1 along its taper (log: `min·(max/min)^v`; linear: `min + v·(max − min)`), on the wire a 14-bit code.

- A bell's frequency is its centre: the bell reaches its gain exactly there. Its Q sets the width (RBJ definition: Q 0.4 is about 3.4 octaves between the half-gain points, Q 1 1.9 octaves, Q 8 0.18 octaves).
- A cut's frequency is its cutoff. The slope is 6 dB/oct (a one-pole), 12 (a second-order section at the resonance Q) or 24 (a Butterworth section into a section at the resonance Q). The resonance does nothing at 6 dB/oct.
- A cut is off (bypassed) with its frequency within 2 % of its travel of the off end, and back on beyond 4 %.
- `eq_enable` off is a true bypass of the stage.

## 2. Signal model (normative)

The EQ is linear, time-invariant when its controls are still, mono and minimum-phase, with no latency. Its response is the product of five sections, in this order (the order matters only for rounding and while parameters move):

```
H(z) = LOWCUT(z) · BELL1(z) · BELL2(z) · BELL3(z) · HIGHCUT(z)
```

Each section is an analogue prototype mapped by the bilinear transform prewarped at the section's frequency `f0`. With `fs` the sample rate (48 kHz), `A = 10^(gain/40)` and `Q` the section's Q, the response at frequency `f` is the prototype at

```
s = j · tan(π f / fs) / tan(π f0 / fs)
```

|Section|Prototype|
|---|---|
|Bell (RBJ cookbook)|`(s² + s·A/Q + 1) / (s² + s/(A·Q) + 1)`|
|Cut, 6 dB/oct|low-cut `s / (1 + s)`, high-cut `1 / (1 + s)`|
|Cut, 12 dB/oct|low-cut `s² / (s² + s/Q + 1)`, high-cut `1 / (s² + s/Q + 1)`|
|Cut, 24 dB/oct|the 12 dB section at Q 1/√2 times the 12 dB section at Q|
|Cut, off|1|

Consequences, all exact:

- A bell's gain at `f0` is `gain`; a bell at 0 dB is exactly 1.
- Negating a bell's gain gives its exact inverse response (`H(−g) · H(g) = 1`).
- A 12 dB cut at Q 1/√2 is −3.01 dB at its cutoff; at Q it is `20·log10(Q)` dB there.

## 3. Reference response (model)

Setting A: low-cut 80 Hz, 12 dB/oct, Q 0.7071; bell 1 −4 dB at 250 Hz, Q 1.4; bell 2 +3 dB at 1.2 kHz, Q 0.7; bell 3 +5 dB at 3.5 kHz, Q 2; high-cut 8 kHz, 12 dB/oct, Q 0.7071. Values in dB at 48 kHz.

|30 Hz|50 Hz|80 Hz|120 Hz|250 Hz|500 Hz|1 kHz|1.2 kHz|2 kHz|3.5 kHz|5 kHz|8 kHz|12 kHz|16 kHz|
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
|−17.15|−8.86|−3.23|−1.39|−3.77|+0.30|+2.79|+3.08|+2.62|+5.57|+1.35|−2.61|−9.88|−19.10|

## 4. Required behaviour

Accuracy:

- The stage's magnitude is within ±0.05 dB of the model from 20 Hz to 20 kHz (61 points, 1/6 octave apart), controls still: each bell alone at the ends and the default of its frequency range, at ±12 dB and Q 0.4 and 8; each cut alone at each slope, Q 0.7071 and 2, at 100 Hz (low-cut) and 5 kHz (high-cut); and setting A.

Symmetry:

- Negating every bell's gain gives the inverse response: the two magnitudes sum to 0 dB within 0.1 dB at every frequency.

Identity:

- With `eq_enable` off, or at the defaults (every gain at 0 dB, both cuts off), the stage's output is bit-identical to its input once any transition has finished. That includes a gain at code 8192 (+0.0007 dB: 0 dB is not on the 14-bit grid) and a gain returned to 0 dB.

Changes while audio plays:

- Gains glide in dB, bell frequencies in log frequency and Qs in log Q, with 20 ms smoothing; the cuts smooth their positions with the same time constant. No zipper noise.
- A jump of any parameter between its ends (code 0 and 16383, a choice between its first and last value) adds no click: over the 50 ms after it, the largest sample-to-sample step of a 220 Hz sine stays within 1.25× the steady-state largest step at the settings before, after, and along the way.
- `eq_enable` crossfades over 20 ms; a cut's slope change crossfades over 5 ms between two running slopes, and so does its off end.
- The output does not depend on how the caller splits its blocks (bit-identical for blocks of 1, 6, 8, 24 and 48 samples).
- The stage stays finite and bounded for any sequence of parameter changes.

Cost:

- The pedal's worst-case load with the EQ moving must stay under the 850 permille ceiling (`docs/ROADMAP.md` "Decisions").

## 5. As built

- `daisy/src/params.h`: ids 0–15 (group `EQ`), the table above. The M9 table (7 parameters: the cuts and the 515 Hz mid) is replaced, not extended, so the settings record's version went to 2 and a version-1 record is ignored (`docs/FIRMWARE.md` "Settings record"). The parameters have no factory control: with no record every control is unassigned.
- `daisy/src/dsp/filters.h`: `CutFilter` (M5's cuts) and the bell, `DesignBell` and `Bell`. A bell is A. Simper's linear trapezoidal state-variable filter mixed to the prototype (output `x + m1·band`, `k = 1/(Q·A)`, `m1 = k(A² − 1)`), designed in double; every section filters in float.
- `daisy/src/dsp/eq.h`: `Eq`, the stage. Control ticks are 24 samples (2 kHz), counted in samples: `Control()` runs at each tick's start, `CutFilter::Run` and `Bell::Run` over whatever part of the tick the caller passes, with ramps of the form `c0 + dc·t` (t from the tick's start), so the output is the same for any split. A bell whose glides moved designs new coefficients on that tick and ramps them linearly across it; one starting from 0 dB starts at its identity. A gain within one 14-bit code step of 0 dB counts as 0, and a bell resting at 0 dB for 100 ms (200 ticks) stops and its state is zeroed. `eq_enable` off fades the stage out over 960 samples, then nothing runs; turning it on restarts the stage from zero state and fades it in. `Control` and `Start` are `noinline`, as in bad_tape, where the inlined double-precision design made every sample save d8–d14.
- `daisy/src/ducky.cpp` calls `gEq.Process(out[0], size, gStore.Values())` after the IR; `bench.cpp` times the worst case.
- `tests/eq_model.h`: the model of section 2 in closed form; `tests/eq_test.cpp` checks it against sections 2 and 3, and the stage against section 4. `tests/filters_test.cpp` keeps the cut filters' own tests (maps, prototypes, click-free slope and bypass changes).
- `editor/src/eqModel.ts`: the same model for the editor's response graph (`EqGraph.tsx`); `editor/src/eqModel.test.ts` checks it against section 3.
- Measured digital error against the model, worst over section 4's cases and the 61-point grid: 0.0039 dB (low-cut, 24 dB/oct, Q 2, at 20 Hz).
- Cost on the Seed (bench, 24-sample blocks, both cuts at mid-travel and full resonance with their slopes fading every 10 blocks, every bell flipping between −12 dB at a quarter and +12 dB at three quarters of its frequency range every 10 blocks, so every glide and ramp runs on every block): 15,564 cycles average, 15,600 at most, 65.0 permille of the block period (the M5 EQ: 36.2). Whole chain with an IR and the gate: 626.9 permille at most, under the 850 ceiling. Live on a model without an IR (`docs/FILTERS.md` "Measured"): load avg 415 permille at the defaults, 423 with both cuts on and the three bells at ±6 dB, 381 with `eq_enable` off.
