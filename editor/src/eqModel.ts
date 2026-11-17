// The pedal's EQ and output volume as a frequency response, from the EQ's parameters (the pedal's VALUES) and Knob 2
// (docs/FILTERS.md "Filters"): low-cut, the mid bell and high-cut in series, then the output volume. Every section
// is a TPT filter, so its response is the analog prototype's at u = tan(pi f / fs) / tan(pi fc / fs), exactly the
// pedal's at its rate. The editor's EQ graph draws it; eqModel.test.ts checks the shapes.
import { MID_HZ, outputDb, type Slope } from "./controls";
import { SAMPLE_RATE } from "./device";
import { isOff, paramValue, unitsOf } from "./link/params";
import type { ParamInfo } from "./link/protocol";

/** filters.h SlopeFromToggle: a slope parameter's index (the toggle position: 0 down, 2 up) as dB/oct. */
export const SLOPE_OF_INDEX: readonly Slope[] = [24, 12, 6];
/** filters.h MidGainDb: the mid parameter's index as the bell's gain in dB. */
export const MID_DB_OF_INDEX: readonly number[] = [-7, 0, 5];

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
  /** null: bypassed (the low-cut frequency at its bottom end). */
  lowCut: Cut | null;
  /** The mid bell's gain in dB; 0 bypassed. */
  midDb: number;
  /** null: bypassed (the high-cut frequency at its top end). */
  highCut: Cut | null;
  /** Knob 2's gain in dB; -Infinity at 0. */
  outDb: number;
}

interface Coded {
  p: ParamInfo;
  code: number;
}

/** The parameter called name with its code; null when the table or the codes lack it. */
function coded(params: readonly ParamInfo[], codes: readonly number[], name: string): Coded | null {
  const p = params.find((x) => x.name === name);
  const code = p === undefined ? undefined : codes[p.id];
  return p === undefined || code === undefined ? null : { p, code };
}

/** A cut filter's three parameters; null when any is missing. */
function cutParams(
  params: readonly ParamInfo[],
  codes: readonly number[],
  prefix: "low_cut" | "high_cut",
): { freq: Coded; res: Coded; slope: Coded } | null {
  const freq = coded(params, codes, `${prefix}_freq`);
  const res = coded(params, codes, `${prefix}_res`);
  const slope = coded(params, codes, `${prefix}_slope`);
  return freq && res && slope ? { freq, res, slope } : null;
}

/**
 * The settings from the pedal's parameter table and codes (VALUES) and Knob 2's position 0..1; null when the table
 * or the codes lack an EQ parameter.
 */
export function readEqSettings(
  params: readonly ParamInfo[],
  codes: readonly number[],
  outKnob: number,
): EqSettings | null {
  const low = cutParams(params, codes, "low_cut");
  const high = cutParams(params, codes, "high_cut");
  const mid = coded(params, codes, "mid");
  if (!low || !high || !mid) return null;
  const units = ({ p, code }: Coded): number => unitsOf(p, paramValue(p, code));
  const cut = (c: NonNullable<typeof low>): Cut | null =>
    isOff(c.freq.p, c.freq.code)
      ? null
      : { hz: units(c.freq), q: units(c.res), slope: SLOPE_OF_INDEX[c.slope.code] ?? 12 };
  return { lowCut: cut(low), midDb: MID_DB_OF_INDEX[mid.code] ?? 0, highCut: cut(high), outDb: outputDb(outKnob) };
}

/** The resonance parameter called name does nothing: its filter's slope is 6 dB/oct or the filter is off. */
export function resonanceInactive(params: readonly ParamInfo[], codes: readonly number[], name: string): boolean {
  const prefix = name === "low_cut_res" ? "low_cut" : name === "high_cut_res" ? "high_cut" : null;
  const c = prefix === null ? null : cutParams(params, codes, prefix);
  return c !== null && (SLOPE_OF_INDEX[c.slope.code] === 6 || isOff(c.freq.p, c.freq.code));
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
