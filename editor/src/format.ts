// The editor's text for numbers and errors: a typographic minus (U+2212) for negatives throughout.
import { SAMPLE_RATE } from "./device";

/** x.toFixed(digits) with "−" for negatives. */
export const fixed = (x: number, digits: number): string => `${x < 0 ? "−" : ""}${Math.abs(x).toFixed(digits)}`;

/** "+1.0 dB", "−7.0 dB"; unit "dBu" for levels. */
export const signedDb = (x: number, unit = "dB"): string => `${x >= 0 ? "+" : "−"}${Math.abs(x).toFixed(1)} ${unit}`;

/** A value's text from the pedal's table (ASCII) as the editor shows it: "-7 dB" becomes "−7 dB". */
export const typographic = (text: string): string => text.replace(/^-(?=\d)/, "−");

export const fmtHz = (f: number): string =>
  f < 1000 ? `${Math.round(f)} Hz` : `${(f / 1000).toFixed(f < 10000 ? 2 : 1)} kHz`;

/** An IR's length in ms. */
export const tapsMs = (taps: number): string => ((1000 * taps) / SAMPLE_RATE).toFixed(1);

export const errorText = (e: unknown): string => (e instanceof Error ? e.message : String(e));
