// The Presets tab's unsaved edits: per slot, the name, model, IR, noise gate and morph target the user wants, each
// part kept from the pedal, removed, or replaced. Save turns a draft into an ERASE, a GATE or an UPLOAD (saveDraft),
// then the morph targets it reports are analysed and written (morph/save.ts). An upload that keeps the stored model
// and leaves the morph alone carries the stored morph over (its block and a trained path's waypoints).

import type { ConvertedIr } from "./ir/convert";
import { SLOT_STATUS, SLOT_STATUS_NAMES, type SlotMessage } from "./link/protocol";
import type { PedalSession } from "./link/session";
import type { ConvertedModel } from "./nam/convert";
import { buildSlot, OFF_GATE, type SlotParts, splitSlot } from "./preset/bank";
import { decodeGate, GATE_DEFAULT, GATE_SIZE, type GateSettings } from "./preset/gate";
import type { SlotInfo } from "./preset/info";
import { resolveMorph, withTarget } from "./preset/morph";

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
  /** The slot (0-7) to morph with, "none" for no morph, null to keep the slot's stored morph. */
  morph: number | "none" | null;
}

/** Unsaved drafts by slot. */
export type Drafts = Readonly<Partial<Record<number, Draft>>>;

/** The draft that changes nothing: the slot as the pedal holds it (a corrupt slot drafts as empty). */
export function baseDraft(s: SlotMessage): Draft {
  if (s.status !== SLOT_STATUS.ok)
    return { name: "", model: { kind: "none" }, ir: { kind: "none" }, gate: null, morph: null };
  return {
    name: s.name,
    model: { kind: "stored" },
    ir: { kind: s.hasIr ? "stored" : "none" },
    gate: null,
    morph: null,
  };
}

/** The draft differs from what the slot holds. */
export function isEdited(d: Draft, s: SlotMessage): boolean {
  const b = baseDraft(s);
  if (d.model.kind !== b.model.kind || d.ir.kind !== b.ir.kind) return true;
  if (d.model.kind === "new" || d.ir.kind === "new") return true;
  if ((d.gate !== null || d.morph !== null) && d.model.kind !== "none") return true;
  return d.model.kind !== "none" && d.name !== b.name;
}

/** Why the draft cannot be saved, or null. */
export function draftProblem(d: Draft): string | null {
  if (d.model.kind === "none") return d.ir.kind === "none" ? null : "An IR needs a model";
  return d.name.trim() === "" ? "A preset needs a name" : null;
}

const partName = (p: Part<{ info: { name: string } }>, stored: string): string | null =>
  p.kind === "new" ? p.value.info.name : p.kind === "stored" ? stored : null;

/** The draft's model name: the new file's, the stored one's, or null. */
export const modelName = (d: Draft, s: SlotMessage): string | null => partName(d.model, s.model);

/** The draft's IR name: the new file's, the stored one's, or null. */
export const irName = (d: Draft, s: SlotMessage): string | null => partName(d.ir, s.ir);

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
    if (s.status === SLOT_STATUS.ok)
      return { state: "ok", name: s.name, model: s.model, ir: s.hasIr ? s.ir : "—", edited: false };
    if (s.status === SLOT_STATUS.empty) return { state: "empty", name: "Empty", model: "", ir: "", edited: false };
    const why = SLOT_STATUS_NAMES[s.status] ?? String(s.status);
    return { state: "corrupt", name: `Corrupt (${why})`, model: "", ir: "", edited: false };
  }
  if (d.model.kind === "none")
    return { state: "empty", name: "Empty", model: "", ir: irName(d, s) ?? "", edited: true };
  return { state: "ok", name: d.name, model: modelName(d, s) ?? "", ir: irName(d, s) ?? "—", edited: true };
}

/**
 * The image a draft saves; stored holds the slot's parts when the draft keeps any; gate is the gate to store and
 * morph the morph section to store (null: none).
 */
