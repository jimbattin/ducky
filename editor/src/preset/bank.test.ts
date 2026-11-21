import { expect, test } from "bun:test";
import { crc32 } from "../crc32";
import { readFixture, waypointBytes, withMorphBlock } from "../testFixtures";
import { BANK_BYTES, buildBank, buildSlot, imageSize, PresetError, SLOT_BYTES, SLOT_COUNT, splitSlot } from "./bank";
import { encodeGate, GATE_DEFAULT, type GateSettings } from "./gate";

const model = await readFixture("nam/A2.bin");
const ir = await readFixture("ir/decay.bin");

test("the A2 + decay slot matches the committed fixture byte for byte", async () => {
  expect(buildSlot(model, ir, "A2 + decay")).toEqual(await readFixture("preset/A2_decay.bin"));
});

test("splitSlot returns the parts a stored preset was built from", async () => {
  expect(splitSlot(await readFixture("preset/A2_decay.bin"))).toEqual({
    name: "A2 + decay",
    model,
    ir,
    gate: GATE_DEFAULT,
    morph: null,
  });
  expect(splitSlot(buildSlot(model, null, "A2"))).toEqual({
    name: "A2",
    model,
    ir: null,
    gate: GATE_DEFAULT,
    morph: null,
  });
});

const morphBlock = await readFixture("morph/A2_shuffled.morph");

for (const k of [0, 3])
  for (const withIr of [true, false])
    test(`a ${withIr ? "model + IR" : "model-only"} slot with a morph section of ${k} waypoints is the layout's image and splits back`, () => {
      const section = new Uint8Array([...morphBlock, ...waypointBytes(k)]);
      const plain = buildSlot(model, withIr ? ir : null, "Morphed");
      const built = buildSlot(model, withIr ? ir : null, "Morphed", GATE_DEFAULT, section);
      // Flags bit 1 and the count in bits 2-3, the section between the model/IR and the CRC.
      const dv = new DataView(built.buffer);
      expect(dv.getUint16(6, true)).toBe((withIr ? 1 : 0) | 2 | (k << 2));
      expect(built.length).toBe(plain.length + 64 + k * 7480);
      expect(built.length).toBe(imageSize(withIr, true, k));
      expect(dv.getUint32(8, true)).toBe(built.length);
      expect(built.subarray(plain.length - 4, built.length - 4)).toEqual(section);
      expect(dv.getUint32(built.length - 4, true)).toBe(crc32(built.subarray(0, built.length - 4)));
      expect(built).toEqual(withMorphBlock(plain, section));
      expect(splitSlot(built)).toEqual({ ...splitSlot(plain), morph: section });
    });

test("a morph section that is not a block and 0-3 waypoints is rejected", () => {
  for (const n of [0, 63, 65, 64 + 7479, 64 + 4 * 7480])
    expect(() => buildSlot(model, null, "x", GATE_DEFAULT, new Uint8Array(n))).toThrow(`morph: size ${n}`);
});

// image with other flags and its CRC recomputed.
function reflagged(image: Uint8Array, flags: number): Uint8Array {
  const out = image.slice();
  const dv = new DataView(out.buffer);
  dv.setUint16(6, flags, true);
  dv.setUint32(out.length - 4, crc32(out.subarray(0, out.length - 4)), true);
  return out;
}

test("splitSlot rejects a waypoint count without a morph, unknown flags and a count the size does not hold", () => {
  const plain = buildSlot(model, null, "x");
  expect(() => splitSlot(reflagged(plain, 0x0004))).toThrow("preset: flags 0x4");
  expect(() => splitSlot(reflagged(plain, 0x0010))).toThrow("preset: flags 0x10");
  const morphed = buildSlot(model, null, "x", GATE_DEFAULT, morphBlock);
  expect(() => splitSlot(reflagged(morphed, 0x0006))).toThrow("preset: size");
});

test("a gate is stored as its 8-byte block and read back", () => {
  const gate: GateSettings = { enabled: true, thresholdDb: -55, attackMs: 1.5, holdMs: 30, releaseMs: 250, ratio: 6 };
  expect([...encodeGate(gate)]).toEqual([0x01, 0x37, 0x0f, 0x1e, 0xfa, 0x00, 0x3c, 0x00]);
  expect(splitSlot(buildSlot(model, null, "x", gate)).gate).toEqual(gate);
});

test("splitSlot rejects a corrupted image read back", async () => {
  const image = (await readFixture("preset/A2_decay.bin")).slice();
  image[5000]! ^= 0xff;
  expect(() => splitSlot(image)).toThrow("preset: crc");
});

test("a model-only slot is 7996 bytes with flags 0 and a valid CRC", () => {
  const slot = buildSlot(model, null, "A2");
  expect(slot.length).toBe(7996);
  const dv = new DataView(slot.buffer);
  expect(dv.getUint16(6, true)).toBe(0);
  expect(dv.getUint32(8, true)).toBe(7996);
  expect(dv.getUint32(7992, true)).toBe(crc32(slot.subarray(0, 7992)));
});

test("a model with a bad CRC is rejected", () => {
  const bad = model.slice();
  bad[1000]! ^= 0xff;
  expect(() => buildSlot(bad, null, "x")).toThrow(PresetError);
  expect(() => buildSlot(bad, null, "x")).toThrow("model: crc");
});

test("an IR of the wrong size is rejected", () => {
  expect(() => buildSlot(model, ir.subarray(0, 100), "x")).toThrow("ir: size");
});

test("a bank with only slots[2] holds it at 0x20000 and 0xFF everywhere else", () => {
  const slot = buildSlot(model, ir, "three");
  const slots: (Uint8Array | null)[] = Array(SLOT_COUNT).fill(null);
  slots[2] = slot;
  const bank = buildBank(slots);
  expect(bank.length).toBe(BANK_BYTES);
  const at = 2 * SLOT_BYTES;
  expect(at).toBe(0x20000);
  expect(bank.subarray(at, at + slot.length)).toEqual(slot);
  const rest = [...bank.subarray(0, at), ...bank.subarray(at + slot.length)];
  expect(rest.every((b) => b === 0xff)).toBe(true);
});

test("a bank of 7 slots is rejected", () => {
  expect(() => buildBank(Array(7).fill(null))).toThrow(PresetError);
});
