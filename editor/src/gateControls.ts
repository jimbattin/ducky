// The noise gate's settings as the gate box's sliders: labels, tapers, resolutions and readouts (preset/gate.ts is the
// stored block).
import { fixed } from "./format";
import type { GateSettings } from "./preset/gate";

/** One of the gate box's sliders. */
export interface GateParam {
  key: Exclude<keyof GateSettings, "enabled">;
  label: string;
  min: number;
  max: number;
  log: boolean;
  step: number;
  format: (v: number) => string;
}

export const GATE_PARAMS: readonly GateParam[] = [
  {
    key: "thresholdDb",
    label: "Threshold",
    min: -80,
    max: -30,
    log: false,
    step: 1,
    format: (v) => `${fixed(v, 0)} dBFS`,
  },
  { key: "attackMs", label: "Attack", min: 0.5, max: 10, log: true, step: 0.1, format: (v) => `${v.toFixed(1)} ms` },
  { key: "holdMs", label: "Hold", min: 10, max: 100, log: true, step: 1, format: (v) => `${v} ms` },
  { key: "releaseMs", label: "Release", min: 10, max: 1000, log: true, step: 1, format: (v) => `${v} ms` },
  { key: "ratio", label: "Ratio", min: 1.5, max: 10, log: true, step: 0.1, format: (v) => `${v.toFixed(1)}:1` },
];

/** v rounded to the parameter's step and kept inside its range. */
const snap = (p: GateParam, v: number): number =>
  Math.min(p.max, Math.max(p.min, Number((Math.round(v / p.step) * p.step).toFixed(p.step < 1 ? 1 : 0))));

/** A value's slider position, 0..1. */
export function gateNorm(p: GateParam, v: number): number {
  const n = p.log ? Math.log(v / p.min) / Math.log(p.max / p.min) : (v - p.min) / (p.max - p.min);
  return Math.min(1, Math.max(0, n));
}

/** A slider position's value, rounded to the parameter's step and kept inside its range. */
export function gateValue(p: GateParam, norm: number): number {
  const n = Math.min(1, Math.max(0, norm));
  return snap(p, p.log ? p.min * (p.max / p.min) ** n : p.min + (p.max - p.min) * n);
}

/** One step of the parameter's resolution up (dir 1) or down (−1) from v, kept inside its range: the arrow keys. */
export const gateStep = (p: GateParam, v: number, dir: 1 | -1): number => snap(p, v + dir * p.step);
