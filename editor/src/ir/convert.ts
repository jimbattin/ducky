// .wav → Ducky IR binary, format 1. Spec: docs/FILTERS.md "IR binary (format 1)".
// Pure TypeScript (no DOM, no Bun APIs): shared by the editor and tools/ir_convert.ts.

import { crc32 } from "../nam/crc32";
import { sanitiseName } from "../name";
import { resampledLength, resampleTo48k } from "./resample";
import { readWav, type WavAudio, WavError } from "./wav";

export const IR_MAGIC = "DKIR";
export const IR_FORMAT_VERSION = 1;
export const IR_BIN_SIZE = 8292;
export const IR_TAPS = 2048;
export const IR_FADE = 64;

// Byte offsets in the binary.
const OFF_VERSION = 4;
const OFF_SIZE = 8;
export const OFF_TAP_COUNT = 12;
export const OFF_SOURCE_RATE = 16;
export const OFF_SOURCE_FRAMES = 20;
export const OFF_SOURCE_CHANNELS = 24;
export const OFF_GAIN = 28;
export const OFF_NAME = 32;
export const OFF_TAPS = 96;
const OFF_CRC = OFF_TAPS + 4 * IR_TAPS; // 8288

export type IrRejectReason = "not-wav" | "format" | "empty" | "samples" | "silent";

export class IrRejected extends Error {
  constructor(
    readonly reason: IrRejectReason,
    readonly detail: string,
  ) {
    super(`${reason}: ${detail}`);
    this.name = "IrRejected";
  }
}

export interface IrInfo {
  name: string; // exactly the stored (sanitised) name
  sourceRate: number;
  sourceFrames: number;
  sourceChannels: number;
  taps: number; // stored tap count, min(resampled length, IR_TAPS)
  truncated: boolean; // the resampled IR was longer than IR_TAPS (faded out over the last IR_FADE taps)
  gainDb: number; // the normalisation applied (the reference band to 0 dB)
}

export interface ConvertedIr {
  bin: Uint8Array;
  info: IrInfo;
}

// The level reference: pink noise band-limited to REF_LOW_HZ..REF_HIGH_HZ, where an amp model's output has its energy.
// The IR's power gain is averaged over REF_POINTS log-spaced frequencies (equal weight per octave, i.e. pink), and the
// IR is scaled so that average is 1 (0 dB): a cab IR then leaves an amp's level about where it was, whatever it does
// above 5 kHz. A full-band (unit-energy) normalisation would instead boost the passband to pay for the cut highs.
const REF_LOW_HZ = 80;
const REF_HIGH_HZ = 5000;
const REF_POINTS = 144; // 24 per octave
const SAMPLE_RATE = 48000;

function referenceBandPower(taps: Float64Array): number {
  let sum = 0;
  for (let i = 0; i < REF_POINTS; i++) {
    const f = REF_LOW_HZ * (REF_HIGH_HZ / REF_LOW_HZ) ** (i / (REF_POINTS - 1));
    const w = (2 * Math.PI * f) / SAMPLE_RATE;
    // H(f) = sum h[n] e^(-jwn), the phasor advanced by complex multiplication.
    const cw = Math.cos(w);
    const sw = Math.sin(w);
    let pr = 1;
    let pi = 0;
    let re = 0;
    let im = 0;
    for (const v of taps) {
      re += v * pr;
      im += v * pi;
      const t = pr * cw + pi * sw;
      pi = pi * cw - pr * sw;
      pr = t;
    }
    sum += re * re + im * im;
  }
  return sum / REF_POINTS;
}

export function convertIr(bytes: Uint8Array, fallbackName: string): ConvertedIr {
  let wav: WavAudio;
  try {
    wav = readWav(bytes);
  } catch (e) {
    if (e instanceof WavError) throw new IrRejected(e.reason, e.detail);
    throw e;
  }
  if (wav.frames === 0) throw new IrRejected("empty", "no audio frames");
  const bad = wav.samples.findIndex((v) => !Number.isFinite(v));
  if (bad >= 0) throw new IrRejected("samples", `sample ${bad} is not finite`);

  const length = resampledLength(wav.frames, wav.rate);
  const taps = resampleTo48k(wav.samples, wav.rate, IR_TAPS);
  const truncated = length > IR_TAPS;
  if (truncated)
    for (let i = 0; i < IR_FADE; i++)
      taps[IR_TAPS - IR_FADE + i]! *= 0.5 * (1 + Math.cos((Math.PI * (i + 1)) / IR_FADE));

  let energy = 0;
  for (const v of taps) energy += v * v;
  if (energy === 0 || !Number.isFinite(energy)) throw new IrRejected("silent", `energy is ${energy}`);
  // The full-band average power gain is the energy (Parseval). An IR whose reference band sits 60 dB or more below it
  // would get a dangerous gain: it is not a cab.
  const band = referenceBandPower(taps);
  if (!(band >= 1e-6 * energy))
    throw new IrRejected("silent", `${REF_LOW_HZ}-${REF_HIGH_HZ} Hz is 60 dB or more below the IR's average`);
  const gain = 1 / Math.sqrt(band);

  const info: IrInfo = {
    name: sanitiseName(fallbackName.trim()),
    sourceRate: wav.rate,
    sourceFrames: wav.frames,
    sourceChannels: wav.channels,
    taps: taps.length,
    truncated,
    gainDb: 20 * Math.log10(gain),
  };

  const bin = new Uint8Array(IR_BIN_SIZE);
  const dv = new DataView(bin.buffer);
  for (let i = 0; i < 4; i++) bin[i] = IR_MAGIC.charCodeAt(i);
  dv.setUint16(OFF_VERSION, IR_FORMAT_VERSION, true);
  dv.setUint32(OFF_SIZE, IR_BIN_SIZE, true);
  dv.setUint32(OFF_TAP_COUNT, info.taps, true);
  dv.setUint32(OFF_SOURCE_RATE, info.sourceRate, true);
  dv.setUint32(OFF_SOURCE_FRAMES, info.sourceFrames, true);
  dv.setUint16(OFF_SOURCE_CHANNELS, info.sourceChannels, true);
  dv.setFloat32(OFF_GAIN, gain, true);
  for (let i = 0; i < info.name.length; i++) bin[OFF_NAME + i] = info.name.charCodeAt(i);
  taps.forEach((v, i) => {
    dv.setFloat32(OFF_TAPS + 4 * i, v * gain, true);
  });
  dv.setUint32(OFF_CRC, crc32(bin.subarray(0, OFF_CRC)), true);
  return { bin, info };
}
