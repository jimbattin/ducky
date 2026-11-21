// The A2-Lite network in float64, one sample at a time, exposing every layer's input and activation: what the morph
// alignment (align.ts) measures. Spec: docs/NAM_FORMAT.md "Weight stream" and "Forward pass"; the pedal's engine is
// daisy/src/dsp/a2lite.h. Pure TypeScript (no DOM, no Bun APIs).

import { DILATIONS, KERNEL_SIZES } from "../nam/convert";

export const CHANNELS = 3;
export const LAYERS = KERNEL_SIZES.length;
export const HEAD_KERNEL = 16;
/** The network's stream weights, without the trailing head_scale. */
export const WEIGHT_COUNT = 1870;
/** Where the head's weights start in the stream: H[j][k] at HEAD_BASE + j * 16 + k, then its bias. */
export const HEAD_BASE = 1821;
/** NAM's prewarm: the receptive field plus one. */
export const PREWARM = 6347;
const LEAKY_SLOPE = 0.01;

/**
 * Where layer l's weights start in the stream. With K its kernel: the conv W[o][i][k] at (o * 3 + i) * K + k, its bias
 * cb[o] at 9K + o, the mixin m[o] at 9K + 3 + o, the layer1x1 L[o][i] at 9K + 6 + o * 3 + i, its bias lb[o] at
 * 9K + 15 + o.
 */
export function layerBase(l: number): number {
  let base = CHANNELS; // the rechannel comes first
  for (let i = 0; i < l; i++) base += 9 * KERNEL_SIZES[i]! + 18;
  return base;
}

export class A2Net {
  /** Layer l's input x_l of the last step, at 3 l. */
  readonly x = new Float64Array(LAYERS * CHANNELS);
  /** Layer l's activation of the last step, at 3 l. */
  readonly a = new Float64Array(LAYERS * CHANNELS);

  private readonly w: Float64Array;
  private readonly bases: number[] = [];
  /** Per layer: its x_l history, (K - 1) d + 1 frames of 3. */
  private readonly rings: Float64Array[] = [];
  private readonly pos = new Int32Array(LAYERS); // per ring: the frame the next step writes
  private readonly head = new Float64Array(HEAD_KERNEL * CHANNELS); // a ring of the head sums
  private headPos = 0;

  /** weights: the WEIGHT_COUNT stream weights. Starts in the settled zero-input state (NAM's prewarm). */
  constructor(
    weights: ArrayLike<number>,
    private readonly headScale: number,
  ) {
    if (weights.length !== WEIGHT_COUNT) throw new Error(`A2Net: ${weights.length} weights, need ${WEIGHT_COUNT}`);
    this.w = Float64Array.from(weights);
    for (let l = 0; l < LAYERS; l++) {
      this.bases.push(layerBase(l));
      this.rings.push(new Float64Array(((KERNEL_SIZES[l]! - 1) * DILATIONS[l]! + 1) * CHANNELS));
    }
    for (let i = 0; i < PREWARM; i++) this.step(0);
  }

  /** The output for input sample u. */
  step(u: number): number {
    const w = this.w;
    let x0 = w[0]! * u;
    let x1 = w[1]! * u;
    let x2 = w[2]! * u;
    let h0 = 0;
    let h1 = 0;
    let h2 = 0;
    for (let l = 0; l < LAYERS; l++) {
      const K = KERNEL_SIZES[l]!;
      const d = DILATIONS[l]!;
      const base = this.bases[l]!;
      const ring = this.rings[l]!;
      const frames = ring.length / CHANNELS;
      const p = this.pos[l]!;
      ring[3 * p] = x0;
      ring[3 * p + 1] = x1;
      ring[3 * p + 2] = x2;
      this.x[3 * l] = x0;
      this.x[3 * l + 1] = x1;
      this.x[3 * l + 2] = x2;

      const rest = base + 9 * K; // cb[3], m[3], L[9], lb[3]
      let z0 = w[rest]! + w[rest + 3]! * u;
      let z1 = w[rest + 1]! + w[rest + 4]! * u;
      let z2 = w[rest + 2]! + w[rest + 5]! * u;
      for (let k = 0; k < K; k++) {
        let q = p - (K - 1 - k) * d;
        if (q < 0) q += frames;
        const s0 = ring[3 * q]!;
        const s1 = ring[3 * q + 1]!;
        const s2 = ring[3 * q + 2]!;
        z0 += w[base + k]! * s0 + w[base + K + k]! * s1 + w[base + 2 * K + k]! * s2;
        z1 += w[base + 3 * K + k]! * s0 + w[base + 4 * K + k]! * s1 + w[base + 5 * K + k]! * s2;
        z2 += w[base + 6 * K + k]! * s0 + w[base + 7 * K + k]! * s1 + w[base + 8 * K + k]! * s2;
      }
      const a0 = z0 >= 0 ? z0 : LEAKY_SLOPE * z0;
      const a1 = z1 >= 0 ? z1 : LEAKY_SLOPE * z1;
      const a2 = z2 >= 0 ? z2 : LEAKY_SLOPE * z2;
      this.a[3 * l] = a0;
      this.a[3 * l + 1] = a1;
      this.a[3 * l + 2] = a2;
      h0 += a0;
      h1 += a1;
      h2 += a2;

      const L = rest + 6;
      x0 += w[L + 9]! + w[L]! * a0 + w[L + 1]! * a1 + w[L + 2]! * a2;
      x1 += w[L + 10]! + w[L + 3]! * a0 + w[L + 4]! * a1 + w[L + 5]! * a2;
      x2 += w[L + 11]! + w[L + 6]! * a0 + w[L + 7]! * a1 + w[L + 8]! * a2;
      this.pos[l] = p + 1 === frames ? 0 : p + 1;
    }

    const head = this.head;
    const hp = this.headPos;
    head[3 * hp] = h0;
    head[3 * hp + 1] = h1;
    head[3 * hp + 2] = h2;
    let y = w[HEAD_BASE + HEAD_KERNEL * CHANNELS]!;
    for (let k = 0; k < HEAD_KERNEL; k++) {
      const q = (hp + 1 + k) % HEAD_KERNEL; // tap 15 is this step's sum
      y +=
        w[HEAD_BASE + k]! * head[3 * q]! +
        w[HEAD_BASE + HEAD_KERNEL + k]! * head[3 * q + 1]! +
        w[HEAD_BASE + 2 * HEAD_KERNEL + k]! * head[3 * q + 2]!;
    }
    this.headPos = (hp + 1) % HEAD_KERNEL;
    return this.headScale * y;
  }
}
