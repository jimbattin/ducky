// Presets and the preset bank, format 3. Spec: daisy/src/preset.h and docs/FIRMWARE.md "Preset store".
// Pure TypeScript (no DOM, no Bun APIs): shared by the editor and tools/preset_bank.ts.
//
// A slot: "DKPS", u16 version, u16 flags (bit 0 PRESET_FLAG_HAS_IR, bit 1 PRESET_FLAG_HAS_MORPH, bits 2-3 the morph's
// waypoint count), u32 size, name[64], the gate block (gate.ts), the model binary, the IR binary when flagged, the
// morph section (morph.ts: the morph block, then the waypoints) when flagged, then the CRC-32 of everything before it
// (little-endian). A morph section gets into a slot with the image (an upload may carry one) or by the link's MORPH,
// which stores a block without waypoints. The bank is SLOT_COUNT slots of SLOT_BYTES, erased (0xFF) where no preset
// is, written by DFU at BANK_ADDRESS.

import { crc32 } from "../crc32";
import { sanitiseName, writeAscii } from "../device";
import { IR_BIN_SIZE, IR_MAGIC } from "../ir/convert";
import { MODEL_BIN_SIZE, MODEL_MAGIC } from "../nam/convert";
import { decodeGate, encodeGate, GATE_DEFAULT, type GateSettings, gateProblem } from "./gate";
import { MORPH_BLOCK_SIZE, MORPH_WAYPOINT_BYTES, sectionWaypoints } from "./morph";

const PRESET_MAGIC = "DKPS";
const PRESET_FORMAT_VERSION = 3;
export const PRESET_FLAG_HAS_IR = 0x0001;
export const PRESET_FLAG_HAS_MORPH = 0x0002;
/** The morph's waypoint count (0-3): flags bits 2-3. Zero without PRESET_FLAG_HAS_MORPH. */
export const PRESET_FLAG_WAYPOINTS_SHIFT = 2;
export const PRESET_FLAG_WAYPOINTS_MASK = 0x000c;
const PRESET_FLAGS_KNOWN = PRESET_FLAG_HAS_IR | PRESET_FLAG_HAS_MORPH | PRESET_FLAG_WAYPOINTS_MASK;
export const PRESET_HEADER_SIZE = 84;
const PRESET_NAME_SIZE = 64;
export const SLOT_COUNT = 8;
export const SLOT_BYTES = 0x10000;
export const BANK_BYTES = SLOT_COUNT * SLOT_BYTES;
export const BANK_ADDRESS = 0x90400000;

const OFF_VERSION = 4;
export const OFF_FLAGS = 6;
export const OFF_SIZE = 8;
export const OFF_NAME = 12;
export const OFF_GATE = 76;

/** A preset image's size (preset.h ImageSize); waypoints: the morph's waypoint count. */
export function imageSize(hasIr: boolean, hasMorph: boolean, waypoints = 0): number {
  const morph = hasMorph ? MORPH_BLOCK_SIZE + waypoints * MORPH_WAYPOINT_BYTES : 0;
  return PRESET_HEADER_SIZE + MODEL_BIN_SIZE + (hasIr ? IR_BIN_SIZE : 0) + morph + 4;
}

/** The waypoint count in a preset's flags. */
export const flagWaypoints = (flags: number): number =>
  (flags & PRESET_FLAG_WAYPOINTS_MASK) >> PRESET_FLAG_WAYPOINTS_SHIFT;

export class PresetError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "PresetError";
  }
}

// A model or IR binary: its length, magic and trailing CRC.
function checkBin(what: string, bin: Uint8Array, size: number, magic: string): void {
  if (bin.length !== size) throw new PresetError(`${what}: size ${bin.length}, need ${size}`);
  if (String.fromCharCode(...bin.subarray(0, 4)) !== magic) throw new PresetError(`${what}: magic, need ${magic}`);
  const stored = new DataView(bin.buffer, bin.byteOffset, bin.length).getUint32(size - 4, true);
  if (stored !== crc32(bin.subarray(0, size - 4))) throw new PresetError(`${what}: crc`);
}

