import { expect, test } from "bun:test";
import type { PedalSession } from "../link/session";
import { buildSlot, splitSlot } from "../preset/bank";
import type { GateSettings } from "../preset/gate";
import { slotInfo } from "../preset/info";
import { decodePathFile } from "../preset/morph";
import { pathFile, readFixture, waypointBytes, withMorphBlock } from "../testFixtures";
import { IDENTITY } from "./align";
import { saveMorphPath } from "./save";

const model = await readFixture("nam/A2.bin");
const other = await readFixture("morph/A2_shuffled.bin");
const ir = await readFixture("ir/decay.bin");
const crc = (bin: Uint8Array): number => new DataView(bin.buffer, bin.byteOffset).getUint32(bin.length - 4, true);
const gate: GateSettings = { enabled: true, thresholdDb: -55, attackMs: 0.5, holdMs: 20, releaseMs: 100, ratio: 4 };

/** A session over slot images (null: empty): readImage and read serve them, upload replaces one. */
function fakeSession(images: (Uint8Array | null)[]) {
  const session = {
    readImage: async (k: number) => images[k]!,
    read: async (k: number, off: number, len: number) => images[k]!.slice(off, off + len),
    upload: async (k: number, bytes: Uint8Array, onProgress?: (sent: number, size: number) => void) => {
      images[k] = bytes;
      onProgress?.(bytes.length, bytes.length);
      return { flashMs: 77, retries: 0 };
    },
  } as unknown as PedalSession;
  return { session, images };
}

const waypoints = waypointBytes(2);
const path = decodePathFile(pathFile(crc(model), crc(other), 0.875, 0.25, waypoints));
// Slot 1: the path's model A with an IR, a gate and a stored straight morph; slot 4: its model B.
const own = withMorphBlock(buildSlot(model, ir, "Mine", gate), await readFixture("morph/A2_shuffled.morph"));
const target = buildSlot(other, null, "Target");

test("a trained path is uploaded as the slot's preset with the path's block and waypoints", async () => {
  const { session, images } = fakeSession([null, own, null, null, target]);
  const progress: number[] = [];
  const r = await saveMorphPath(session, 1, 4, path, (f) => progress.push(f));
  expect(r).toEqual({ flashMs: 77, target: 4, fit: 0.875, unalignedFit: 0.25, waypoints: 2 });
  expect(progress).toEqual([1]);
  const saved = splitSlot(images[1]!);
  expect({ ...saved, morph: null }).toEqual({ name: "Mine", model, ir, gate, morph: null });
  expect(saved.morph!.subarray(64)).toEqual(waypoints);
  expect(slotInfo(images[1]!).morph).toEqual({
    ...IDENTITY,
    target: 4,
    modelCrc: crc(other),
    fit: 0.875,
    unalignedFit: 0.25,
    waypoints: 2,
  });
  expect(images[4]).toBe(target);
});

test("a path trained for other models is refused, naming the slot, and nothing is uploaded", async () => {
  // The slots the other way round: slot 1 holds B, slot 4 holds A.
  const swapped = fakeSession([null, target, null, null, own]);
  await expect(saveMorphPath(swapped.session, 1, 4, path)).rejects.toThrow("Slot 2: its model is not the one");
  // Slot 1 is right; slot 4 holds A again instead of B.
  const same = fakeSession([null, own, null, null, own]);
  await expect(saveMorphPath(same.session, 1, 4, path)).rejects.toThrow("Slot 5: its model is not the one");
  await expect(saveMorphPath(same.session, 1, 1, path)).rejects.toThrow("cannot morph with itself");
  expect(swapped.images[1]).toBe(target);
  expect(same.images[1]).toBe(own);
});
