// The pedal's parameters and control slots as hosts show them (daisy/src/params.h; docs/MIDI.md "Parameters",
// "Controls"): a parameter's wire code as its value, its units and its readout, and which control takes which
// parameter. Pure TypeScript: shared by the editor and tools/pedal.ts. The table itself comes from the pedal (DUMP).

import { fmtHz, signedDb } from "../format";
import type { ParamInfo } from "./protocol";

/** A continuous parameter's top code (params.h kCodeMax). */
export const CODE_MAX = 16383;
/** filters.h kBypassEnter: a filter is off within this of its off end (the 0.02-0.04 hysteresis is not shown). */
const BYPASS = 0.02;

export interface Control {
  /** The pedal's legend: "K3" ... "SW3". */
  id: string;
  /** The log's and tools/pedal.ts's name: "k3" ... "sw3". */
  key: string;
  knob: boolean;
}

/** The control slots, by their wire index 0-6: Knobs 3-6, then SW1-SW3. Knobs 1 and 2 are fixed. */
export const CONTROLS: readonly Control[] = [
  { id: "K3", key: "k3", knob: true },
  { id: "K4", key: "k4", knob: true },
  { id: "K5", key: "k5", knob: true },
  { id: "K6", key: "k6", knob: true },
  { id: "SW1", key: "sw1", knob: false },
  { id: "SW2", key: "sw2", knob: false },
  { id: "SW3", key: "sw3", knob: false },
];

/** The value of a code: a continuous parameter's position 0..1 along its taper, a choice's index. */
export const paramValue = (p: ParamInfo, code: number): number =>
  p.taper === "choice" ? Math.min(code, p.max) : Math.min(code, CODE_MAX) / CODE_MAX;

/** paramValue's inverse, clamped and rounded. */
export const codeOf = (p: ParamInfo, value: number): number =>
  p.taper === "choice"
    ? Math.round(Math.min(Math.max(value, 0), p.max))
    : Math.round(Math.min(Math.max(value, 0), 1) * CODE_MAX);

/** A continuous parameter at position norm (0..1), in its unit: log or linear from min to max. */
export const unitsOf = (p: ParamInfo, norm: number): number =>
  p.taper === "linear" ? p.min + norm * (p.max - p.min) : p.min * (p.max / p.min) ** norm;

/** unitsOf's inverse as a code: units clamped to min..max, rounded to a code 0..CODE_MAX. */
export const codeOfUnits = (p: ParamInfo, units: number): number => {
  const u = Math.min(Math.max(units, p.min), p.max);
  const norm = p.taper === "linear" ? (u - p.min) / (p.max - p.min) : Math.log(u / p.min) / Math.log(p.max / p.min);
  return codeOf(p, norm);
};

/** The parameter's filter is bypassed at this code: within BYPASS of its off end. */
export function isOff(p: ParamInfo, code: number): boolean {
  if (p.taper === "choice") return false;
  const norm = paramValue(p, code);
  return (p.off === "min" && norm <= BYPASS) || (p.off === "max" && norm >= 1 - BYPASS);
}

/** A choice's labels, in index order. */
export const choiceLabels = (p: ParamInfo): string[] => p.unit.split(",");

/** The readout of a code: "Off", a choice's label, "100 Hz", "Q 1.00", "+3.0 dB", "50 %". */
export function formatParam(p: ParamInfo, code: number): string {
  if (isOff(p, code)) return "Off";
  if (p.taper === "choice") return choiceLabels(p)[paramValue(p, code)] ?? String(code);
  const units = unitsOf(p, paramValue(p, code));
  if (p.unit === "Hz") return fmtHz(units);
  if (p.unit === "Q") return `Q ${units.toFixed(2)}`;
  if (p.unit === "dB") return signedDb(units);
  if (p.unit === "%") return `${Math.round(units)} %`;
  return `${units.toFixed(2)} ${p.unit}`;
}

/** Control `control` (0-6) may hold p: a knob any parameter, a switch a choice; null (none) always. */
export const accepts = (control: number, p: ParamInfo | null): boolean =>
  p === null || (CONTROLS[control]?.knob ?? false) || p.taper === "choice";

/**
 * A choice's labels by toggle position, down first: the index is STATUS's position (0 down, 1 middle, 2 up). Three
 * values: the index is the position. Two values: 0 in the middle, 1 up or down.
 */
export function switchLabels(p: ParamInfo): [string, string, string] {
  const l = choiceLabels(p);
  return l.length >= 3 ? [l[0]!, l[1]!, l[2]!] : [l[1] ?? "", l[0] ?? "", l[1] ?? ""];
}

/** A toggle's positions as the editor captions them. */
export const POSITION = { up: "↑ up", middle: "– middle", down: "↓ down", upOrDown: "↑↓ up or down" } as const;

/**
 * The toggle position that selects each of a choice's values, in index order (params.h FromToggle): three values
 * down, middle, up; two values middle (the first), then up or down.
 */
export const togglePositions = (p: ParamInfo): string[] =>
  choiceLabels(p).length >= 3 ? [POSITION.down, POSITION.middle, POSITION.up] : [POSITION.middle, POSITION.upOrDown];

const labelKey = (text: string): string => text.toLowerCase().replaceAll(" ", "").replaceAll("−", "-");

/**
 * The code a typed value means, or null when it is not one of p's. A choice: its index or its label (case, spaces
 * and the minus sign's form ignored). A continuous parameter: a number in its unit within min..max (as the pedal's
 * single-precision ends), or "off" when it has an off end.
 */
export function parseParamValue(p: ParamInfo, text: string): number | null {
  const t = text.trim();
  if (p.taper === "choice") {
    if (/^\d+$/.test(t)) return Number(t) <= p.max ? Number(t) : null;
    const i = choiceLabels(p).findIndex((l) => labelKey(l) === labelKey(t));
    return i < 0 ? null : i;
  }
  if (t.toLowerCase() === "off") return p.off === "min" ? 0 : p.off === "max" ? CODE_MAX : null;
  const v = Number(t.replace(/^−/, "-"));
  if (t === "" || !Number.isFinite(v) || Math.fround(v) < p.min || Math.fround(v) > p.max) return null;
  return codeOfUnits(p, v);
}
