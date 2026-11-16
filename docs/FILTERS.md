# IR and filters (M5)

The cab IR and the EQ that follow the model. Code: `daisy/src/dsp/convolver.h`, `daisy/src/dsp/filters.h`, `daisy/src/ir.h`; converter `editor/src/ir/`, `tools/ir_convert.ts`. Tests: `tests/convolver_test.cpp`, `tests/filters_test.cpp`, `tests/ir_test.cpp`, `editor/src/ir/convert.test.ts`.

## Signal chain

`AudioCallback` (`daisy/src/ducky.cpp`), in place on the left output buffer:

```mermaid
graph LR
  IN[in gain] --> NAM[A2Lite] --> IR[IR convolver] --> LC[low-cut] --> MID[mid] --> HC[high-cut] --> OUT[out gain × level match] --> M[mute] --> R[copy to R]
```

The IR stage exists only when an IR is compiled in (`IR=`, below). Every EQ section runs every block, whatever the knobs and switches, so the load does not depend on the controls.

## IR convolver

`dkdsp::IrConvolver`: uniformly partitioned overlap-save convolution with CMSIS-DSP's real FFT.

| | |
|---|---|
| IR length | `kIrTaps` 2048 taps at 48 kHz (42.7 ms) |
| Partition | `kIrPartition` = `kBlockSize` = 48 taps; `kIrParts` = 43 partitions (the last one 32 taps, zero-padded) |
| FFT | `kIrFft` = 128-point real (`arm_rfft_fast_f32`, a 64-point complex FFT inside) |
| Latency | none: a block's output includes its own input's contribution |
| Cost per block | constant: 1 forward FFT, 43 × 64 complex multiply-adds, 1 inverse FFT |

