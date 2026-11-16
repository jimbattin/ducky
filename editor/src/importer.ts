// The import panel's file handling: .nam and .wav files through the shared converters, their rejections as text, and
// the warnings worth reading before an upload.
import { type ConvertedIr, convertIr, IR_FADE, IR_TAPS, type IrInfo, IrRejected } from "./ir/convert";
import { type ConvertedModel, convertNam, type ModelInfo, ModelRejected } from "./nam/convert";

export const loadModel = async (file: File): Promise<ConvertedModel> =>
  convertNam(await file.text(), file.name.replace(/\.nam$/i, ""));

export const loadIr = async (file: File): Promise<ConvertedIr> =>
  convertIr(new Uint8Array(await file.arrayBuffer()), file.name.replace(/\.wav$/i, ""));

export function rejectionText(e: unknown): string {
  if (e instanceof ModelRejected) return `Model rejected (${e.reason}): ${e.detail}`;
  if (e instanceof IrRejected) return `IR rejected (${e.reason}): ${e.detail}`;
  return e instanceof Error ? e.message : String(e);
}

export function importWarnings(model: ModelInfo, ir: IrInfo | null): string[] {
  const out: string[] = [];
  if (model.loudness === null) out.push("No loudness: the pedal plays it at unity, not level-matched");
  if (model.inputLevelDbu === null) out.push("No input_level_dbu: assumed captured at +12 dBu");
  if (ir !== null && model.gearType !== null && /cab/i.test(model.gearType))
    out.push(`The model's gear type "${model.gearType}" already includes a cab`);
  if (ir?.truncated)
    out.push(
      `The IR is longer than ${IR_TAPS} taps (${(IR_TAPS / 48).toFixed(1)} ms): truncated with a ${IR_FADE}-tap fade`,
    );
  return out;
}
