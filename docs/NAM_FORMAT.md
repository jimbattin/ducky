# NAM A2 model format

What Ducky reads from a `.nam` file, and how the A2-Lite network computes. This is the input to M1 (the converter and the reference outputs) and M2 (the engine on the PC).

## Reference implementation

[NeuralAmpModelerCore](https://github.com/sdatkinson/NeuralAmpModelerCore) (MIT), checked at commit `d3189ed` (2026-11-10). The facts below come from these files:

| File | Contents |
|---|---|
| `NAM/container.cpp` | `SlimmableContainer`: picks the first submodel whose `max_value` is greater than the slim value, else the last. |
| `NAM/wavenet/model.cpp` | The generic WaveNet (`Layer::set_weights_`, `LayerArray::set_weights_`, `WaveNet::set_weights_`). |
| `NAM/wavenet/a2_fast.cpp` | An A2-specific fast path. `is_a2_shape` is the shape check, `_load_weights` reads the weight stream in the generic order, and `_layer_forward_k` / `_head_forward` give the forward pass in scalar form. |
| `NAM/get_dsp.cpp` | `apply_metadata`: the top-level `metadata` sets loudness and the input and output levels. |
| `tools/render.cpp` | `render [--slim <0.0-1.0>] <model.nam> <input.wav> [output.wav]`. `--slim 0.25` selects submodel 0, which is A2-Lite. |

`render` calls `Reset(48000, 64)`. Reset prewarms by default (`gPrewarmOnResetDefault = true`): it runs `GetPrewarmSamples()` samples of silence through the model before the first input sample. It then processes the file in 64-sample buffers. Reference outputs from `render` therefore start from the settled zero-input state, and that state is what Ducky's constant-fill reset must reproduce (ROADMAP "Feasibility"). `NAM_SAMPLE` is `double` unless `NAM_SAMPLE_FLOAT` is defined. The network itself computes in `float` either way.

## File structure

A `.nam` file is JSON. Both development models (`models/`, git-ignored) use format `version` `0.7.0`:

```
{
  "version": "0.7.0",
  "architecture": "SlimmableContainer",
  "metadata": { name, modeled_by, gear_type, gear_make, gear_model, tone_type, trainer,
                loudness, gain, input_level_dbu, output_level_dbu, t3k_id, t3k_gear, weights_hash, date },
  "config": { "submodels": [
      { "max_value": 0.5, "model": { "architecture": "WaveNet", "config": {...}, "weights": [1871 floats],
                                     "metadata": { loudness, gain, date }, "sample_rate": 48000 } },   // A2-Lite
      { "max_value": 1,   "model": { ... "weights": [12146 floats] ... } }                             // A2-Full
  ] },
  "weights": [],
  "sample_rate": 48000
}
```

The JSON key order differs between files. Parse by key, never by position.

## The A2-Lite shape

Ducky runs only the submodel with `max_value` 0.5. The converter rejects the file, with a named reason, unless that submodel's `model.config` matches this shape exactly. These checks are `is_a2_shape`'s, plus `channels == 3`:

- `architecture` is `WaveNet`, `sample_rate` is 48000, `config.head` is `null` (no post-stack head), and `config.layers` has exactly one entry (one layer array).
- In that entry: `input_size` 1, `condition_size` 1, `channels` 3, `bottleneck` 3, `groups_input` 1, `groups_input_mixin` 1.
- There are 23 layers:
  - `kernel_sizes` is `6×14, 15, 15, 6×7`;
  - `dilations` is `1 3 7 17 41 101 239, 1 3 7 17 41 101 239, 1 13, 1 3 7 17 41 101 239`.
- `activation`: every entry is `{"type": "LeakyReLU", "negative_slope": 0.01}`.
- `gating_mode`: every entry is `"none"`. `secondary_activation`: every entry is `null`.
- `layer1x1` is `{"active": true, "groups": 1}`. `head1x1.active` is `false`.
- The eight FiLM blocks (`conv_pre_film`, `conv_post_film`, `input_mixin_pre_film`, `input_mixin_post_film`, `activation_pre_film`, `activation_post_film`, `layer1x1_post_film`, `head1x1_post_film`) all have `active` set to `false`.
- `head` (the layer array's head rechannel) is `{"out_channels": 1, "kernel_size": 16, "bias": true}`.
- `slimmable` is `null`. `weights` has exactly 1871 entries.

The A2-Full submodel has the same layout with `channels` and `bottleneck` set to 8 (12,146 weights).

## Weight stream

The weights form one flat list, read in this order (`_load_weights`). C is 3 (channels = bottleneck). A K-tap convolution from `in` to `out` channels is read as `for o in out: for i in in: for k in 0..K-1`, then its `out` biases. Tap `k = K-1` multiplies the newest sample.

| Block | Shape | Bias | Floats |
|---|---|---|---|
| Rechannel | 1 → C, 1×1 | no | 3 |
| Per layer, 23 times in order: | | | |
| · dilated conv | C → C, K taps, dilation d | yes | 9K + 3 |
| · input mixin | 1 → C, 1×1 (condition = the model input) | no | 3 |
| · layer1x1 | C → C, 1×1 | yes | 12 |
| Head rechannel | C → 1, 16 taps, dilation 1 | yes | 49 |
| `head_scale` | scalar | | 1 |

The totals are 21 layers × 72 + 2 layers × 153 = 1818, plus 3 + 49 + 1 = 1871. The trailing `head_scale` in the weights overrides `config.head_scale`. In both development models the two agree to float32 precision: the weight is the config value rounded to float32, for example 0.011273000174 against 0.011273000426.

## Forward pass (per sample t)

With `u[t]` as the input sample:

```
x_0[t]   = r · u[t]                                         r: rechannel, C×1
for layer l = 0..22 (kernel K_l, dilation d_l):
    z[t] = b_l + m_l · u[t] + Σ_{k=0}^{K_l-1} W_l[k] · x_l[t − (K_l−1−k)·d_l]
    a[t] = LeakyReLU_{0.01}(z[t])
    h[t] += a[t]                                            head sum, starts at 0 each sample
    x_{l+1}[t] = x_l[t] + c_l + L_l · a[t]                  layer1x1 residual
y[t] = head_scale · ( hb + Σ_{k=0}^{15} H[k] · h[t − (15−k)] )
```

The last layer's residual output `x_23` is unused. Each layer keeps `(K_l−1)·d_l` samples of history of its own input `x_l`, and the head keeps 15 samples of `h`. The receptive field is `Σ (K_l−1)·d_l + 15 = 6331 + 15 = 6346` samples (132 ms). NAM's prewarm is 6347 samples.

## Metadata Ducky uses

| Field | Where | Use |
|---|---|---|
| `name`, `gear_type`, `gear_make`, `gear_model`, `modeled_by` | top-level `metadata` | Shown in the editor. A `gear_type` that contains `cab` triggers the "already has a cab" warning when an IR is added. |
| `input_level_dbu`, `output_level_dbu` | top-level `metadata` | Shown in the editor, not applied (ROADMAP "Input gain"). |
| `loudness` (dB) | **the A2-Lite submodel's own** `model.metadata` | Output level-matching (Knob 2 at noon). The top-level `loudness` belongs to A2-Full, which is the one NAM's `apply_metadata` reads. The two values differ by about 0.1 dB in the development models. |
| `head_scale` | the A2-Lite weight stream's last float | Part of the network. |

Any field may be missing in other files. `loudness` is optional in NAM (`HasLoudness`). A model without an A2-Lite `loudness` is accepted, plays at unity, and the converter warns (see the binary's `FLAG_HAS_LOUDNESS`).

## Development models

| File | `name` | Gear | Lite `loudness` | Lite `head_scale` |
|---|---|---|---|---|
| `fender_super_eob_with_cab.nam` | FNDR BFSR VB Edge WRM2 CAB FREE | Fender Super Reverb 1965, `amp_cab` | −15.903149 | 0.011273 |
| `sss_with_cab_clean.nam` | AATR SX3 Clean BAL3 CAB | Two-Rock SSS 100W, `amp_cab` | −21.709897 | 0.0073515 |

Both come from TONE3000 (`trainer`), with `input_level_dbu` 15 and `output_level_dbu` 14. Both match the A2-Lite shape above.

## Upstream fixtures (NeuralAmpModelerCore `d3189ed`)

| File | What it is | Expected converter result |
|---|---|---|
| `example_models/A2.nam` | `SlimmableContainer`, submodels 0.5 → 3 channels (1871 weights), 1.0 → 8 channels | Accept (a third A2-Lite) |
| `example_models/slimmable_container.nam` | Container of LSTM (0.33), WaveNet 3 ch (0.66, 131 weights), WaveNet 4 ch | Reject: no `max_value` 0.5 submodel |
| `example_models/wavenet_a1_standard.nam` | Plain `WaveNet`, two layer arrays (16 and 8 channels): an A1 model | Reject: not a container |
| `example_models/wavenet_a2_feature_test.nam` | Plain `WaveNet`, 4 channels, A2 features exercised | Reject: not a container |
| `example_audio/input.wav` | 48 kHz, 24-bit, mono, 96,000 frames (2 s) | A short stimulus for reference renders |

## Ducky model binary (format 1)

The converter (`editor/src/nam/convert.ts`, CLI `bun tools/nam_convert.ts IN.nam...`, which writes `IN.bin`) turns the A2-Lite submodel into a fixed 7652-byte image. M2 (the PC engine) and M6 (the pedal) read this format. All values are little-endian.

| Offset | Type | Field |
|---|---|---|
| 0 | `u8[4]` | magic `"DKNM"` |
| 4 | `u16` | format version `1` |
| 6 | `u16` | flags: bit 0 `FLAG_HAS_LOUDNESS`, bit 1 `FLAG_HAS_INPUT_LEVEL` |
| 8 | `u32` | total size `7652` |
| 12 | `u8` ×4 | channels 3, bottleneck 3, layers 23, head kernel 16 |
| 16 | `u8[23]` | kernel sizes |
| 39 | `u8` | pad 0 |
| 40 | `u16[23]` | dilations |
| 86 | `u16` | pad 0 |
| 88 | `u32` | weight count `1870` |
| 92 | `f32[1870]` | the A2-Lite `weights[0..1869]` in the "Weight stream" order above, without the trailing `head_scale` |
| 7572 | `f32` | `head_scale` = `weights[1870]` (not `config.head_scale`) |
| 7576 | `f32` | A2-Lite `loudness` (dB); 0 when the flag is clear |
| 7580 | `f32` | top-level `input_level_dbu`; 0 when the flag is clear |
| 7584 | `u8[64]` | name, ASCII, NUL-padded (at most 63 characters, so always NUL-terminated) |
| 7648 | `u32` | CRC-32 of bytes `[0, 7648)` |

- **Floats:** each JSON double is rounded to float32.
- **CRC-32:** IEEE 802.3 / zlib (reflected polynomial `0xEDB88320`, init and final XOR `0xFFFFFFFF`; check value `crc32("123456789") = 0xCBF43926`).
- **Name:** the top-level `metadata.name`, trimmed, when it is a non-empty string, else the file's basename without `.nam`. Every code point outside U+0020–U+007E becomes `?`, then the name is cut to 63 characters. The pedal's LOG lines are printable ASCII.
- **Flags:** without `FLAG_HAS_LOUDNESS` the model plays at unity (no level-matching). Without `FLAG_HAS_INPUT_LEVEL` the editor shows the input level as "unknown".

### Reject reasons

The converter throws `ModelRejected(reason, detail)`; the detail names the JSON path and the value found. Checks run in this order:

| Reason | Meaning |
|---|---|
| `not-json` | Not JSON, or the top level is not an object. |
| `not-container` | `architecture` is not `SlimmableContainer` (plain `WaveNet` files: A1 models, or an A2-Full exported alone). |
| `no-lite-submodel` | No `config.submodels` entry has `max_value` 0.5. |
| `not-wavenet` | The 0.5 submodel's `architecture` is not `WaveNet`. |
| `sample-rate` | The 0.5 submodel's `sample_rate` is not 48000. |
| `shape` | The submodel fails the A2-Lite shape above (upstream `is_a2_shape`, `NAM/wavenet/a2_fast.cpp:832-984`, with its absent-key defaults, plus `channels` 3). An 8-channel model's detail says `(A2-Full)`. |
| `weights` | `weights` is not exactly 1871 finite numbers. |

## Reference renders

`tools/nam_reference.sh` clones NeuralAmpModelerCore into `build/namcore`, pins it at `d3189ed482ded880c128bf312941266c4fa5ceec` (with its Eigen and AudioDSPTools submodules), and builds `build/namcore-render` with plain `g++ -std=c++20 -O2 -DNAM_ENABLE_A2_FAST` (upstream's CMake fails under CMake 4.5; `-O2` instead of Release's `-Ofast` keeps the references strict IEEE). Every render uses `--slim 0.25`, so the A2-Lite submodel runs. Outputs are mono float32 at 48 kHz, the stimulus's length, and start from the prewarmed state (the first sample of `A2.input.ref.wav` is about −9.74e-4, not 0).

```
tools/nam_reference.sh          # ~1 min the first time (the build); rerunning rewrites identical files
uv run tools/make_stimuli.py    # just the guitar stimulus; --guitar WAV for another copy of the pinned file
bun tools/nam_convert.ts models/*.nam
make -C editor test check       # converter tests, tsc, Biome
```

| File | Source | Committed |
|---|---|---|
| `tests/fixtures/nam/A2.nam`, `slimmable_container.nam`, `wavenet_a2_feature_test.nam` | upstream `example_models/` at `d3189ed`, byte-for-byte (`A2.nam` sha256 `2d2d7445…fcf4ca`) | yes |
| `tests/fixtures/nam/input.wav` | upstream `example_audio/input.wav` (mono, 24-bit, 48 kHz, 96,000 frames) | yes |
| `tests/fixtures/nam/A2.bin` | `bun tools/nam_convert.ts tests/fixtures/nam/A2.nam`; `editor/src/nam/convert.test.ts` checks it byte for byte | yes |
| `tests/fixtures/nam/LICENSE` | upstream `LICENSE` (MIT, Steven Atkinson), which covers the files above | yes |
| `tests/fixtures/nam/A2.input.ref.wav` | `render --slim 0.25 A2.nam input.wav` | yes |
| `renders/stimuli/guitar_di.wav` | `make_stimuli.py`: left channel, first 20 s (960,000 frames) of `../spx90/guitar.wav` (16-bit stereo 48 kHz, sha256 `af49eb8e…7828f3`), as float32 | no (`renders/` is ignored) |
| `renders/ref/<model>.<stimulus>.ref.wav` | every `models/*.nam` and `A2.nam` on `input.wav` and `guitar_di.wav` | no (TONE3000 models stay local) |

`guitar.wav` stands in for a DI recording: its channels differ and it peaks at −0.05 dBFS. `make_stimuli.py` refuses any file whose sha256 differs from the pin.

## Engine

`daisy/src/dsp/a2lite.h` (`dkdsp::A2Lite`) runs the network from a format-1 image parsed by `daisy/src/model.h` (`dkmodel::Parse`). It keeps upstream `A2FastModel<3>`'s per-output summation order (conv bias, taps oldest first, inputs 0..2 within a tap, then the mixin; the layer1x1 residual as `lb + L0·a0 + L1·a1 + L2·a2`), so on the host (`g++ -O2`, no FMA) its output is bit-identical to every reference render. On the device, GCC contracts multiply-adds into FMAs; the error is then 241e-9 on `input.wav[48000, 72000)` (`docs/FIRMWARE.md` "Bench").

Each chunk of up to 48 frames passes through every layer in turn. A layer reads its input `x_l` from its own history ring and writes `x_{l+1}` straight into the next layer's ring. It works in tiles of 6 frames whose 18 conv sums stay in registers across every kernel tap, then applies the tile's mixin, activation, head sum and residual. Each ring holds its lookback plus one chunk, followed by a 5-frame guard that mirrors its first 5 frames, so any 6 consecutive frames are contiguous in memory (90,600 bytes in all). The stages (`BeginChunk`, `RunLayer`, `EndChunk`) are public so the bench can time each layer. Why the code is shaped as load islands followed by runs of multiply-adds: `docs/M7_PERFORMANCE.md`. Differences from upstream, none of which changes a result bit:

- **Reset** fills every history with the settled zero-input state in one pass per layer, instead of running a receptive field of silence.
- **Subnormal inputs** are flushed to zero (keeping the sign), so no history ever holds a subnormal.
- **Block size:** `Process` takes any length and runs internally in chunks of `kBlockSize` (48); the output does not depend on how the input is split.
- **Leaky ReLU** is `max(z, 0.01·z)`, which equals `z ≥ 0 ? z : 0.01·z` for every `z` and needs no branch.
- **The last layer's residual** `x_23` is not computed.

```
make render
build/host/render IN.wav OUT.wav --model X.bin [--block N]   # mono float32 48 kHz; channel 0 of a multi-channel input
```
