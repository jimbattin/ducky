// WAV decoding for the IR converter, with the semantics of tools/wav.h dkwav::Read.
// Pure TypeScript (no DOM, no Bun APIs): shared by the editor and tools/ir_convert.ts.

export class WavError extends Error {
  constructor(
    readonly reason: "not-wav" | "format",
    readonly detail: string,
  ) {
    super(`${reason}: ${detail}`);
    this.name = "WavError";
  }
}

export interface WavAudio {
  rate: number;
  channels: number;
  frames: number;
  samples: Float32Array; // channel 0 only
}

const MIN_RATE = 8000;
const MAX_RATE = 384000;

function tag4(b: Uint8Array, at: number): string {
  return String.fromCharCode(b[at]!, b[at + 1]!, b[at + 2]!, b[at + 3]!);
}

// PCM 16/24/32-bit (scaled by 1/2^(bits-1)), IEEE float 32-bit, and WAVE_FORMAT_EXTENSIBLE (0xFFFE) whose subformat
// GUID starts with 1 or 3. Walks chunks: skips unknown ones, honours odd-size padding, tolerates a truncated last chunk.
export function readWav(bytes: Uint8Array): WavAudio {
  if (bytes.length < 12 || tag4(bytes, 0) !== "RIFF" || tag4(bytes, 8) !== "WAVE")
    throw new WavError("not-wav", "not a RIFF/WAVE file");
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let tag = 0;
  let channels = 0;
  let bits = 0;
  let rate = 0;
  let haveFmt = false;
  let dataAt = -1;
  let dataSize = 0;
  for (let pos = 12; pos + 8 <= bytes.length; ) {
    const size = dv.getUint32(pos + 4, true);
    const body = Math.min(size, bytes.length - pos - 8);
    const id = tag4(bytes, pos);
    if (id === "fmt " && body >= 16) {
      tag = dv.getUint16(pos + 8, true);
      channels = dv.getUint16(pos + 10, true);
      rate = dv.getUint32(pos + 12, true);
      bits = dv.getUint16(pos + 22, true);
      if (tag === 0xfffe && body >= 40) tag = dv.getUint16(pos + 32, true); // the subformat GUID's first two bytes
      haveFmt = true;
    } else if (id === "data") {
      dataAt = pos + 8;
      dataSize = body;
    }
    pos += 8 + size + (size & 1);
  }
  if (!haveFmt) throw new WavError("not-wav", "no fmt chunk");
  if (dataAt < 0) throw new WavError("not-wav", "no data chunk");
  const pcm = tag === 1 && (bits === 16 || bits === 24 || bits === 32);
  const ieee = tag === 3 && bits === 32;
  if ((!pcm && !ieee) || channels === 0 || rate < MIN_RATE || rate > MAX_RATE)
    throw new WavError("format", `${tag}/${bits} bit, ${rate} Hz`);

  const width = bits / 8;
  const frame = width * channels;
  const frames = Math.floor(dataSize / frame);
  const samples = new Float32Array(frames);
  const scale = 2 ** -(bits - 1);
  for (let i = 0; i < frames; i++) {
    const p = dataAt + i * frame;
    if (ieee) samples[i] = dv.getFloat32(p, true);
    else if (bits === 16) samples[i] = dv.getInt16(p, true) * scale;
    else if (bits === 32) samples[i] = dv.getInt32(p, true) * scale;
    else samples[i] = (dv.getUint16(p, true) | (dv.getInt8(p + 2) << 16)) * scale;
  }
  return { rate, channels, frames, samples };
}