export function composeSlot(
  d: Draft,
  stored: SlotParts | null,
  gate: GateSettings,
  morph: Uint8Array | null,
): Uint8Array {
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
  return buildSlot(model, ir, d.name.trim(), gate, morph);
}

/** What saveDraft did, and the morph step that must follow it once every draft is written (morph/save.ts). */
export interface DraftSaved {
  result: "erased" | "saved";
  /** The slot to morph with (to analyse and write), "none" to remove the stored morph, null for nothing to do. */
  morph: number | "none" | null;
}

/**
 * Applies a draft to its slot: ERASE when its model is removed; GATE when the model, the IR and the name are kept and
 * the gate changed (no upload, so the playing preset is not reloaded); else UPLOAD of the composed preset, reading the
 * slot back first when the draft keeps a stored part or the stored gate. An upload that keeps the stored model and
 * whose draft leaves the morph alone carries the stored morph section over (the block, aimed at storedMorph when it
 * resolves there, and any waypoints) and reports nothing to analyse; a section whose target would be the slot itself
 * (the pedal refuses it) is left out. Any other upload carries no morph: the draft's choice is returned to be
 * analysed, or with a new model and no choice the stored target (storedMorph: resolveMorph's, null for none), to be
 * analysed against the new model.
 */
export async function saveDraft(
  session: PedalSession,
  s: SlotMessage,
  d: Draft,
  storedMorph: number | null,
  onProgress: (fraction: number) => void,
): Promise<DraftSaved> {
  const problem = draftProblem(d);
  if (problem !== null) throw new Error(`Slot ${s.slot + 1}: ${problem}`);
  if (d.model.kind === "none") {
    await session.erase(s.slot);
    return { result: "erased", morph: null };
  }
  const keeps = d.model.kind === "stored" && d.ir.kind === (s.hasIr ? "stored" : "none") && d.name === s.name;
  if (keeps) {
    if (d.gate !== null) await session.gate(s.slot, d.gate);
    return { result: "saved", morph: d.morph };
  }
  let stored: SlotParts | null = null;
  if (d.model.kind === "stored" || d.ir.kind === "stored") {
    if (s.status !== SLOT_STATUS.ok || (d.ir.kind === "stored" && !s.hasIr))
      throw new Error(`Slot ${s.slot + 1} changed on the pedal: revert it and edit again`);
    stored = splitSlot(await session.readImage(s.slot));
  }
  const gate =
    d.gate ??
    stored?.gate ??
    (s.status === SLOT_STATUS.ok ? decodeGate(await session.read(s.slot, OFF_GATE, GATE_SIZE)) : GATE_DEFAULT);
  const carries = d.model.kind === "stored" && d.morph === null;
  let morph: Uint8Array | null = null;
  if (carries && stored?.morph) {
    morph = storedMorph === null ? stored.morph : withTarget(stored.morph, storedMorph);
    if (morph[0] === s.slot) morph = null;
  }
  await session.upload(s.slot, composeSlot(d, stored, gate, morph), (sent, size) => onProgress(sent / size));
  if (carries) return { result: "saved", morph: null };
  return { result: "saved", morph: d.morph === "none" ? null : (d.morph ?? storedMorph) };
}

/**
 * The stored morphs a save of drafts breaks and must analyse again: [slot, target] for every ok slot that keeps its
 * own model and its stored morph and whose morph target gets a new model. A slot that gets a new model or a morph
 * choice of its own is saveDraft's to report; a target that is erased is not analysed again (the pedal turns that
 * morph off).
 */
export function morphDependents(
  slots: readonly SlotMessage[],
  infos: readonly (SlotInfo | null)[],
  drafts: Drafts,
): [slot: number, target: number][] {
  const out: [number, number][] = [];
  for (const s of slots) {
    const own = drafts[s.slot];
    if (s.status !== SLOT_STATUS.ok || (own !== undefined && (own.model.kind !== "stored" || own.morph !== null)))
      continue;
    const target = resolveMorph(infos, s.slot);
    if (target !== null && drafts[target]?.model.kind === "new") out.push([s.slot, target]);
  }
  return out;
}
