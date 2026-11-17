// The Presets tab's unsaved edits: per slot, the name, model, IR and noise gate the user wants, each part kept from
// the pedal, removed, or replaced. Save turns a draft into an ERASE, a GATE or an UPLOAD (saveDraft).

import type { ConvertedIr } from "./ir/convert";
import { IMAGE_SIZES, SLOT_STATUS_NAMES, type SlotMessage } from "./link/protocol";
import type { PedalSession } from "./link/session";
import type { ConvertedModel } from "./nam/convert";
import { buildSlot, OFF_GATE, type SlotParts, splitSlot } from "./preset/bank";
import { decodeGate, GATE_DEFAULT, GATE_SIZE, type GateSettings } from "./preset/gate";

const OK = 0;
const EMPTY = 1;

/** A preset part: what the slot stores, nothing, or a newly converted file. */
export type Part<T> = { kind: "stored" } | { kind: "none" } | { kind: "new"; value: T };

export interface Draft {
  name: string;
  model: Part<ConvertedModel>;
  ir: Part<ConvertedIr>;
  /**
   * The gate to save, null to keep the slot's stored gate (GATE_DEFAULT for a new preset). Never compared with a
   * stored gate here: the slot infos are null while a refresh runs, and a guessed one would save over the real gate.
   */
  gate: GateSettings | null;
}

/** The draft that changes nothing: the slot as the pedal holds it (a corrupt slot drafts as empty). */
export function baseDraft(s: SlotMessage): Draft {
  if (s.status !== OK) return { name: "", model: { kind: "none" }, ir: { kind: "none" }, gate: null };
  return { name: s.name, model: { kind: "stored" }, ir: { kind: s.hasIr ? "stored" : "none" }, gate: null };
}

/** The draft differs from what the slot holds. */
export function isEdited(d: Draft, s: SlotMessage): boolean {
  const b = baseDraft(s);
  if (d.model.kind !== b.model.kind || d.ir.kind !== b.ir.kind) return true;
  if (d.model.kind === "new" || d.ir.kind === "new") return true;
  if (d.gate !== null && d.model.kind !== "none") return true;
  return d.model.kind !== "none" && d.name !== b.name;
}

/** Why the draft cannot be saved, or null. */
export function draftProblem(d: Draft): string | null {
  if (d.model.kind === "none") return d.ir.kind === "none" ? null : "An IR needs a model";
  return d.name.trim() === "" ? "A preset needs a name" : null;
}

/** The draft's model name: the new file's, the stored one's, or null. */
export function modelName(d: Draft, s: SlotMessage): string | null {
  return d.model.kind === "new" ? d.model.value.info.name : d.model.kind === "stored" ? s.model : null;
}

/** The draft's IR name: the new file's, the stored one's, or null. */
export function irName(d: Draft, s: SlotMessage): string | null {
  return d.ir.kind === "new" ? d.ir.value.info.name : d.ir.kind === "stored" ? s.ir : null;
}

/** A new model replaces the old one; the name follows it unless the user typed their own. */
export function withModel(d: Draft, s: SlotMessage, model: ConvertedModel): Draft {
  const old = modelName(d, s);
  const name = d.name.trim() === "" || d.name === old ? model.info.name : d.name;
  return { ...d, name, model: { kind: "new", value: model } };
}

/** What the slot table shows for a slot: the draft's values when it has one. */
export interface SlotView {
  state: "ok" | "empty" | "corrupt";
  name: string;
  model: string;
  ir: string;
  edited: boolean;
}

export function slotView(s: SlotMessage, d: Draft | undefined): SlotView {
  if (d === undefined || !isEdited(d, s)) {
    if (s.status === OK) return { state: "ok", name: s.name, model: s.model, ir: s.hasIr ? s.ir : "—", edited: false };
    if (s.status === EMPTY) return { state: "empty", name: "Empty", model: "", ir: "", edited: false };
    const why = SLOT_STATUS_NAMES[s.status] ?? String(s.status);
    return { state: "corrupt", name: `Corrupt (${why})`, model: "", ir: "", edited: false };
  }
  if (d.model.kind === "none")
    return { state: "empty", name: "Empty", model: "", ir: irName(d, s) ?? "", edited: true };
  return { state: "ok", name: d.name, model: modelName(d, s) ?? "", ir: irName(d, s) ?? "—", edited: true };
}

/** The image a draft saves; stored holds the slot's parts when the draft keeps any; gate is the gate to store. */
export function composeSlot(d: Draft, stored: SlotParts | null, gate: GateSettings): Uint8Array {
  const need = (what: string): SlotParts => {
    if (stored === null) throw new Error(`The stored ${what} is not available`);
    return stored;
  };
  if (d.model.kind === "none") throw new Error("A preset needs a model");
  const model = d.model.kind === "new" ? d.model.value.bin : need("model").model;
  let ir: Uint8Array | null = null;
  if (d.ir.kind === "new") ir = d.ir.value.bin;
  else if (d.ir.kind === "stored") {
    ir = need("IR").ir;
    if (ir === null) throw new Error("The slot no longer has an IR");
  }
  return buildSlot(model, ir, d.name.trim(), gate);
}

/**
 * Applies a draft to its slot: ERASE when its model is removed; GATE when only the gate changed (no upload, so the
 * playing preset is not reloaded); else UPLOAD of the composed preset, reading the slot back first when the draft
 * keeps a stored part or the stored gate. Returns what was done.
 */
export async function saveDraft(
  session: PedalSession,
  s: SlotMessage,
  d: Draft,
  onProgress: (fraction: number) => void,
): Promise<"erased" | "saved"> {
  const problem = draftProblem(d);
  if (problem !== null) throw new Error(`Slot ${s.slot + 1}: ${problem}`);
  if (d.model.kind === "none") {
    await session.erase(s.slot);
    return "erased";
  }
  if (
    d.gate !== null &&
    d.model.kind === "stored" &&
    d.ir.kind === (s.hasIr ? "stored" : "none") &&
    d.name === s.name
  ) {
    await session.gate(s.slot, d.gate);
    return "saved";
  }
  let stored: SlotParts | null = null;
  if (d.model.kind === "stored" || d.ir.kind === "stored") {
    if (s.status !== OK || (d.ir.kind === "stored" && !s.hasIr))
      throw new Error(`Slot ${s.slot + 1} changed on the pedal: revert it and edit again`);
    stored = splitSlot(await session.read(s.slot, 0, IMAGE_SIZES[s.hasIr ? 1 : 0]));
  }
  const gate =
    d.gate ??
    stored?.gate ??
    (s.status === OK ? decodeGate(await session.read(s.slot, OFF_GATE, GATE_SIZE)) : GATE_DEFAULT);
  await session.upload(s.slot, composeSlot(d, stored, gate), (sent, size) => onProgress(sent / size));
  return "saved";
}
