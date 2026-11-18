// Preset morph alignment: re-expresses model B in model A's channel coordinates, so a straight blend of the two weight
// sets is a network between them. The pedal applies the same transform (daisy/src/morph.h); tests/fixtures/morph pins
// the two together. Pure TypeScript.
//
// An A2-Lite network (docs/NAM_FORMAT.md "Forward pass") computes the same output when
//   - its hidden channels (the activations a, summed into the head) are permuted and scaled by positive factors:
//     a' = G a, with G[perm[j]][j] = scale[j]. A leaky ReLU commutes with a positive scale;
//   - its residual path (the layer inputs x) goes through any invertible matrix: x' = M x.
// The weights that do it: r' = M r; per layer and tap W' = G W M^-1, cb' = G cb, m' = G m, L' = M L G^-1, lb' = M lb;
// per head tap H' = s H G^-1, hb' = s hb, with the head scale s folded in (the result's head scale is 1).
// fitAlignment picks the G and M that bring B's activations and layer inputs closest to A's on morphStimulus, and
// keeps them only when the blends they give morph between the two models better than the unaligned blends do.

import { inputCalDb, levelMatchDb } from "../controls";
import { KERNEL_SIZES, type ModelWeights } from "../nam/convert";
import { A2Net, CHANNELS, HEAD_BASE, HEAD_KERNEL, LAYERS, layerBase, WEIGHT_COUNT } from "./a2";
import { morphStimulus } from "./stimulus";

export interface MorphTransform {
  /** B's hidden channel j becomes channel perm[j]. */
  perm: [number, number, number];
  /** ... scaled by scale[j] > 0. */
  scale: [number, number, number];
  /** M, row-major: B's residual path becomes M x. */
  m: number[];
}

/** A transform and how well the blends it gives morph between the two models. */
export interface MorphFit extends MorphTransform {
  /**
   * How closely the blends follow a crossfade of the two models' outputs (1 - blendError, held to 0..1): 1 is a
   * blend that is the crossfade, 0 one no closer to it than silence is.
   */
  fit: number;
  /** The same for the unaligned blends (the identity transform): equal to fit when the identity was kept. */
  unalignedFit: number;
}

const I3 = [1, 0, 0, 0, 1, 0, 0, 0, 1];
export const IDENTITY: MorphTransform = { perm: [0, 1, 2], scale: [1, 1, 1], m: I3 };

/** The scale's range (daisy/src/morph.h ParseBlock). */
const SCALE_MAX = 16;
/** The blend positions at which an alignment's blends are compared with the crossfade of the two models' outputs. */
const BLEND_POSITIONS = [0.25, 0.5, 0.75];
const PERMS: [number, number, number][] = [
  [0, 1, 2],
  [0, 2, 1],
  [1, 0, 2],
  [1, 2, 0],
  [2, 0, 1],
  [2, 1, 0],
];

// 3x3 matrices, row-major.
type Mat = ArrayLike<number>;

function mul(a: Mat, b: Mat): number[] {
  const out = new Array<number>(9);
  for (let r = 0; r < 3; r++)
    for (let c = 0; c < 3; c++)
      out[3 * r + c] = a[3 * r]! * b[c]! + a[3 * r + 1]! * b[3 + c]! + a[3 * r + 2]! * b[6 + c]!;
  return out;
}

function det(a: Mat): number {
  return (
    a[0]! * (a[4]! * a[8]! - a[5]! * a[7]!) -
    a[1]! * (a[3]! * a[8]! - a[5]! * a[6]!) +
    a[2]! * (a[3]! * a[7]! - a[4]! * a[6]!)
  );
}

function inverse(a: Mat): number[] {
  const d = det(a);
  return [
    (a[4]! * a[8]! - a[5]! * a[7]!) / d,
    (a[2]! * a[7]! - a[1]! * a[8]!) / d,
    (a[1]! * a[5]! - a[2]! * a[4]!) / d,
    (a[5]! * a[6]! - a[3]! * a[8]!) / d,
    (a[0]! * a[8]! - a[2]! * a[6]!) / d,
    (a[2]! * a[3]! - a[0]! * a[5]!) / d,
    (a[3]! * a[7]! - a[4]! * a[6]!) / d,
    (a[1]! * a[6]! - a[0]! * a[7]!) / d,
    (a[0]! * a[4]! - a[1]! * a[3]!) / d,
  ];
}