// One slot's preset: imageSize bytes. morph is a morph section (morph.ts morphSection: the block, then 0-3
// waypoints; its length gives the count), stored as given.
export function buildSlot(
  model: Uint8Array,
  ir: Uint8Array | null,
  name: string,
  gate: GateSettings = GATE_DEFAULT,
  morph: Uint8Array | null = null,
): Uint8Array {
  checkBin("model", model, MODEL_BIN_SIZE, MODEL_MAGIC);
  if (ir !== null) checkBin("ir", ir, IR_BIN_SIZE, IR_MAGIC);
  const problem = gateProblem(gate);
  if (problem !== null) throw new PresetError(`gate: ${problem}`);
  const waypoints = morph === null ? 0 : sectionWaypoints(morph.length);
  if (waypoints === null) throw new PresetError(`morph: size ${morph!.length}`);
  const size = imageSize(ir !== null, morph !== null, waypoints);
  const flags =
    (ir === null ? 0 : PRESET_FLAG_HAS_IR) |
    (morph === null ? 0 : PRESET_FLAG_HAS_MORPH) |
    (waypoints << PRESET_FLAG_WAYPOINTS_SHIFT);
  const out = new Uint8Array(size);
  const dv = new DataView(out.buffer);
  writeAscii(out, 0, PRESET_MAGIC);
  dv.setUint16(OFF_VERSION, PRESET_FORMAT_VERSION, true);
  dv.setUint16(OFF_FLAGS, flags, true);
  dv.setUint32(OFF_SIZE, size, true);
  writeAscii(out, OFF_NAME, sanitiseName(name)); // at most 63 characters: the rest stays NUL
  out.set(encodeGate(gate), OFF_GATE);
  out.set(model, PRESET_HEADER_SIZE);
  if (ir !== null) out.set(ir, PRESET_HEADER_SIZE + MODEL_BIN_SIZE);
  if (morph !== null) out.set(morph, size - 4 - morph.length);
  dv.setUint32(size - 4, crc32(out.subarray(0, size - 4)), true);
  return out;
}

/** A stored preset's parts, as buildSlot takes them. */
export interface SlotParts {
  name: string;
  model: Uint8Array;
  ir: Uint8Array | null;
  gate: GateSettings;
  /** The morph section (the block, then its waypoints); null without a morph. */
  morph: Uint8Array | null;
}

// A whole preset image (as READ returns it) split into its name, gate, model, IR and morph section. The preset's
// flags, size and CRC are checked here and the model's and IR's own CRCs by buildSlot when the parts are reused.
export function splitSlot(image: Uint8Array): SlotParts {
  if (image.length < PRESET_HEADER_SIZE) throw new PresetError(`preset: ${image.length} bytes`);
  if (String.fromCharCode(...image.subarray(0, 4)) !== PRESET_MAGIC) throw new PresetError("preset: magic");
  const dv = new DataView(image.buffer, image.byteOffset, image.length);
  const version = dv.getUint16(OFF_VERSION, true);
  if (version !== PRESET_FORMAT_VERSION)
    throw new PresetError(`preset: version ${version}, need ${PRESET_FORMAT_VERSION}`);
  const flags = dv.getUint16(OFF_FLAGS, true);
  const hasIr = (flags & PRESET_FLAG_HAS_IR) !== 0;
  const hasMorph = (flags & PRESET_FLAG_HAS_MORPH) !== 0;
  if ((flags & ~PRESET_FLAGS_KNOWN) !== 0 || (!hasMorph && flagWaypoints(flags) !== 0))
    throw new PresetError(`preset: flags 0x${flags.toString(16)}`);
  const modelEnd = PRESET_HEADER_SIZE + MODEL_BIN_SIZE;
  const irEnd = modelEnd + (hasIr ? IR_BIN_SIZE : 0);
  const size = imageSize(hasIr, hasMorph, flagWaypoints(flags));
  if (dv.getUint32(OFF_SIZE, true) !== size || image.length !== size)
    throw new PresetError(`preset: size ${image.length}, need ${size}`);
  if (dv.getUint32(size - 4, true) !== crc32(image.subarray(0, size - 4))) throw new PresetError("preset: crc");
  const nameBytes = image.subarray(OFF_NAME, OFF_NAME + PRESET_NAME_SIZE);
  const end = nameBytes.indexOf(0);
  return {
    name: String.fromCharCode(...nameBytes.subarray(0, end < 0 ? nameBytes.length : end)),
    model: image.slice(PRESET_HEADER_SIZE, modelEnd),
    ir: hasIr ? image.slice(modelEnd, irEnd) : null,
    gate: decodeGate(image, OFF_GATE),
    morph: hasMorph ? image.slice(irEnd, size - 4) : null,
  };
}

// The BANK_BYTES image: slot k at k * SLOT_BYTES, 0xFF everywhere else.
export function buildBank(slots: readonly (Uint8Array | null)[]): Uint8Array {
  if (slots.length !== SLOT_COUNT) throw new PresetError(`bank: ${slots.length} slots, need ${SLOT_COUNT}`);
  const bank = new Uint8Array(BANK_BYTES).fill(0xff);
  slots.forEach((slot, k) => {
    if (slot === null) return;
    if (slot.length > SLOT_BYTES) throw new PresetError(`slot ${k + 1}: ${slot.length} bytes, max ${SLOT_BYTES}`);
    bank.set(slot, k * SLOT_BYTES);
  });
  return bank;
}
