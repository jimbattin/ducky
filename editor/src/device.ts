// What the converters, the preset code, the link and the editor share about the pedal: its sample rate and the rules
// for the text its binaries store and its LOG lines print.

/** daisy/src/dsp/blocks.h kSampleRate: the codec's rate and the rate NAM A2 models are trained at. */
export const SAMPLE_RATE = 48000;

export const NAME_MAX = 63;

// Printable ASCII only (the pedal's LOG lines), at most NAME_MAX characters.
export function sanitiseName(s: string): string {
  let out = "";
  for (const ch of s) {
    const cp = ch.codePointAt(0)!;
    out += cp >= 0x20 && cp <= 0x7e ? ch : "?";
  }
  return out.slice(0, NAME_MAX);
}

/** s's character codes at bytes[at]; s must be ASCII already (sanitiseName). */
export function writeAscii(bytes: Uint8Array, at: number, s: string): void {
  for (let i = 0; i < s.length; i++) bytes[at + i] = s.charCodeAt(i);
}