/** v[at + step * i] for i in 0..2, times the matrix: out[at + step * r] = sum_c a[r][c] v[at + step * c]. */
function mulColumn(a: Mat, v: ArrayLike<number>, at: number, step: number, out: Float64Array): void {
  for (let r = 0; r < 3; r++)
    out[at + step * r] = a[3 * r]! * v[at]! + a[3 * r + 1]! * v[at + step]! + a[3 * r + 2]! * v[at + 2 * step]!;
}

/**
 * The WEIGHT_COUNT stream weights of the network (weights, headScale) in t's coordinates, with the head scale folded
 * into the head: the result's head scale is 1.
 */
export function applyTransform(t: MorphTransform, weights: ArrayLike<number>, headScale: number): Float64Array {
  const g = new Array<number>(9).fill(0);
  const gInv = new Array<number>(9).fill(0);
  for (let j = 0; j < 3; j++) {
    g[3 * t.perm[j]! + j] = t.scale[j]!;
    gInv[3 * j + t.perm[j]!] = 1 / t.scale[j]!;
  }
  const m = t.m;
  const mInv = inverse(m);
  const out = new Float64Array(WEIGHT_COUNT);
  mulColumn(m, weights, 0, 1, out); // r' = M r

  const tap = new Array<number>(9);
  for (let l = 0; l < LAYERS; l++) {
    const K = KERNEL_SIZES[l]!;
    const base = layerBase(l);
    for (let k = 0; k < K; k++) {
      for (let j = 0; j < 9; j++) tap[j] = weights[base + j * K + k]!; // W[k][o][i] at (o * 3 + i) * K + k
      const w = mul(mul(g, tap), mInv);
      for (let j = 0; j < 9; j++) out[base + j * K + k] = w[j]!;
    }
    const rest = base + 9 * K;
    mulColumn(g, weights, rest, 1, out); // cb' = G cb
    mulColumn(g, weights, rest + 3, 1, out); // m' = G m
    for (let j = 0; j < 9; j++) tap[j] = weights[rest + 6 + j]!;
    const lw = mul(mul(m, tap), gInv); // L' = M L G^-1
    for (let j = 0; j < 9; j++) out[rest + 6 + j] = lw[j]!;
    mulColumn(m, weights, rest + 15, 1, out); // lb' = M lb
  }

  // The head: per tap the row H[k] (over the hidden channels j) times G^-1, scaled.
  for (let k = 0; k < HEAD_KERNEL; k++)
    for (let c = 0; c < 3; c++) {
      let sum = 0;
      for (let j = 0; j < 3; j++) sum += weights[HEAD_BASE + j * HEAD_KERNEL + k]! * gInv[3 * j + c]!;
      out[HEAD_BASE + c * HEAD_KERNEL + k] = headScale * sum;
    }
  const hb = HEAD_BASE + HEAD_KERNEL * CHANNELS;
  out[hb] = headScale * weights[hb]!;
  return out;
}

/**
 * How far the weight blends of a and the transformed b are from morphing between the two models: per blend position
 * p, the error energy of the blended network's output against the crossfade (1 - p) outA + p outB of the models' own
 * outputs, both with their means removed, relative to the crossfade's; the mean over BLEND_POSITIONS. 0 is a perfect
 * morph, 1 what silence scores. The blend runs as on the pedal: the input calibration and the level match blended in
 * dB. outA and outB are the models' level-matched outputs on s.
 */
function blendError(
  a: ModelWeights,
  b: ModelWeights,
  t: MorphTransform,
  s: Float64Array,
  outA: Float64Array,
  outB: Float64Array,
): number {
  const wa = applyTransform(IDENTITY, a.weights, a.headScale);
  const wb = applyTransform(t, b.weights, b.headScale);
  const calA = inputCalDb(a.inputLevelDbu);
  const calB = inputCalDb(b.inputLevelDbu);
  const levelA = levelMatchDb(a.loudness) ?? 0;
  const levelB = levelMatchDb(b.loudness) ?? 0;
  let total = 0;
  for (const p of BLEND_POSITIONS) {
    const net = new A2Net(
      wa.map((v, i) => v + p * (wb[i]! - v)),
      1,
    );
    const cal = 10 ** ((calA + p * (calB - calA)) / 20);
    const level = 10 ** ((levelA + p * (levelB - levelA)) / 20);
    let err = 0; // sums of the error and of the crossfade, and of their squares
    let err2 = 0;
    let mix = 0;
    let mix2 = 0;
    for (let i = 0; i < s.length; i++) {
      const want = (1 - p) * outA[i]! + p * outB[i]!;
      const e = level * net.step(cal * s[i]!) - want;
      err += e;
      err2 += e * e;
      mix += want;
      mix2 += want * want;
    }
    total += (err2 - (err * err) / s.length) / (mix2 - (mix * mix) / s.length);
  }
  return total / BLEND_POSITIONS.length;
}

