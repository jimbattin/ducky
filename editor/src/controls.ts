// The pedal's controls as the sidebar shows them: each knob's readout in its unit, the switches' positions, and the
// model's level-match and input calibration. The tapers are the firmware's: daisy/src/dsp/gain.h (K1, K2, the
// calibration) and daisy/src/dsp/filters.h (K3-K6, SW1-SW3). k is the knob position 0..1 (STATUS permille / 1000).

export interface Knob {
  id: string;
  label: string;
  readout: (k: number) => string;
}

export interface Switch {
  id: string;
  label: string;
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
const DEFAULT_MODEL_INPUT_DBU = 12;
/** A slope toggle's position for 6 dB/oct (up), where the resonance knob does nothing. */
const SLOPE_6DB = 2;

export const signedDb = (x: number): string => `${x >= 0 ? "+" : "−"}${Math.abs(x).toFixed(1)} dB`;

export const fmtHz = (f: number): string =>
  f < 1000 ? `${Math.round(f)} Hz` : `${(f / 1000).toFixed(f < 10000 ? 2 : 1)} kHz`;

const resonance = (k: number): string => `Q ${(RES_MIN_Q * (RES_MAX_Q / RES_MIN_Q) ** k).toFixed(2)}`;
const lowCutOff = (k: number): boolean => k <= BYPASS;
const highCutOff = (k: number): boolean => k >= 1 - BYPASS;

export const KNOBS: readonly Knob[] = [
  { id: "K1", label: "Input trim", readout: (k) => signedDb(TRIM_DB * (2 * k - 1)) },
  {
    id: "K2",
    label: "Output volume",
    // (2k)^3 up to noon, then dB linear to +12 dB.
    readout: (k) => (k === 0 ? "Off" : k <= 0.5 ? signedDb(60 * Math.log10(2 * k)) : signedDb(TRIM_DB * (2 * k - 1))),
  },
  { id: "K3", label: "High-cut resonance", readout: resonance },
  {
    id: "K4",
    label: "High-cut",
    readout: (k) => (highCutOff(k) ? "Off" : fmtHz(HIGH_CUT_MIN_HZ * (HIGH_CUT_MAX_HZ / HIGH_CUT_MIN_HZ) ** k)),
  },
  {
    id: "K5",
    label: "Low-cut",
    readout: (k) => (lowCutOff(k) ? "Off" : fmtHz(LOW_CUT_MIN_HZ * (LOW_CUT_MAX_HZ / LOW_CUT_MIN_HZ) ** k)),
  },
  { id: "K6", label: "Low-cut resonance", readout: resonance },
];

/** A resonance knob does something: its filter's slope is not 6 dB/oct and the filter is not off. */
export function resonanceActive(freqK: number, toggle: number, lowCut: boolean): boolean {
  if (toggle === SLOPE_6DB) return false;
  return !(lowCut ? lowCutOff(freqK) : highCutOff(freqK));
}

export const SWITCHES: readonly Switch[] = [
  { id: "SW1", label: "Low-cut slope", options: ["6", "12", "24"] },
  { id: "SW2", label: "High-cut slope", options: ["6", "12", "24"] },
  { id: "SW3", label: "Mid 515 Hz", options: ["+5 dB", "Off", "−7 dB"] },
];

/** STATUS's toggle (0 down, 1 middle, 2 up) as an index into a Switch's options (up first). */
export const positionIndex = (toggle: number): number => 2 - toggle;

/** The model's level-match gain in dB (to -18 dB loudness); null without a loudness: unity. */
export const levelMatchDb = (loudness: number | null): number | null =>
  loudness === null ? null : LEVEL_TARGET_DB - loudness;

/** The input calibration in dB at K1's noon: the pedal's +4.5 dBu input against the model's capture level. */
export const inputCalDb = (inputLevelDbu: number | null): number =>
  INPUT_LEVEL_DBU - (inputLevelDbu ?? DEFAULT_MODEL_INPUT_DBU);
