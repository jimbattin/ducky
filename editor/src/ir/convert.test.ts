import { expect, test } from "bun:test";
import { convertIr, IrRejected, type IrRejectReason } from "./convert";

const FIXTURES = `${import.meta.dir}/../../../tests/fixtures/ir`;

interface WavSpec {
  rate: number;
  tag?: number; // 1 PCM, 3 IEEE float
  bits?: number;
  channels: Float32Array[]; // equal lengths
}

// Canonical 44-byte header; PCM samples are full-scale-clamped integers.
function makeWav({ rate, tag = 3, bits = 32, channels }: WavSpec): Uint8Array {
  const width = bits / 8;
  const frames = channels[0]!.length;
  const dataSize = frames * channels.length * width;
  const bytes = new Uint8Array(44 + dataSize);
  const dv = new DataView(bytes.buffer);
  bytes.set(new TextEncoder().encode("RIFF"), 0);
  dv.setUint32(4, 36 + dataSize, true);
  bytes.set(new TextEncoder().encode("WAVEfmt "), 8);
  dv.setUint32(16, 16, true);
  dv.setUint16(20, tag, true);
  dv.setUint16(22, channels.length, true);
  dv.setUint32(24, rate, true);
  dv.setUint32(28, rate * channels.length * width, true);
  dv.setUint16(32, channels.length * width, true);
  dv.setUint16(34, bits, true);
  bytes.set(new TextEncoder().encode("data"), 36);
  dv.setUint32(40, dataSize, true);
  for (let i = 0; i < frames; i++)
    channels.forEach((ch, c) => {
      const p = 44 + (i * channels.length + c) * width;
      if (tag === 3) dv.setFloat32(p, ch[i]!, true);
      else if (bits === 16) dv.setInt16(p, Math.round(ch[i]! * 32767), true);
      else bytes[p] = Math.round(ch[i]! * 127) + 128;
    });
  return bytes;
}

function taps(bin: Uint8Array): Float32Array {
  return new Float32Array(bin.slice(96, 96 + 4 * 2048).buffer);
}

function xorshiftNoise(n: number): Float32Array {
  const out = new Float32Array(n);
  let s = 0x12345678;
  for (let i = 0; i < n; i++) {
    s ^= s << 13;
    s >>>= 0;
    s ^= s >>> 17;
    s ^= s << 5;
    s >>>= 0;
    out[i] = (s / 4294967296) * 2 - 1;
  }
  return out;
}

function rejection(bytes: Uint8Array): IrRejectReason {
  try {
    convertIr(bytes, "x");
  } catch (e) {
    expect(e).toBeInstanceOf(IrRejected);
    return (e as IrRejected).reason;
  }
  throw new Error("IR was accepted");
}

test("decay.wav converts byte for byte to the committed decay.bin", async () => {
  const wav = new Uint8Array(await Bun.file(`${FIXTURES}/decay.wav`).arrayBuffer());
  const want = new Uint8Array(await Bun.file(`${FIXTURES}/decay.bin`).arrayBuffer());
  const { bin, info } = convertIr(wav, "decay");
  expect(info).toMatchObject({ sourceRate: 44100, sourceFrames: 4410, taps: 2048, truncated: true });
  expect(bin).toEqual(want);
});

test("48 kHz impulse is copied and normalised to unit energy", () => {
  const x = new Float32Array(100);
  x[0] = 0.5;
  const { bin, info } = convertIr(makeWav({ rate: 48000, channels: [x] }), "imp");
  const dv = new DataView(bin.buffer);
  expect(info.taps).toBe(100);
  expect(info.truncated).toBe(false);
  expect(Math.abs(info.gainDb - 6.0206)).toBeLessThan(1e-4);
  expect(Math.abs(info.gainDb - 20 * Math.log10(2))).toBeLessThan(1e-6);
  expect(dv.getUint32(12, true)).toBe(100);
  expect(dv.getFloat32(28, true)).toBe(2);
  const t = taps(bin);
  expect(t[0]).toBe(1);
  for (let i = 1; i < 2048; i++) expect(t[i]).toBe(0);
});

test("96 kHz and 44.1 kHz resample to the 48 kHz rendering", () => {
  const render = (rate: number) => {
    const n = Math.round(0.05 * rate);
    const x = new Float32Array(n);
    for (let i = 0; i < n; i++) {
      const t = i / rate;
      x[i] = Math.exp(-t / 0.005) * Math.sin(2 * Math.PI * 1000 * t);
    }
    return taps(convertIr(makeWav({ rate, channels: [x] }), "r").bin);
  };
  const ref = render(48000);
  const peak = ref.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
  for (const rate of [96000, 44100]) {
    const got = render(rate);
    let err = 0;
    for (let i = 0; i < 2048; i++) err = Math.max(err, Math.abs(got[i]! - ref[i]!));
    expect(err).toBeLessThanOrEqual(0.02 * peak);
  }
});

test("long IRs are truncated with a fade to an exact zero, still unit energy", () => {
  const { bin, info } = convertIr(makeWav({ rate: 48000, channels: [xorshiftNoise(3000)] }), "n");
  expect(info.taps).toBe(2048);
  expect(info.truncated).toBe(true);
  const t = taps(bin);
  expect(t[2047]).toBe(0);
  const e = t.reduce((s, v) => s + v * v, 0);
  expect(Math.abs(e - 1)).toBeLessThan(1e-6);
});

test("stereo uses channel 0", () => {
  const l = new Float32Array(32);
  const r = new Float32Array(32);
  l[0] = 0.5;
  r[10] = 0.5;
  const { bin, info } = convertIr(makeWav({ rate: 48000, channels: [l, r] }), "st");
  const t = taps(bin);
  expect(t[0]).not.toBe(0);
  expect(t[10]).toBe(0);
  expect(info.sourceChannels).toBe(2);
});

test("rejections", () => {
  const random = new Uint8Array(256).map((_, i) => (i * 73 + 11) & 0xff);
  const one = new Float32Array([0.5, 0.25]);
  const nan = new Float32Array([0.5, Number.NaN]);
  expect(rejection(random)).toBe("not-wav");
  expect(rejection(makeWav({ rate: 48000, tag: 1, bits: 8, channels: [one] }))).toBe("format");
  expect(rejection(makeWav({ rate: 48000, tag: 2, bits: 16, channels: [one] }))).toBe("format");
  expect(rejection(makeWav({ rate: 48000, channels: [new Float32Array(0)] }))).toBe("empty");
  expect(rejection(makeWav({ rate: 48000, channels: [nan] }))).toBe("samples");
  expect(rejection(makeWav({ rate: 44100, channels: [new Float32Array(64)] }))).toBe("silent");
});

test("names are stored as printable ASCII", () => {
  const x = new Float32Array([1]);
  const { bin, info } = convertIr(makeWav({ rate: 48000, tag: 1, bits: 16, channels: [x] }), " Mix ü ");
  expect(info.name).toBe("Mix ?");
  const field = bin.subarray(32, 96);
  expect(new TextDecoder().decode(field.subarray(0, field.indexOf(0)))).toBe("Mix ?");
});
