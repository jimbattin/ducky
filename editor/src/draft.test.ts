import { expect, test } from "bun:test";
import { baseDraft, composeSlot, type Draft, isEdited, saveDraft, withModel } from "./draft";
import type { SlotMessage } from "./link/protocol";
import type { PedalSession } from "./link/session";
import type { ConvertedModel, ModelInfo } from "./nam/convert";
import { buildSlot, splitSlot } from "./preset/bank";
import type { GateSettings } from "./preset/gate";

const FIXTURES = `${import.meta.dir}/../../tests/fixtures`;
const read = async (rel: string) => new Uint8Array(await Bun.file(`${FIXTURES}/${rel}`).arrayBuffer());
const model = await read("nam/A2.bin");
const ir = await read("ir/decay.bin");
const stored = splitSlot(await read("preset/A2_decay.bin"));

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
  expect(composeSlot(d, stored, stored.gate)).toEqual(buildSlot(model, null, "A2 + decay"));
});

test("replacing the model keeps the stored IR", () => {
  const d = withModel(baseDraft(slot), slot, newModel("Other"));
  expect(composeSlot(d, stored, stored.gate)).toEqual(buildSlot(model, ir, "A2 + decay"));
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

test("saving a gate-only draft sends GATE and never uploads", async () => {
  const calls: string[] = [];
  const session = {
    gate: async (k: number, g: GateSettings) => {
      calls.push(`gate ${k} ${g.thresholdDb}`);
      return 0;
    },
    upload: async () => {
      calls.push("upload");
      return { flashMs: 0, retries: 0 };
    },
    read: async () => {
      calls.push("read");
      return new Uint8Array();
    },
  } as unknown as PedalSession;
  expect(await saveDraft(session, slot, { ...baseDraft(slot), gate }, () => undefined)).toBe("saved");
  expect(calls).toEqual(["gate 2 -55"]);
});
