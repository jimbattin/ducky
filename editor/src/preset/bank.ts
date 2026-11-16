// Presets and the preset bank, format 2. Spec: daisy/src/preset.h and docs/FIRMWARE.md "Preset store".
// Pure TypeScript (no DOM, no Bun APIs): shared by the editor and tools/preset_bank.ts.
//
// A slot: "DKPS", u16 version, u16 flags (bit 0 PRESET_FLAG_HAS_IR), u32 size, name[64], the model binary, the IR
// binary when flagged, then the CRC-32 of everything before it (little-endian). The bank is SLOT_COUNT slots of
// SLOT_BYTES, erased (0xFF) where no preset is, written by DFU at BANK_ADDRESS.

import { IR_BIN_SIZE, IR_MAGIC } from "../ir/convert";
import { MODEL_BIN_SIZE, MODEL_MAGIC } from "../nam/convert";
import { crc32 } from "../nam/crc32";
import { sanitiseName } from "../name";

export const PRESET_MAGIC = "DKPS";
export const PRESET_FORMAT_VERSION = 2;
export const PRESET_FLAG_HAS_IR = 0x0001;
export const PRESET_HEADER_SIZE = 76;
export const PRESET_NAME_SIZE = 64;
export const SLOT_COUNT = 8;
export const SLOT_BYTES = 0x10000;
export const BANK_BYTES = SLOT_COUNT * SLOT_BYTES;
export const BANK_ADDRESS = 0x90400000;

const OFF_VERSION = 4;
export const OFF_FLAGS = 6;
const OFF_SIZE = 8;
export const OFF_NAME = 12;

export class PresetError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "PresetError";
  }
}

function ascii(bytes: Uint8Array, at: number, s: string): void {
  for (let i = 0; i < s.length; i++) bytes[at + i] = s.charCodeAt(i);
}

// A model or IR binary: its length, magic and trailing CRC.
function checkBin(what: string, bin: Uint8Array, size: number, magic: string): void {
  if (bin.length !== size) throw new PresetError(`${what}: size ${bin.length}, need ${size}`);
  if (String.fromCharCode(...bin.subarray(0, 4)) !== magic) throw new PresetError(`${what}: magic, need ${magic}`);
  const stored = new DataView(bin.buffer, bin.byteOffset, bin.length).getUint32(size - 4, true);
  if (stored !== crc32(bin.subarray(0, size - 4))) throw new PresetError(`${what}: crc`);
}

// One slot's preset: exactly PRESET_HEADER_SIZE + MODEL_BIN_SIZE + 4 bytes, plus IR_BIN_SIZE with an IR.
export function buildSlot(model: Uint8Array, ir: Uint8Array | null, name: string): Uint8Array {
  checkBin("model", model, MODEL_BIN_SIZE, MODEL_MAGIC);
  if (ir !== null) checkBin("ir", ir, IR_BIN_SIZE, IR_MAGIC);
  const size = PRESET_HEADER_SIZE + MODEL_BIN_SIZE + (ir === null ? 0 : IR_BIN_SIZE) + 4;
  const out = new Uint8Array(size);
  const dv = new DataView(out.buffer);
  ascii(out, 0, PRESET_MAGIC);
  dv.setUint16(OFF_VERSION, PRESET_FORMAT_VERSION, true);
  dv.setUint16(OFF_FLAGS, ir === null ? 0 : PRESET_FLAG_HAS_IR, true);
  dv.setUint32(OFF_SIZE, size, true);
  ascii(out, OFF_NAME, sanitiseName(name)); // at most 63 characters: the rest stays NUL
  out.set(model, PRESET_HEADER_SIZE);
  if (ir !== null) out.set(ir, PRESET_HEADER_SIZE + MODEL_BIN_SIZE);
  dv.setUint32(size - 4, crc32(out.subarray(0, size - 4)), true);
  return out;
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
