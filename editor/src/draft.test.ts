import { expect, test } from "bun:test";
import { baseDraft, composeSlot, type Draft, isEdited, morphDependents, saveDraft, withModel } from "./draft";
import type { SlotMessage } from "./link/protocol";
import type { PedalSession } from "./link/session";
import type { ConvertedModel, ModelInfo } from "./nam/convert";
import { buildSlot, splitSlot } from "./preset/bank";
import type { GateSettings } from "./preset/gate";
import { type SlotInfo, slotInfo } from "./preset/info";
import { decodeMorphBlock, withTarget } from "./preset/morph";
import { readFixture, waypointBytes, withMorphBlock } from "./testFixtures";

const model = await readFixture("nam/A2.bin");
const ir = await readFixture("ir/decay.bin");
const stored = splitSlot(await readFixture("preset/A2_decay.bin"));

const slot: SlotMessage = {
  type: "slot",
  slot: 2,
  status: 0,
  hasIr: true,
  playing: false,
  name: "A2 + decay",
  model: "A2",
  ir: "decay",
};
const newModel = (name: string): ConvertedModel => ({ bin: model, info: { name } as ModelInfo });

test("removing the IR saves the stored model alone under the same name", () => {
  const d: Draft = { ...baseDraft(slot), ir: { kind: "none" } };
  expect(isEdited(d, slot)).toBe(true);
  expect(composeSlot(d, stored, stored.gate, null)).toEqual(buildSlot(model, null, "A2 + decay"));
});

test("replacing the model keeps the stored IR", () => {
  const d = withModel(baseDraft(slot), slot, newModel("Other"));
  expect(composeSlot(d, stored, stored.gate, null)).toEqual(buildSlot(model, ir, "A2 + decay"));
});

test("a new model renames the preset only while the name is still the old model's", () => {
  const named = { ...slot, name: "A2" };
  expect(withModel(baseDraft(named), named, newModel("Other")).name).toBe("Other");
  expect(withModel(baseDraft(slot), slot, newModel("Other")).name).toBe("A2 + decay");
});

test("a draft back at the stored state is not edited", () => {
  expect(isEdited(baseDraft(slot), slot)).toBe(false);
  expect(isEdited({ ...baseDraft(slot), name: "A2 + decay" }, slot)).toBe(false);
});

const gate: GateSettings = { enabled: true, thresholdDb: -55, attackMs: 0.5, holdMs: 20, releaseMs: 100, ratio: 4 };

test("a draft with only a gate is edited", () => {
  expect(isEdited({ ...baseDraft(slot), gate }, slot)).toBe(true);
});

/** A session that records its calls; readImage returns image. */
function fakeSession(image: Uint8Array = new Uint8Array()) {
  const calls: string[] = [];
  const uploads: Uint8Array[] = [];
  const session = {
    gate: async (k: number, g: GateSettings) => {
      calls.push(`gate ${k} ${g.thresholdDb}`);
      return 0;
    },
    upload: async (_k: number, bytes: Uint8Array) => {
      calls.push("upload");
      uploads.push(bytes);
      return { flashMs: 0, retries: 0 };
    },
    read: async () => {
      calls.push("read");
      return new Uint8Array();
    },
    readImage: async () => {
      calls.push("readImage");
      return image;
    },
  } as unknown as PedalSession;
  return { session, calls, uploads };
}

test("saving a gate-only draft sends GATE and never uploads", async () => {
  const { session, calls } = fakeSession();
  expect(await saveDraft(session, slot, { ...baseDraft(slot), gate }, 1, () => undefined)).toEqual({
    result: "saved",
    morph: null,
  });
  expect(calls).toEqual(["gate 2 -55"]);
});

test("a morph-only draft is edited, writes nothing itself and hands its target on", async () => {
  const d: Draft = { ...baseDraft(slot), morph: 1 };
  expect(isEdited(d, slot)).toBe(true);
  const { session, calls } = fakeSession();
  expect(await saveDraft(session, slot, d, null, () => undefined)).toEqual({ result: "saved", morph: 1 });
  expect(await saveDraft(session, slot, { ...d, morph: "none" }, 1, () => undefined)).toEqual({
    result: "saved",
    morph: "none",
  });
  expect(calls).toEqual([]);
});

const morphBlock = await readFixture("morph/A2_shuffled.morph");
const morphed = withMorphBlock(await readFixture("preset/A2_decay.bin"), morphBlock);
// The same preset with a trained path of three waypoints; its block targets slot 0.
const pathSection = new Uint8Array([...morphBlock, ...waypointBytes(3)]);
const trained = withMorphBlock(await readFixture("preset/A2_decay.bin"), pathSection);
const renamed: Draft = { ...baseDraft(slot), name: "Renamed" };

