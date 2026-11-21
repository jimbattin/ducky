// Writes tests/fixtures/ir/decay.wav: 24-bit mono PCM at 44.1 kHz, 4410 frames of exponentially decaying xorshift
// noise. Then `bun tools/ir_convert.ts tests/fixtures/ir/decay.wav` makes the committed decay.bin.
//
// Usage: bun tools/make_ir_fixture.ts

const RATE = 44100;
const FRAMES = 4410;
const out = `${import.meta.dir}/../tests/fixtures/ir/decay.wav`;

const bytes = new Uint8Array(44 + 3 * FRAMES);
const dv = new DataView(bytes.buffer);
const ascii = (at: number, s: string) => {
  for (let i = 0; i < s.length; i++) bytes[at + i] = s.charCodeAt(i);
};
ascii(0, "RIFF");
dv.setUint32(4, bytes.length - 8, true);
ascii(8, "WAVEfmt ");
dv.setUint32(16, 16, true);
dv.setUint16(20, 1, true); // PCM
dv.setUint16(22, 1, true); // mono
dv.setUint32(24, RATE, true);
dv.setUint32(28, RATE * 3, true); // byte rate
dv.setUint16(32, 3, true); // block align
dv.setUint16(34, 24, true);
ascii(36, "data");
dv.setUint32(40, 3 * FRAMES, true);

let s = 0x12345678;
for (let n = 0; n < FRAMES; n++) {
  s ^= s << 13;
  s >>>= 0;
  s ^= s >>> 17;
  s ^= s << 5;
  s >>>= 0;
  const u = (s >>> 8) / 16777216;
  const v = 0.9 * (2 * u - 1) * Math.exp(-n / 441);
  const q = Math.round(v * 8388607);
  bytes[44 + 3 * n] = q & 0xff;
  bytes[45 + 3 * n] = (q >> 8) & 0xff;
  bytes[46 + 3 * n] = (q >> 16) & 0xff;
}
await Bun.write(out, bytes);
console.log(`${out}: ${FRAMES} frames, ${RATE} Hz, 24-bit mono`);
