import { expect, test } from "bun:test";
import { crc32 } from "../nam/crc32";
import { BANK_BYTES, buildBank, buildSlot, PresetError, SLOT_BYTES, SLOT_COUNT, splitSlot } from "./bank";

const FIXTURES = `${import.meta.dir}/../../../tests/fixtures`;
const read = async (rel: string) => new Uint8Array(await Bun.file(`${FIXTURES}/${rel}`).arrayBuffer());
const model = await read("nam/A2.bin");
const ir = await read("ir/decay.bin");

test("the A2 + decay slot matches the committed fixture byte for byte", async () => {
  expect(buildSlot(model, ir, "A2 + decay")).toEqual(await read("preset/A2_decay.bin"));
});

test("splitSlot returns the parts a stored preset was built from", async () => {
  expect(splitSlot(await read("preset/A2_decay.bin"))).toEqual({ name: "A2 + decay", model, ir });
  expect(splitSlot(buildSlot(model, null, "A2"))).toEqual({ name: "A2", model, ir: null });
});

test("splitSlot rejects a corrupted image read back", async () => {
  const image = (await read("preset/A2_decay.bin")).slice();
  image[5000]! ^= 0xff;
  expect(() => splitSlot(image)).toThrow("preset: crc");
});

test("a model-only slot is 7988 bytes with flags 0 and a valid CRC", () => {
  const slot = buildSlot(model, null, "A2");
  expect(slot.length).toBe(7988);
  const dv = new DataView(slot.buffer);
  expect(dv.getUint16(6, true)).toBe(0);
  expect(dv.getUint32(8, true)).toBe(7988);
  expect(dv.getUint32(7984, true)).toBe(crc32(slot.subarray(0, 7984)));
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
