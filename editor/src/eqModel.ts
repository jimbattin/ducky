// The pedal's EQ and output volume as a frequency response, from STATUS's knobs and toggles (docs/FILTERS.md
// "Filters"): low-cut, the SW3 mid bell and high-cut in series, then Knob 2. Every section is a TPT filter, so its
// response is the analog prototype's at u = tan(pi f / fs) / tan(pi fc / fs), exactly the pedal's at its rate. The
// editor's EQ graph draws it; eqModel.test.ts checks the shapes.
import {
  highCutHz,
  highCutOff,
  knobPosition,
  lowCutHz,
  lowCutOff,
  MID_HZ,
  midGainDb,
  outputDb,
  resonanceQ,
  type Slope,
  slopeOf,
} from "./controls";
import { SAMPLE_RATE } from "./device";

/** filters.h kButterworthQ: the 24 dB slope's first stage. */
const BUTTERWORTH_Q = Math.SQRT1_2;
/** filters.h kMidQ. */
const MID_Q = 1;

/** A cut filter in use: its cutoff (Hz), resonance Q (unused at 6 dB/oct) and slope. */
export interface Cut {
  hz: number;
  q: number;
  slope: Slope;
}

export interface EqSettings {
  /** null: bypassed (Knob 5 fully counter-clockwise). */
  lowCut: Cut | null;
  /** SW3's bell gain in dB; 0 bypassed. */
  midDb: number;
  /** null: bypassed (Knob 4 fully clockwise). */
  highCut: Cut | null;
  /** Knob 2's gain in dB; -Infinity at 0. */
  outDb: number;
}

/** The settings from STATUS's knobs (permille, Knobs 1-6) and toggles (SW1-SW3). */
export function readEqSettings(knobs: readonly number[], toggles: readonly number[]): EqSettings {
  const k = (i: number): number => knobPosition(knobs, i);
  const t = (i: number): number => toggles[i] ?? 1;
  return {
    lowCut: lowCutOff(k(4)) ? null : { hz: lowCutHz(k(4)), q: resonanceQ(k(5)), slope: slopeOf(t(0)) },
    midDb: midGainDb(t(2)),
    highCut: highCutOff(k(3)) ? null : { hz: highCutHz(k(3)), q: resonanceQ(k(2)), slope: slopeOf(t(1)) },
    outDb: outputDb(k(1)),
  };
}

/** u: the frequency f relative to fc, prewarped. */
const ratio = (f: number, fc: number): number =>
  Math.tan((Math.PI * f) / SAMPLE_RATE) / Math.tan((Math.PI * fc) / SAMPLE_RATE);
/** |a s^2 + b s + c|^2 at s = ju. */
const quad2 = (a: number, b: number, c: number, u: number): number => (c - a * u * u) ** 2 + (b * u) ** 2;
/** A one-pole's power gain at u. */
const order1 = (u: number, hp: boolean): number => (hp ? u * u : 1) / (1 + u * u);
/** A second-order low- or high-pass's power gain at u. */
const order2 = (u: number, q: number, hp: boolean): number => (hp ? u ** 4 : 1) / quad2(1, 1 / q, 1, u);

/** A cut filter alone in dB at f (Hz); hp: the low-cut. */
export function cutDb(c: Cut, hp: boolean, f: number): number {
  const u = ratio(f, c.hz);
  const power =
    c.slope === 6
      ? order1(u, hp)
      : c.slope === 12
        ? order2(u, c.q, hp)
        : order2(u, BUTTERWORTH_Q, hp) * order2(u, c.q, hp);
  return 10 * Math.log10(power);
}

/** The mid bell alone in dB at f (Hz): constant-Q, A = 10^(dB/40), 1 + (A^2 - 1) k s / (s^2 + k s + 1), k = 1/(Q A). */
export function midDb(db: number, f: number): number {
  if (db === 0) return 0;
  const A = 10 ** (db / 40);
  const u = ratio(f, MID_HZ);
  return 10 * Math.log10(quad2(1, A / MID_Q, 1, u) / quad2(1, 1 / (A * MID_Q), 1, u));
}

export interface Curves {
  low: number[];
  mid: number[];
  high: number[];
  /** The three sections and the output volume. */
  total: number[];
}

/** Each section alone and the whole chain (dB) at freqs; a bypassed section is 0 dB. */
export function eqCurves(s: EqSettings, freqs: readonly number[]): Curves {
  const { lowCut, highCut } = s;
  const low = freqs.map((f) => (lowCut ? cutDb(lowCut, true, f) : 0));
  const mid = freqs.map((f) => midDb(s.midDb, f));
  const high = freqs.map((f) => (highCut ? cutDb(highCut, false, f) : 0));
  const total = freqs.map((_, i) => low[i]! + mid[i]! + high[i]! + s.outDb);
  return { low, mid, high, total };
}

/** The whole chain in dB at f (Hz). */
export const eqTotalDb = (s: EqSettings, f: number): number => eqCurves(s, [f]).total[0]!;