test("an upload that keeps the model and leaves the morph alone carries the stored morph section over", async () => {
  for (const [image, section] of [
    [morphed, morphBlock],
    [trained, pathSection],
  ] as const) {
    const { session, calls, uploads } = fakeSession(image);
    expect(await saveDraft(session, slot, renamed, 0, () => undefined)).toEqual({ result: "saved", morph: null });
    expect(calls).toEqual(["readImage", "upload"]);
    expect(uploads[0]).toEqual(buildSlot(model, ir, "Renamed", stored.gate, section));
    // An IR change keeps it too.
    await saveDraft(session, slot, { ...baseDraft(slot), ir: { kind: "none" } }, 0, () => undefined);
    expect(uploads[1]).toEqual(buildSlot(model, null, "A2 + decay", stored.gate, section));
  }
});

test("a carried morph section is aimed at the slot its block resolves to", async () => {
  // The target's preset moved to slot 6 (a swap): the block still names slot 0.
  const { session, uploads } = fakeSession(trained);
  await saveDraft(session, slot, renamed, 5, () => undefined);
  expect(uploads[0]).toEqual(buildSlot(model, ir, "Renamed", stored.gate, withTarget(pathSection, 5)));
  // A block that resolves nowhere is carried as stored, unless it names the slot being written (the pedal refuses that).
  await saveDraft(session, slot, renamed, null, () => undefined);
  expect(uploads[1]).toEqual(buildSlot(model, ir, "Renamed", stored.gate, pathSection));
  const own = fakeSession(withMorphBlock(await readFixture("preset/A2_decay.bin"), withTarget(pathSection, slot.slot)));
  expect(await saveDraft(own.session, slot, renamed, null, () => undefined)).toEqual({ result: "saved", morph: null });
  expect(own.uploads[0]).toEqual(buildSlot(model, ir, "Renamed"));
});

test("an upload with a new model or a morph choice carries no morph and hands the target on to be analysed", async () => {
  const { session, uploads } = fakeSession(trained);
  const replaced = withModel(baseDraft(slot), slot, newModel("Other"));
  expect(await saveDraft(session, slot, replaced, 1, () => undefined)).toEqual({ result: "saved", morph: 1 });
  expect(uploads[0]).toEqual(buildSlot(model, ir, "A2 + decay"));
  // The draft's own choice wins; "none" needs no MORPH after an upload.
  expect((await saveDraft(session, slot, { ...renamed, morph: 4 }, 1, () => undefined)).morph).toBe(4);
  expect((await saveDraft(session, slot, { ...renamed, morph: "none" }, 1, () => undefined)).morph).toBeNull();
  expect(uploads[1]).toEqual(buildSlot(model, ir, "Renamed"));
  expect(uploads[2]).toEqual(buildSlot(model, ir, "Renamed"));
});

// Slots 2 and 5 hold the fixture preset (the same model); slot 5 morphs with slot 2.
const slots: SlotMessage[] = Array.from({ length: 8 }, (_, k) =>
  k === 1 || k === 4
    ? { ...slot, slot: k }
    : { type: "slot", slot: k, status: 1, hasIr: false, playing: false, name: "", model: "", ir: "" },
);
const info = slotInfo(morphed);
const infos: (SlotInfo | null)[] = Array(8).fill(null);
infos[1] = { ...info, morph: null };
infos[4] = {
  ...info,
  morph: { ...decodeMorphBlock(morphBlock, 0), target: 1, modelCrc: info.modelCrc, waypoints: 0 },
};

test("a new model in a morph target puts the presets that morph with it up for analysis again", () => {
  const target = withModel(baseDraft(slots[1]!), slots[1]!, newModel("Other"));
  expect(morphDependents(slots, infos, { 1: target })).toEqual([[4, 1]]);
  expect(morphDependents(slots, infos, { 1: target, 4: { ...baseDraft(slots[4]!), gate } })).toEqual([[4, 1]]);
});

test("a renamed target, an erased dependent and a dependent with a model or a morph of its own are not dependents", () => {
  const target = withModel(baseDraft(slots[1]!), slots[1]!, newModel("Other"));
  const own = (d: Draft) => morphDependents(slots, infos, { 1: target, 4: d });
  expect(morphDependents(slots, infos, { 1: { ...baseDraft(slots[1]!), name: "Renamed" } })).toEqual([]);
  expect(own({ ...baseDraft(slots[4]!), model: { kind: "none" } })).toEqual([]);
  expect(own(withModel(baseDraft(slots[4]!), slots[4]!, newModel("Mine")))).toEqual([]);
  // Its own "none" stands, even when its save uploads (a rename) and so returns no morph step.
  expect(own({ ...baseDraft(slots[4]!), name: "Renamed", morph: "none" })).toEqual([]);
});
