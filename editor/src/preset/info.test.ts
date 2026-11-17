import { expect, test } from "bun:test";
import { convertIr } from "../ir/convert";
import { convertNam } from "../nam/convert";
import { buildSlot } from "./bank";
import { GATE_DEFAULT } from "./gate";
import { infoRanges, type SlotInfo, slotInfo } from "./info";

const FIXTURES = `${import.meta.dir}/../../../tests/fixtures`;
const root = JSON.parse(await Bun.file(`${FIXTURES}/nam/A2.nam`).text());
root.metadata.gear_type = "amp";
root.metadata.gear_make = "Fender";
const model = convertNam(JSON.stringify(root), "A2");
const ir = convertIr(new Uint8Array(await Bun.file(`${FIXTURES}/ir/decay.wav`).arrayBuffer()), "decay");

function expectMatches(info: SlotInfo, withIr: boolean): void {
  const m = model.info;
  expect(info.name).toBe("Info test");
  expect(info.model.name).toBe(m.name);
  expect(info.model.gearType).toBe("amp");
  expect(info.model.gearMake).toBe("Fender");
  expect(info.model.gearModel).toBeNull();
  expect(info.model.modeledBy).toBeNull();
  expect(info.model.loudness).toBeCloseTo(m.loudness!, 4);
  expect(info.model.inputLevelDbu).toBeCloseTo(m.inputLevelDbu!, 4);
  expect(info.gate).toEqual(GATE_DEFAULT);
  if (!withIr) {
    expect(info.ir).toBeNull();
    return;
  }
  const i = info.ir!;
  expect(i.name).toBe(ir.info.name);
  expect(i.taps).toBe(ir.info.taps);
  expect(i.sourceRate).toBe(ir.info.sourceRate);
  expect(i.sourceFrames).toBe(ir.info.sourceFrames);
  expect(i.sourceChannels).toBe(ir.info.sourceChannels);
  expect(i.gainDb).toBeCloseTo(ir.info.gainDb, 4);
}

// A zero-filled image holding only the infoRanges bytes of full.
function onlyRanges(full: Uint8Array, hasIr: boolean): Uint8Array {
  const sparse = new Uint8Array(full.length);
  for (const [off, len] of infoRanges(hasIr)) sparse.set(full.subarray(off, off + len), off);
  return sparse;
}

for (const withIr of [true, false]) {
  test(`a ${withIr ? "model + IR" : "model-only"} slot decodes to its sources' metadata from infoRanges alone`, () => {
    const full = buildSlot(model.bin, withIr ? ir.bin : null, "Info test");
    const info = slotInfo(full);
    expectMatches(info, withIr);
    expect(slotInfo(onlyRanges(full, withIr))).toEqual(info);
  });
}
