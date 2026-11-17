// The Presets tab's file handling: .nam and .wav files through the shared converters, their rejections as text, and
// the warnings worth reading before a save.
import { DEFAULT_MODEL_INPUT_DBU } from "./controls";
import { errorText, signedDb, tapsMs } from "./format";
import { type ConvertedIr, convertIr, IR_FADE, IR_TAPS, IrRejected } from "./ir/convert";
import { type ConvertedModel, convertNam, ModelRejected } from "./nam/convert";
import type { StoredIr, StoredModel } from "./preset/info";

/** An IR's metadata, stored or newly converted; truncated is known only for a new one. */
export type IrMeta = StoredIr & { truncated?: boolean };

export const loadModel = async (file: File): Promise<ConvertedModel> =>
  convertNam(await file.text(), file.name.replace(/\.nam$/i, ""));

export const loadIr = async (file: File): Promise<ConvertedIr> =>
  convertIr(new Uint8Array(await file.arrayBuffer()), file.name.replace(/\.wav$/i, ""));

export function rejectionText(e: unknown): string {
  if (e instanceof ModelRejected) return `Model rejected (${e.reason}): ${e.detail}`;
  if (e instanceof IrRejected) return `IR rejected (${e.reason}): ${e.detail}`;
  return errorText(e);
}

export function importWarnings(model: StoredModel, ir: IrMeta | null): string[] {
  const out: string[] = [];
  if (model.loudness === null) out.push("No loudness: the pedal plays it at unity, not level-matched");
  if (model.inputLevelDbu === null)
    out.push(`No input_level_dbu: assumed captured at ${signedDb(DEFAULT_MODEL_INPUT_DBU, "dBu")}`);
  if (ir !== null && model.gearType !== null && /cab/i.test(model.gearType))
    out.push(`The model's gear type "${model.gearType}" already includes a cab`);
  if (ir?.truncated)
    out.push(`The IR is longer than ${IR_TAPS} taps (${tapsMs(IR_TAPS)} ms): truncated with a ${IR_FADE}-tap fade`);
  return out;
}
