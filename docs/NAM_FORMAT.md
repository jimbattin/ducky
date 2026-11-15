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

Any field may be missing in other files. `loudness` is optional in NAM (`HasLoudness`), so M1 must decide how to handle a model without it: reject it, or store "unknown" and play it at unity.

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