- **Per block:** the frame is the newest 128 input samples (80 older, 48 new). Its spectrum is stored as the newest entry of the input-spectrum ring. The output spectrum is Σₚ X(block − p)·Hₚ over the 43 partitions. The inverse FFT's last 48 samples are the block's output: overlap-save keeps outputs 47…127 of the circular convolution (81 valid samples, 48 needed).
- **Spectra:** CMSIS packs a real spectrum as `[DC, Nyquist, re1, im1, …, re63, im63]`. The multiply-accumulate treats every pair as one complex bin and then recomputes pair 0 as the two real products. CMSIS's inverse is a true inverse (unity scale), which the impulse test checks.
- **Mirrored ring:** `x_` holds 2 × 43 spectra and each new spectrum is written twice, at `slot` and `slot + 43`. Partition p reads `x_[slot + 43 − p]`, so its pointer moves down one spectrum per partition and never wraps.
- **Kernel:** 16 tiles of 4 complex bins. A tile keeps 8 accumulators in registers across all 43 partitions; each partition is one island of 16 loads (8 of X, 8 of H, each through `FpHere`) and then 16 multiply-adds, ordered so that an accumulator's two updates are 8 instructions apart (`docs/M7_PERFORMANCE.md` rules 2 and 6). The image reads `L16 M16` per partition.
- **Memory:** `h_` (partition spectra) 22,016 bytes, `x_` (input spectra) 44,032 bytes; 68 KB in all with the FFT buffers. No default member initialisers, so an instance may live in a `NOLOAD` section.
- **Placement:** AXI SRAM (`.bss`). The 66 KB of spectra stream through the 16 KB D-cache every block, and the bench measured the convolver at 109 permille from AXI SRAM against 138 from D2 SRAM (`DK_D2`), so D2 stays unused. The FFT itself (CMSIS-DSP C, built with Ducky's flags) runs from AXI SRAM, not ITCM.
- **Build:** libDaisy's `libdaisy.a` does not contain CMSIS-DSP. `daisy/cmsis_fft.mk` lists the sources and table defines for the 128-point real FFT; `daisy/Makefile` compiles them for the pedal, and the top-level `Makefile` compiles them for the host tests and `render` with CMSIS's own host switch `__GNUC_PYTHON__`. A missing table compiles fine but makes `Init` return false.

## IR binary (format 1)

`bun tools/ir_convert.ts IN.wav...` writes `IN.bin` next to each input; `dkir::Parse` validates and decodes it. All fields little-endian; unused taps and the pad are 0.

| Offset | Type | Field |
|---|---|---|
| 0 | `u8[4]` | `"DKIR"` |
| 4 | `u16` | version 1 |
| 6 | `u16` | flags 0 (reserved) |
| 8 | `u32` | size 8292 |
| 12 | `u32` | tap count, `min(resampled length, 2048)` |
| 16 | `u32` | source rate (Hz) |
| 20 | `u32` | source frames |
| 24 | `u16` | source channels |
| 26 | `u16` | pad 0 |
| 28 | `f32` | normalisation gain (linear), already applied to the taps |
| 32 | `u8[64]` | name, NUL-padded printable ASCII (at most 63 characters) |
| 96 | `f32[2048]` | taps at 48 kHz |
| 8288 | `u32` | CRC-32 (zlib) of bytes `[0, 8288)` |

`dkir::Parse` checks, in order: size (the length and the size field are both 8292) → `size`; magic → `magic`; version → `version`; CRC → `crc`; `1 ≤ tap count ≤ 2048` and every tap past the count is zero → `taps`; every tap and the gain finite → `samples`.

### Converter

`editor/src/ir/convert.ts` `convertIr(bytes, fallbackName)`, pure TypeScript shared with the M8 editor:

1. **Read** (`ir/wav.ts`, the semantics of `tools/wav.h`): RIFF/WAVE chunks walked (unknown ones skipped, odd sizes padded, a truncated last chunk tolerated). PCM 16/24/32-bit scaled by 1/2^(bits−1), IEEE float 32-bit, and `WAVE_FORMAT_EXTENSIBLE` with PCM or float subformat. Channel 0 only; `ir_convert` warns when there are more.
2. **Resample** to 48 kHz (`ir/resample.ts`) unless the source already is: direct windowed-sinc evaluation in float64. Lowpass at 0.95 × the lower Nyquist (`fc = 0.95 · min(rate, 48000) / 2`), Kaiser window β 8.6, 32 zero crossings each side. The output length is `ceil(frames · 48000 / rate)`.
3. **Truncate** to 2048 taps. A longer IR is faded out over its last 64 taps with a raised cosine that ends at exactly 0.
4. **Normalise** to unit energy (Σh² = 1) after resampling and truncation. The pedal plays the taps as stored.

Name: the fallback name (the file name without `.wav`) trimmed, non-printable characters replaced by `?`, cut at 63.

| Reject reason | When |
|---|---|
| `not-wav` | not RIFF/WAVE, or no `fmt ` or `data` chunk |
| `format` | a sample format other than the above, 0 channels, or a rate outside 8–384 kHz |
| `empty` | no frames |
| `samples` | a non-finite sample |
| `silent` | zero (or non-finite) energy |

The York Audio packs in `irs/` (3,480 WAVs at 44.1, 48 and 96 kHz, 500 ms each, so every one is truncated) all convert. The 44.1 kHz and 48 kHz versions of the same IR land within 0.2 % of the peak tap of each other, at the same −7.2 dB gain.

## Filters

`daisy/src/dsp/filters.h`. Every section is a trapezoidal (TPT, Zavalishin/Simper) one-pole or state-variable filter: bilinear-exact responses, with coefficients recomputed every block from the smoothed knobs (one-pole smoothing, 20 ms, at the block rate). `g = tan(π f / 48000)`.

- **One-pole:** `G = g/(1+g)`; `v = (x − s)·G`, `lp = v + s`, `s = lp + v`, `hp = x − lp`.
- **SVF:** `k = 1/Q`, `a1 = 1/(1 + g(g + k))`, `a2 = g·a1`, `a3 = g·a2`; per sample `v3 = x − ic2`, `v1 = a1·ic1 + a2·v3`, `v2 = ic2 + a2·ic1 + a3·v3`, `ic1 = 2v1 − ic1`, `ic2 = 2v2 − ic2`. LP `v2`, HP `x − k·v1 − v2`, bell `x + m1·v1`.

### Low-cut and high-cut

| Control | Map |
|---|---|
| Knob 5 (low-cut frequency) | 20…500 Hz, log (`20 · 25^k`) |
| Knob 4 (high-cut frequency) | 1.5…20 kHz, log |
| Knobs 6, 3 (resonance) | Q 0.5…2, log; 1.0 at noon |
| SW1 (low-cut), SW2 (high-cut) | up 6 dB/oct, middle 12 dB/oct, down 24 dB/oct |

- **Slopes:** 6 dB a one-pole (no resonance); 12 dB an SVF at the knob's Q; 24 dB a Butterworth SVF (Q 0.7071) into an SVF at the knob's Q. At Q 0.7071 on the knob the 24 dB cascade is two Butterworth sections (a Linkwitz–Riley knee, −6 dB at the cutoff); a higher Q peaks it.
- **All slopes run every block,** whatever the switch, with their states warm. A switch crossfades linearly from the current slope weights to the new slope over 5 ms (240 samples); a change mid-fade starts from where the weights are. Once a fade completes the weights are exactly one-hot, and the output is bit-identical to a filter that ran in the new slope all along.
- **Off end:** with the smoothed frequency knob within 0.02 of the end that turns the filter off (low-cut fully CCW, high-cut fully CW), the filter crossfades to dry over 5 ms, and comes back beyond 0.04 (hysteresis). At mix 0 the output is the input bit for bit.
- **Statement order:** the per-sample loop interleaves the one-pole, the 12 dB SVF and the 24 dB Butterworth stage so their latencies overlap (`docs/M7_PERFORMANCE.md` "Recursive filters"); the 24 dB second stage follows.

### Mid (SW3)

A bell at 515 Hz, Q 1.0, constant-Q form: `A = 10^(dB/40)`, `k = 1/(Q·A)`, `m1 = k(A² − 1)`. SW3 **up is +5 dB (boost), middle bypass, down −7 dB (scoop)** — the opposite of the order in which `CLAUDE.md` lists them, chosen for M5. The gain is smoothed in dB (20 ms), so a flip glides; at 0 dB (`|dB| < 1e-3`) `m1` is exactly 0 and the output is the input bit for bit.

### Tests

`tests/filters_test.cpp`: the maps and the hysteresis; every slope at Q 0.707 and 2 for a 100 Hz low-cut and a 5 kHz high-cut at 4 frequencies each, and the mid bell, against the bilinear-exact prototypes (±0.05 dB above −40 dB); neutral settings bit-exact; 4 s of noise with both cutoffs and resonances swept and SW3 flipping every 100 ms stays finite and below 8; slope switches and the bypass engage add no step larger than 1.25× the steady signal's (a hard switch measures 32×).

## Build: `IR=`

```sh
bun tools/ir_convert.ts "irs/<pack>/48k (…)/Mixes/BLND/YA FDMN 412 BLND Mix 01.wav"
cp "irs/<pack>/48k (…)/Mixes/BLND/YA FDMN 412 BLND Mix 01.bin" irs/fdmn_blnd_mix01.bin
make -C daisy -j IR=irs/fdmn_blnd_mix01.bin
```

`IR=FILE.bin` (relative to the repo root or absolute; default none, no IR stage) is `.incbin`ed through `DK_IR_PATH`. The path must not contain spaces (make and `.incbin` both split on them), so copy a converted pack file to a plain name first. `build/<BOARD>/ir_path` records the path (`none` without one), so switching `IR` rebuilds `ducky.o`. A compiled-in IR that does not parse, or a failed FFT init, stops audio like a bad model: the pedal serves USB (DFU included) and logs `ir <status>` (or `ir fft`) every second. M6 replaces the compiled-in IR with the preset store.

## Measured (Seed 1.2)

Bench (`APP=bench`, `docs/FIRMWARE.md` "Bench"), cycles per 48-sample block of noise, permille of the 480,000-cycle block period:

| Stage | Avg cycles | Max permille |
|---|---|---|
| A2-Lite | 168,561 | 352.4 |
| IR convolver, AXI SRAM (the pedal's) | 51,735 | 109.8 |
| IR convolver, D2 SRAM | 64,440 | 138.2 |
| EQ (both cuts, mid; slope fades every 10 blocks) | 15,135 | 31.8 |
| Whole chain (gate, limit 550) | 237,192 | 496.4 |

- **IR check:** the convolver against a direct convolution in double on the device, 4,800 samples of `input.wav`: max error 214e-9 (limit 1e-5).
- **Live** (`A2.bin`, `IR=irs/fdmn_blnd_mix01.bin`, 5 minutes): load avg 499, max 501 permille, 0 underruns (limit 600). Without `IR=`: avg 392, max 393. The load does not depend on the controls, since every section always runs.
- **Level:** unit energy is a power-neutral gain for white noise, not for guitar: a cab IR passes 100 Hz–5 kHz and rejects the rest, so in its passband it gains. On `A2.bin` the FDMN mix adds about 11 dB (`docs/FIRMWARE.md` "Measured (M5)").
