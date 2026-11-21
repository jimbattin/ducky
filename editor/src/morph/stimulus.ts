// The signal the morph alignment (align.ts) drives both models with: eight plucked strings across the guitar's range,
// loud and quiet in turn, so the fit sees the models' clean and driven behaviour. Deterministic: the same samples on
// every run and host. Pure TypeScript.

import { SAMPLE_RATE } from "../device";

const LENGTH = 2 * SAMPLE_RATE;
const SPACING = SAMPLE_RATE / 4;
const FREQS = [82.41, 110, 146.83, 196, 246.94, 329.63, 164.81, 220];
const LOUD = 0.5;
const QUIET = 0.125;
const DECAY = 0.996;
const SEED = 0x5eed;

function mulberry32(seed: number): () => number {
  let a = seed;
  return () => {
    a = (a + 0x6d2b79f5) | 0;
    let t = Math.imul(a ^ (a >>> 15), a | 1);
    t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

/** 2 s at 48 kHz: Karplus-Strong plucks every 250 ms, each normalised to its peak (0.5 and 0.125 in turn). */
export function morphStimulus(): Float64Array {
  const out = new Float64Array(LENGTH);
  const rand = mulberry32(SEED);
  const pluck = new Float64Array(LENGTH);
  FREQS.forEach((f, i) => {
    const onset = SPACING * i;
    const period = Math.round(SAMPLE_RATE / f);
    const buf = new Float64Array(period);
    for (let k = 0; k < period; k++) buf[k] = 2 * rand() - 1;
    let peak = 0;
    for (let n = onset; n < LENGTH; n++) {
      const j = (n - onset) % period;
      const y = buf[j]!;
      buf[j] = DECAY * 0.5 * (y + buf[(j + 1) % period]!);
      pluck[n] = y;
      peak = Math.max(peak, Math.abs(y));
    }
    const gain = (i % 2 === 0 ? LOUD : QUIET) / peak;
    for (let n = onset; n < LENGTH; n++) out[n] = out[n]! + gain * pluck[n]!;
  });
  return out;
}
