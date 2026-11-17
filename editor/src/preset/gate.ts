// A preset's noise gate (daisy/src/dsp/noise_gate.h, the gate block in daisy/src/preset.h). Pure TypeScript: shared by
// the slot builder, the slot reader, the link protocol, the drafts and tools/pedal.ts.
//
// The 8-byte block: u8 flags (bit 0 enabled), u8 threshold (dB below full scale), u8 attack (0.1 ms), u8 hold (ms),
// u16 release (ms, little-endian), u8 ratio (0.1), u8 0.

/** A gate in user units. */
export interface GateSettings {
  enabled: boolean;
  thresholdDb: number; // -80..-30 dBFS: closes below it, opens GATE_HYSTERESIS_DB above
  attackMs: number; // 0.5..10
  holdMs: number; // 10..100
  releaseMs: number; // 10..1000: the time to fall 60 dB
  ratio: number; // 1.5..10 (expander ratio)
}

export const GATE_DEFAULT: GateSettings = {
  enabled: false,
  thresholdDb: -60,
  attackMs: 0.5,
  holdMs: 20,
  releaseMs: 100,
  ratio: 4,
};
export const GATE_SIZE = 8;
export const GATE_HYSTERESIS_DB = 6;

type Field = Exclude<keyof GateSettings, "enabled">;

// Each field's stored value from its user value, and its stored range.
const STORED: Record<Field, { encode: (v: number) => number; decode: (b: number) => number; lo: number; hi: number }> =
  {
    thresholdDb: { encode: (v) => -v, decode: (b) => -b, lo: 30, hi: 80 },
    attackMs: { encode: (v) => Math.round(v * 10), decode: (b) => b / 10, lo: 5, hi: 100 },
    holdMs: { encode: (v) => v, decode: (b) => b, lo: 10, hi: 100 },
    releaseMs: { encode: (v) => v, decode: (b) => b, lo: 10, hi: 1000 },
    ratio: { encode: (v) => Math.round(v * 10), decode: (b) => b / 10, lo: 15, hi: 100 },
  };

const FIELD_TEXT: Record<Field, (v: number) => string> = {
  thresholdDb: (v) => `threshold ${v} dBFS: must be -80..-30`,
  attackMs: (v) => `attack ${v} ms: must be 0.5..10`,
  holdMs: (v) => `hold ${v} ms: must be 10..100`,
  releaseMs: (v) => `release ${v} ms: must be 10..1000`,
  ratio: (v) => `ratio ${v}: must be 1.5..10`,
};

const FIELDS = Object.keys(STORED) as Field[];

/** The first field whose stored value is not an integer inside its range, as text; null when every field is. */
export function gateProblem(g: GateSettings): string | null {
  for (const f of FIELDS) {
    const { encode, lo, hi } = STORED[f];
    const b = encode(g[f]);
    if (!Number.isInteger(b) || b < lo || b > hi) return FIELD_TEXT[f](g[f]);
  }
  return null;
}

/** The 8-byte gate block. The fields must pass gateProblem. */
export function encodeGate(g: GateSettings): Uint8Array {
  const out = new Uint8Array(GATE_SIZE);
  const release = STORED.releaseMs.encode(g.releaseMs);
  out[0] = g.enabled ? 1 : 0;
  out[1] = STORED.thresholdDb.encode(g.thresholdDb);
  out[2] = STORED.attackMs.encode(g.attackMs);
  out[3] = STORED.holdMs.encode(g.holdMs);
  out[4] = release & 0xff;
  out[5] = release >> 8;
  out[6] = STORED.ratio.encode(g.ratio);
  return out;
}

/** A gate block at bytes[at], each field clamped into its range as the firmware's ClampGate does. */
export function decodeGate(bytes: Uint8Array, at = 0): GateSettings {
  const field = (f: Field, b: number) => STORED[f].decode(Math.min(STORED[f].hi, Math.max(STORED[f].lo, b)));
  return {
    enabled: (bytes[at]! & 1) !== 0,
    thresholdDb: field("thresholdDb", bytes[at + 1]!),
    attackMs: field("attackMs", bytes[at + 2]!),
    holdMs: field("holdMs", bytes[at + 3]!),
    releaseMs: field("releaseMs", bytes[at + 4]! | (bytes[at + 5]! << 8)),
    ratio: field("ratio", bytes[at + 6]!),
  };
}

/** Two gates that store the same block. */
export function gateEqual(a: GateSettings, b: GateSettings): boolean {
  const ea = encodeGate(a);
  const eb = encodeGate(b);
  return ea.every((v, i) => v === eb[i]);
}
