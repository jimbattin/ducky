// What a stored preset holds, decoded from its image (daisy/src/preset.h, docs/NAM_FORMAT.md "Ducky model binary
// (format 2)", docs/FILTERS.md "IR binary (format 1)"). Pure TypeScript: the editor and tools/pedal.ts read slots back
// over the link (READ) and decode only the bytes in infoRanges.

import * as ir from "../ir/convert";
import * as model from "../nam/convert";
import * as preset from "./bank";
import { decodeGate, type GateSettings } from "./gate";
import { decodeMorphBlock, MORPH_BLOCK_SIZE, type MorphBlock } from "./morph";

/** A stored model's metadata. convert.ts's ModelInfo is assignable to it. */
export interface StoredModel {
  name: string;
  gearType: string | null;
  gearMake: string | null;
  gearModel: string | null;
  modeledBy: string | null;
  loudness: number | null;
  inputLevelDbu: number | null;
}

/** A stored IR's metadata. ir/convert.ts's IrInfo is assignable to it. */
export interface StoredIr {
  name: string;
  sourceRate: number;
  sourceFrames: number;
  sourceChannels: number;
  taps: number;
  gainDb: number;
}

export interface SlotInfo {
  name: string;
  model: StoredModel;
  /** The model binary's stored CRC: what a morph block names its target's model by. */
  modelCrc: number;
  ir: StoredIr | null;
  gate: GateSettings;
  morph: MorphBlock | null;
}

const MODEL_AT = preset.PRESET_HEADER_SIZE;
const IR_AT = preset.PRESET_HEADER_SIZE + model.MODEL_BIN_SIZE;
const TEXT_SIZE = 64;

/** The [offset, length] runs of a preset image that slotInfo reads. */
export function infoRanges(hasIr: boolean, hasMorph: boolean): [offset: number, length: number][] {
  const ranges: [number, number][] = [
    [0, MODEL_AT + 12], // the preset header (the gate block included), then the model's magic, version, flags and size
    [MODEL_AT + model.OFF_LOUDNESS, model.MODEL_BIN_SIZE - model.OFF_LOUDNESS], // loudness .. modeled_by, the CRC
  ];
  if (hasIr) ranges.push([IR_AT + ir.OFF_TAP_COUNT, ir.OFF_TAPS - ir.OFF_TAP_COUNT]); // tap count .. name
  if (hasMorph) ranges.push([preset.imageSize(hasIr, true) - 4 - MORPH_BLOCK_SIZE, MORPH_BLOCK_SIZE]);
  return ranges;
}

/** A NUL-padded text field: up to the first NUL within 64 bytes, non-printables as '?'. */
function text(image: Uint8Array, at: number): string {
  let out = "";
  for (let i = 0; i < TEXT_SIZE; i++) {
    const b = image[at + i]!;
    if (b === 0) break;
    out += b >= 0x20 && b <= 0x7e ? String.fromCharCode(b) : "?";
  }
  return out;
}

function textOrNull(image: Uint8Array, at: number): string | null {
  const s = text(image, at);
  return s === "" ? null : s;
}

/** Decodes a preset image's metadata from the bytes in infoRanges only; no CRC is checked. */
export function slotInfo(image: Uint8Array): SlotInfo {
  const dv = new DataView(image.buffer, image.byteOffset, image.length);
  const flags = dv.getUint16(preset.OFF_FLAGS, true);
  const hasIr = (flags & preset.PRESET_FLAG_HAS_IR) !== 0;
  const hasMorph = (flags & preset.PRESET_FLAG_HAS_MORPH) !== 0;
  const mflags = dv.getUint16(MODEL_AT + model.OFF_FLAGS, true);
  const m: StoredModel = {
    name: text(image, MODEL_AT + model.OFF_NAME),
    gearType: textOrNull(image, MODEL_AT + model.OFF_GEAR_TYPE),
    gearMake: textOrNull(image, MODEL_AT + model.OFF_GEAR_MAKE),
    gearModel: textOrNull(image, MODEL_AT + model.OFF_GEAR_MODEL),
    modeledBy: textOrNull(image, MODEL_AT + model.OFF_MODELED_BY),
    loudness: mflags & model.FLAG_HAS_LOUDNESS ? dv.getFloat32(MODEL_AT + model.OFF_LOUDNESS, true) : null,
    inputLevelDbu: mflags & model.FLAG_HAS_INPUT_LEVEL ? dv.getFloat32(MODEL_AT + model.OFF_INPUT_LEVEL, true) : null,
  };
  const i: StoredIr | null = hasIr
    ? {
        name: text(image, IR_AT + ir.OFF_NAME),
        sourceRate: dv.getUint32(IR_AT + ir.OFF_SOURCE_RATE, true),
        sourceFrames: dv.getUint32(IR_AT + ir.OFF_SOURCE_FRAMES, true),
        sourceChannels: dv.getUint16(IR_AT + ir.OFF_SOURCE_CHANNELS, true),
        taps: dv.getUint32(IR_AT + ir.OFF_TAP_COUNT, true),
        gainDb: 20 * Math.log10(dv.getFloat32(IR_AT + ir.OFF_GAIN, true)),
      }
    : null;
  return {
    name: text(image, preset.OFF_NAME),
    model: m,
    modelCrc: dv.getUint32(MODEL_AT + model.OFF_CRC, true),
    ir: i,
    gate: decodeGate(image, preset.OFF_GATE),
    morph: hasMorph ? decodeMorphBlock(image, preset.imageSize(hasIr, true) - 4 - MORPH_BLOCK_SIZE) : null,
  };
}
