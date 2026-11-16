// Preset-facing names (models, IRs): what the binaries store and the pedal's LOG lines print.

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