/**
 * The transform of b to blend it with a: both run morphStimulus at their own input calibration. The fitted alignment
 * when its blends follow the crossfade of the two models' outputs more closely than the unaligned blends do
 * (blendError), else the identity: captures of different amps seldom correspond channel for channel, and aligning
 * their barely correlated activations would scramble the target instead.
 */
export function fitAlignment(a: ModelWeights, b: ModelWeights): MorphFit {
  const s = morphStimulus();
  const gainA = 10 ** (inputCalDb(a.inputLevelDbu) / 20);
  const gainB = 10 ** (inputCalDb(b.inputLevelDbu) / 20);
  const levelA = 10 ** ((levelMatchDb(a.loudness) ?? 0) / 20);
  const levelB = 10 ** ((levelMatchDb(b.loudness) ?? 0) / 20);
  const netA = new A2Net(a.weights, a.headScale);
  const netB = new A2Net(b.weights, b.headScale);
  const outA = new Float64Array(s.length);
  const outB = new Float64Array(s.length);
  const hab = new Float64Array(9); // sum aA_i aB_j at 3 i + j
  const haa = new Float64Array(3);
  const hbb = new Float64Array(3);
  const xab = new Float64Array(9); // sum xA xB^T
  const xbb = new Float64Array(9); // sum xB xB^T
  const n = LAYERS * CHANNELS;
  const { a: actA, x: xA } = netA;
  const { a: actB, x: xB } = netB;
  for (let k = 0; k < s.length; k++) {
    outA[k] = levelA * netA.step(s[k]! * gainA);
    outB[k] = levelB * netB.step(s[k]! * gainB);
    for (let at = 0; at < n; at += 3)
      for (let i = 0; i < 3; i++) {
        const ai = actA[at + i]!;
        const bi = actB[at + i]!;
        const xi = xA[at + i]!;
        const yi = xB[at + i]!;
        haa[i] = haa[i]! + ai * ai;
        hbb[i] = hbb[i]! + bi * bi;
        for (let j = 0; j < 3; j++) {
          hab[3 * i + j] = hab[3 * i + j]! + ai * actB[at + j]!;
          xab[3 * i + j] = xab[3 * i + j]! + xi * xB[at + j]!;
          xbb[3 * i + j] = xbb[3 * i + j]! + yi * xB[at + j]!;
        }
      }
  }

  // The hidden match: the permutation whose best scales leave the least of A's activation energy unexplained.
  let perm = PERMS[0]!;
  let scale: [number, number, number] = [1, 1, 1];
  let best = Number.POSITIVE_INFINITY;
  for (const p of PERMS) {
    const d: [number, number, number] = [1, 1, 1];
    let cost = 0;
    for (let j = 0; j < 3; j++) {
      const i = p[j]!;
      const dj = hbb[j] === 0 ? 1 : Math.min(SCALE_MAX, Math.max(1 / SCALE_MAX, hab[3 * i + j]! / hbb[j]!));
      d[j] = dj;
      cost += haa[i]! - 2 * dj * hab[3 * i + j]! + dj * dj * hbb[j]!;
    }
    if (cost < best) {
      best = cost;
      perm = p;
      scale = d;
    }
  }

  // The residual fit: least squares xA = M xB, or the identity when B's layer inputs do not span the space.
  let m = mul(xab, inverse(xbb));
  const trace = xbb[0]! + xbb[4]! + xbb[8]!;
  const usable =
    m.every(Number.isFinite) &&
    Math.abs(det(xbb)) >= 1e-12 * (trace / 3) ** 3 &&
    Math.abs(det(m)) >= 1e-6 &&
    Math.hypot(...m) * Math.hypot(...inverse(m)) <= 1e4;
  if (!usable) m = I3;

  const fitted: MorphTransform = { perm, scale, m };
  const alignedError = blendError(a, b, fitted, s, outA, outB);
  const unalignedError = blendError(a, b, IDENTITY, s, outA, outB);
  const fitOf = (error: number): number => Math.min(1, Math.max(0, 1 - error));
  return alignedError < unalignedError
    ? { ...fitted, fit: fitOf(alignedError), unalignedFit: fitOf(unalignedError) }
    : { ...IDENTITY, fit: fitOf(unalignedError), unalignedFit: fitOf(unalignedError) };
}
