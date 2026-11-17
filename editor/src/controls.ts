// The pedal's fixed controls as the sidebar and the EQ graph show them: Knob 1's and Knob 2's readouts in their units,
// a toggle's position, and the model's level-match and input calibration. The tapers are the firmware's
// (daisy/src/dsp/gain.h). Knobs 3-6 and SW1-SW3 drive parameters: link/params.ts. k is the knob position 0..1 (STATUS
// permille / 1000); a toggle is STATUS's position (0 down, 1 middle, 2 up).

import { signedDb } from "./format";

export interface Knob {
  id: string;
  label: string;
  readout: (k: number) => string;
}

/** gain.h kTrimRangeDb: K1's range and K2's top. */
const TRIM_DB = 12;
/** gain.h kLevelTargetDb, kInputLevelDbu, kDefaultModelInputDbu. */
const LEVEL_TARGET_DB = -18;
const INPUT_LEVEL_DBU = 4.5;
export const DEFAULT_MODEL_INPUT_DBU = 12;

/** Knob i's position 0..1 from STATUS's knobs (permille); 0 when absent. */
export const knobPosition = (knobs: readonly number[], i: number): number => (knobs[i] ?? 0) / 1000;
/** K2's gain in dB: (2k)^3 up to noon (-Infinity at 0), then dB linear to +12 dB. */
export const outputDb = (k: number): number => (k <= 0.5 ? 60 * Math.log10(2 * k) : TRIM_DB * (2 * k - 1));

/** A cut filter's slope in dB/oct. */
export type Slope = 6 | 12 | 24;

/** Knobs 1 and 2: no parameter is assigned to them. */
export const FIXED_KNOBS: readonly Knob[] = [
  { id: "K1", label: "Input trim", readout: (k) => signedDb(TRIM_DB * (2 * k - 1)) },
  { id: "K2", label: "Output volume", readout: (k) => (k === 0 ? "Off" : signedDb(outputDb(k))) },
];

/** The model's level-match gain in dB (to -18 dB loudness); null without a loudness: unity. */
export const levelMatchDb = (loudness: number | null): number | null =>
  loudness === null ? null : LEVEL_TARGET_DB - loudness;

/** The input calibration in dB at K1's noon: the pedal's +4.5 dBu input against the model's capture level. */
export const inputCalDb = (inputLevelDbu: number | null): number =>
  INPUT_LEVEL_DBU - (inputLevelDbu ?? DEFAULT_MODEL_INPUT_DBU);
