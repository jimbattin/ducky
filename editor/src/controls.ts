// The pedal's controls as the sidebar and the EQ graph show them: each knob's readout in its unit, the switches'
// positions, and the model's level-match and input calibration. The tapers are the firmware's: daisy/src/dsp/gain.h
// (K1, K2, the calibration) and daisy/src/dsp/filters.h (K3-K6, SW1-SW3). k is the knob position 0..1 (STATUS
// permille / 1000); a toggle is STATUS's position (0 down, 1 middle, 2 up).

import { fmtHz, signedDb } from "./format";

export interface Knob {
  id: string;
  label: string;
  readout: (k: number) => string;
}

export interface Switch {
  id: string;
  label: string;
  /** Appended to the accessible name. */
  unit?: string;
  /** Up, middle, down. */
  options: readonly [string, string, string];
}

/** gain.h kTrimRangeDb: K1's range and K2's top. */
const TRIM_DB = 12;
/** filters.h kBypassEnter: a cut filter is off within this of its off end (the 0.02-0.04 hysteresis is not shown). */
const BYPASS = 0.02;
const LOW_CUT_MIN_HZ = 20;
const LOW_CUT_MAX_HZ = 500;
const HIGH_CUT_MIN_HZ = 1500;
const HIGH_CUT_MAX_HZ = 20000;
const RES_MIN_Q = 0.5;
const RES_MAX_Q = 2;
/** gain.h kLevelTargetDb, kInputLevelDbu, kDefaultModelInputDbu. */
const LEVEL_TARGET_DB = -18;
const INPUT_LEVEL_DBU = 4.5;
export const DEFAULT_MODEL_INPUT_DBU = 12;
/** filters.h kMidHz, kMidBoostDb, kMidCutDb: SW3's bell. */
export const MID_HZ = 515;
const MID_BOOST_DB = 5;
const MID_CUT_DB = -7;

/** Knob i's position 0..1 from STATUS's knobs (permille); 0 when absent. */
export const knobPosition = (knobs: readonly number[], i: number): number => (knobs[i] ?? 0) / 1000;
/** K5: 20-500 Hz, log. */
export const lowCutHz = (k: number): number => LOW_CUT_MIN_HZ * (LOW_CUT_MAX_HZ / LOW_CUT_MIN_HZ) ** k;
/** K4: 1.5-20 kHz, log. */
export const highCutHz = (k: number): number => HIGH_CUT_MIN_HZ * (HIGH_CUT_MAX_HZ / HIGH_CUT_MIN_HZ) ** k;
/** K6, K3: Q 0.5-2, log; 1 at noon. */
export const resonanceQ = (k: number): number => RES_MIN_Q * (RES_MAX_Q / RES_MIN_Q) ** k;
/** The low-cut is bypassed: K5 at its counter-clockwise end. */
export const lowCutOff = (k: number): boolean => k <= BYPASS;
/** The high-cut is bypassed: K4 at its clockwise end. */
export const highCutOff = (k: number): boolean => k >= 1 - BYPASS;
/** K2's gain in dB: (2k)^3 up to noon (-Infinity at 0), then dB linear to +12 dB. */
export const outputDb = (k: number): number => (k <= 0.5 ? 60 * Math.log10(2 * k) : TRIM_DB * (2 * k - 1));

export type Slope = 6 | 12 | 24;
/** SW1, SW2: up 6, middle 12, down 24 dB/oct. */
export const slopeOf = (toggle: number): Slope => (toggle >= 2 ? 6 : toggle === 1 ? 12 : 24);
/** SW3: up +5 dB, middle 0 (bypassed), down -7 dB. */
export const midGainDb = (toggle: number): number => (toggle >= 2 ? MID_BOOST_DB : toggle === 1 ? 0 : MID_CUT_DB);

const resonance = (k: number): string => `Q ${resonanceQ(k).toFixed(2)}`;

export const KNOBS: readonly Knob[] = [
  { id: "K1", label: "Input trim", readout: (k) => signedDb(TRIM_DB * (2 * k - 1)) },
  { id: "K2", label: "Output volume", readout: (k) => (k === 0 ? "Off" : signedDb(outputDb(k))) },
  { id: "K3", label: "High-cut resonance", readout: resonance },
  { id: "K4", label: "High-cut", readout: (k) => (highCutOff(k) ? "Off" : fmtHz(highCutHz(k))) },
  { id: "K5", label: "Low-cut", readout: (k) => (lowCutOff(k) ? "Off" : fmtHz(lowCutHz(k))) },
  { id: "K6", label: "Low-cut resonance", readout: resonance },
];

/** A resonance knob does something: its filter's slope is not 6 dB/oct and the filter is not off. */
export function resonanceActive(freqK: number, toggle: number, lowCut: boolean): boolean {
  if (slopeOf(toggle) === 6) return false;
  return !(lowCut ? lowCutOff(freqK) : highCutOff(freqK));
}

export const SWITCHES: readonly Switch[] = [
  { id: "SW1", label: "Low-cut slope", unit: "dB/oct", options: ["6", "12", "24"] },
  { id: "SW2", label: "High-cut slope", unit: "dB/oct", options: ["6", "12", "24"] },
  { id: "SW3", label: `Mid ${MID_HZ} Hz`, options: [`+${MID_BOOST_DB} dB`, "Off", `−${-MID_CUT_DB} dB`] },
];

/** STATUS's toggle (0 down, 1 middle, 2 up) as an index into a Switch's options (up first). */
export const positionIndex = (toggle: number): number => 2 - toggle;

/** The model's level-match gain in dB (to -18 dB loudness); null without a loudness: unity. */
export const levelMatchDb = (loudness: number | null): number | null =>
  loudness === null ? null : LEVEL_TARGET_DB - loudness;

/** The input calibration in dB at K1's noon: the pedal's +4.5 dBu input against the model's capture level. */
export const inputCalDb = (inputLevelDbu: number | null): number =>
  INPUT_LEVEL_DBU - (inputLevelDbu ?? DEFAULT_MODEL_INPUT_DBU);
