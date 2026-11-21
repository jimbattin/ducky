import { expect, test } from "bun:test";
import { convertIr } from "../ir/convert";
import { convertNam } from "../nam/convert";
import { readFixture, readFixtureText, waypointBytes, withMorphBlock } from "../testFixtures";
import { buildSlot, imageSize } from "./bank";
import { GATE_DEFAULT } from "./gate";
import { infoRanges, type SlotInfo, slotInfo } from "./info";

const root = JSON.parse(await readFixtureText("nam/A2.nam"));
root.metadata.gear_type = "amp";
root.metadata.gear_make = "Fender";
const model = convertNam(JSON.stringify(root), "A2");
const ir = convertIr(await readFixture("ir/decay.wav"), "decay");

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
function onlyRanges(full: Uint8Array, hasIr: boolean, hasMorph: boolean): Uint8Array {
  const sparse = new Uint8Array(full.length);
  for (const [off, len] of infoRanges(hasIr, hasMorph)) sparse.set(full.subarray(off, off + len), off);
  return sparse;
}

const morphBlock = await readFixture("morph/A2_shuffled.morph");

for (const withIr of [true, false])
  for (const withMorph of [true, false]) {
    const what = `${withIr ? "model + IR" : "model-only"} slot ${withMorph ? "with" : "without"} a morph block`;
    test(`a ${what} decodes to its sources' metadata from infoRanges alone`, () => {
      const plain = buildSlot(model.bin, withIr ? ir.bin : null, "Info test");
      const full = withMorph ? withMorphBlock(plain, morphBlock) : plain;
      const info = slotInfo(full);
      expectMatches(info, withIr);
      expect(info.modelCrc).toBe(new DataView(model.bin.buffer).getUint32(model.bin.length - 4, true));
      if (withMorph) {
        expect(info.morph).toMatchObject({ target: 0, perm: [1, 2, 0], modelCrc: 0xcb02c66f, waypoints: 0 });
        expect(info.morph!.fit).toBeGreaterThan(0.999);
      } else expect(info.morph).toBeNull();
      expect(slotInfo(onlyRanges(full, withIr, withMorph))).toEqual(info);
    });
  }

for (const withIr of [true, false])
  test(`a ${withIr ? "model + IR" : "model-only"} slot with a trained path gives its block and waypoint count without its waypoints`, () => {
    const plain = buildSlot(model.bin, withIr ? ir.bin : null, "Info test");
    const full = withMorphBlock(plain, new Uint8Array([...morphBlock, ...waypointBytes(3)]));
    const info = slotInfo(full);
    expectMatches(info, withIr);
    expect(info.morph).toEqual({ ...slotInfo(withMorphBlock(plain, morphBlock)).morph!, waypoints: 3 });
    // The ranges stop where the waypoints start, so what readInfo allocates (an image without them) holds them all.
    const cut = imageSize(withIr, true);
    expect(cut).toBe(full.length - 3 * 7480);
    for (const [off, len] of infoRanges(withIr, true)) expect(off + len).toBeLessThanOrEqual(cut - 4);
    expect(slotInfo(onlyRanges(full, withIr, true).subarray(0, cut))).toEqual(info);
  });
