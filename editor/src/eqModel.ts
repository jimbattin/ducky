// The pedal's EQ and output volume as a frequency response, from the EQ's parameters (the pedal's VALUES or the
// editor's edits) and Knob 2 (docs/EQ.md): low-cut, bells 1-3 and high-cut in series, then the output volume. Every
// section is a TPT filter, so its response is the analog prototype's at u = tan(pi f / fs) / tan(pi f0 / fs), exactly
// the pedal's at its rate (tests/eq_model.h is the same model). The editor's EQ graph draws it; eqModel.test.ts
// checks the shapes.
import { outputDb, type Slope } from "./controls";
import { SAMPLE_RATE } from "./device";
import { CODE_MAX, isOff, paramValue, unitsOf } from "./link/params";
import type { ParamInfo } from "./link/protocol";

/** filters.h SlopeFromToggle: a slope parameter's index (the toggle position: 0 down, 2 up) as dB/oct. */
export const SLOPE_OF_INDEX: readonly Slope[] = [24, 12, 6];

/** filters.h kButterworthQ: the 24 dB slope's first stage. */
const BUTTERWORTH_Q = Math.SQRT1_2;

export type BandId = "low" | "bell1" | "bell2" | "bell3" | "high";
export type BandKind = "lowCut" | "bell" | "highCut";
export interface Band {
  id: BandId;
  /** The graph handle's letter. */
  letter: string;
  label: string;
  kind: BandKind;
  /** Its parameters' names: `${prefix}_freq`, `_res`, `_slope` (cuts); `_gain`, `_freq`, `_q` (bells). */
  prefix: string;
}

/** The bands in processing order (eq.h). */
export const BANDS: readonly Band[] = [
  { id: "low", letter: "L", label: "Low-cut", kind: "lowCut", prefix: "low_cut" },
  { id: "bell1", letter: "1", label: "Bell 1", kind: "bell", prefix: "bell1" },
  { id: "bell2", letter: "2", label: "Bell 2", kind: "bell", prefix: "bell2" },
  { id: "bell3", letter: "3", label: "Bell 3", kind: "bell", prefix: "bell3" },
  { id: "high", letter: "H", label: "High-cut", kind: "highCut", prefix: "high_cut" },
];

/** A band's parameter names, in the EQ tab's order. */
export const bandParamNames = (b: Band): string[] =>
  (b.kind === "bell" ? ["gain", "freq", "q"] : ["freq", "res", "slope"]).map((s) => `${b.prefix}_${s}`);

/** A cut filter in use: its cutoff (Hz), resonance Q (unused at 6 dB/oct) and slope. */
export interface Cut {
  hz: number;
  q: number;
  slope: Slope;
}

/** A bell: gain (dB; 0 when off), centre frequency (Hz) and Q. */
export interface Bell {
  gain: number;
  hz: number;
  q: number;
}

export interface EqSettings {
  /** eq_enable: off, the EQ passes everything. */
  enabled: boolean;
  /** null: bypassed (the low-cut frequency at its bottom end). */
  low: Cut | null;
  bells: [Bell, Bell, Bell];
  /** null: bypassed (the high-cut frequency at its top end). */
  high: Cut | null;
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
  prefix: string,
): { freq: Coded; res: Coded; slope: Coded } | null {
  const freq = coded(params, codes, `${prefix}_freq`);
  const res = coded(params, codes, `${prefix}_res`);
  const slope = coded(params, codes, `${prefix}_slope`);
  return freq && res && slope ? { freq, res, slope } : null;
}

const units = ({ p, code }: Coded): number => unitsOf(p, paramValue(p, code));

/**
 * The settings from the pedal's parameter table and codes and Knob 2's position 0..1; null when the table or the
 * codes lack an EQ parameter. A bell gain within one code step of 0 dB is 0, as eq.h has it (code 8192 is +0.0007 dB).
 */
export function readEqSettings(
  params: readonly ParamInfo[],
  codes: readonly number[],
  outKnob: number,
): EqSettings | null {
  const enable = coded(params, codes, "eq_enable");
  const low = cutParams(params, codes, "low_cut");
  const high = cutParams(params, codes, "high_cut");
  const bells: Bell[] = [];
  for (const b of BANDS) {
    if (b.kind !== "bell") continue;
    const [gain, freq, q] = bandParamNames(b).map((n) => coded(params, codes, n));
    if (!gain || !freq || !q) return null;
    const g = units(gain);
    bells.push({ gain: Math.abs(g) < (gain.p.max - gain.p.min) / CODE_MAX ? 0 : g, hz: units(freq), q: units(q) });
  }
  if (!enable || !low || !high) return null;
  const cut = (c: NonNullable<typeof low>): Cut | null =>
    isOff(c.freq.p, c.freq.code)
      ? null
      : { hz: units(c.freq), q: units(c.res), slope: SLOPE_OF_INDEX[c.slope.code] ?? 12 };
  return {
    enabled: enable.code !== 0,
    low: cut(low),
    bells: bells as [Bell, Bell, Bell],
    high: cut(high),
    outDb: outputDb(outKnob),
  };
}

/** The resonance parameter called name does nothing: its filter's slope is 6 dB/oct or the filter is off. */
export function resonanceInactive(params: readonly ParamInfo[], codes: readonly number[], name: string): boolean {
  const prefix = name === "low_cut_res" ? "low_cut" : name === "high_cut_res" ? "high_cut" : null;
  const c = prefix === null ? null : cutParams(params, codes, prefix);
  return c !== null && (SLOPE_OF_INDEX[c.slope.code] === 6 || isOff(c.freq.p, c.freq.code));
}

/** u: the frequency f relative to f0, prewarped. */
const ratio = (f: number, f0: number): number =>
  Math.tan((Math.PI * f) / SAMPLE_RATE) / Math.tan((Math.PI * f0) / SAMPLE_RATE);
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

/** A bell alone in dB at f (Hz): the RBJ bell (s^2 + s A / Q + 1) / (s^2 + s / (A Q) + 1), A = 10^(gain / 40). */
export function bellDb(b: Bell, f: number): number {
  if (b.gain === 0) return 0;
  const A = 10 ** (b.gain / 40);
  const u = ratio(f, b.hz);
  return 10 * Math.log10(quad2(1, A / b.q, 1, u) / quad2(1, 1 / (A * b.q), 1, u));
}

/** Each band alone and the whole chain (dB) at freqs: a bypassed band is 0 dB; disabled, every band is. */
export function eqCurves(s: EqSettings, freqs: readonly number[]): Record<BandId | "total", number[]> {
  const zero = freqs.map(() => 0);
  const cut = (c: Cut | null, hp: boolean) => (s.enabled && c ? freqs.map((f) => cutDb(c, hp, f)) : zero);
  const bell = (b: Bell) => (s.enabled ? freqs.map((f) => bellDb(b, f)) : zero);
  const bands = {
    low: cut(s.low, true),
    bell1: bell(s.bells[0]),
    bell2: bell(s.bells[1]),
    bell3: bell(s.bells[2]),
    high: cut(s.high, false),
  };
  const total = freqs.map((_, i) => BANDS.reduce((sum, b) => sum + bands[b.id][i]!, s.outDb));
  return { ...bands, total };
}

/** The whole chain in dB at f (Hz). */
export const eqTotalDb = (s: EqSettings, f: number): number => eqCurves(s, [f]).total[0]!;
