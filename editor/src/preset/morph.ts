// A preset's morph block (daisy/src/morph.h, the slot layout in daisy/src/preset.h): the preset it morphs with and
// the alignment of that preset's model to its own (morph/align.ts). Pure TypeScript: shared by the slot reader, the
// link and tools/pedal.ts.
//
// The 64-byte block, little-endian: u8 target (slot 0-7), u8 perm[3], u32 the target model binary's stored CRC,
// f32 scale[3], f32 m[9] (row-major), f32 fit, f32 unalignedFit.

import type { MorphFit } from "../morph/align";
import type { SlotInfo } from "./info";

export const MORPH_BLOCK_SIZE = 64;
const OFF_PERM = 1;
const OFF_MODEL_CRC = 4;
const OFF_SCALE = 8;
const OFF_M = 20;
const OFF_FIT = 56;
const OFF_UNALIGNED_FIT = 60;

export interface MorphBlock extends MorphFit {
  /** The slot (0-7) the preset morphs with when the block was written. */
  target: number;
  /** That slot's model then: the model binary's stored CRC. */
  modelCrc: number;
}

export function encodeMorphBlock(b: MorphBlock): Uint8Array {
  const out = new Uint8Array(MORPH_BLOCK_SIZE);
  const dv = new DataView(out.buffer);
  out[0] = b.target;
  out.set(b.perm, OFF_PERM);
  dv.setUint32(OFF_MODEL_CRC, b.modelCrc, true);
  b.scale.forEach((v, i) => {
    dv.setFloat32(OFF_SCALE + 4 * i, v, true);
  });
  b.m.forEach((v, i) => {
    dv.setFloat32(OFF_M + 4 * i, v, true);
  });
  dv.setFloat32(OFF_FIT, b.fit, true);
  dv.setFloat32(OFF_UNALIGNED_FIT, b.unalignedFit, true);
  return out;
}

/** The block at bytes[at]; nothing is checked (the slot's CRC and the pedal's parser guard it). */
export function decodeMorphBlock(bytes: Uint8Array, at: number): MorphBlock {
  const dv = new DataView(bytes.buffer, bytes.byteOffset + at, MORPH_BLOCK_SIZE);
  const f32 = (off: number, n: number): number[] =>
    Array.from({ length: n }, (_, i) => dv.getFloat32(off + 4 * i, true));
  return {
    target: dv.getUint8(0),
    perm: [dv.getUint8(OFF_PERM), dv.getUint8(OFF_PERM + 1), dv.getUint8(OFF_PERM + 2)],
    modelCrc: dv.getUint32(OFF_MODEL_CRC, true),
    scale: f32(OFF_SCALE, 3) as [number, number, number],
    m: f32(OFF_M, 9),
    fit: dv.getFloat32(OFF_FIT, true),
    unalignedFit: dv.getFloat32(OFF_UNALIGNED_FIT, true),
  };
}

/**
 * The slot that `slot`'s preset morphs with, as the pedal resolves it (daisy/src/preset.h ResolveMorph): the block's
 * target while it still holds the model the block was fitted to, else the first other slot that holds it; null with
 * no block or no such slot. infos: each slot's metadata, null where empty or unread.
 */
export function resolveMorph(infos: readonly (SlotInfo | null)[], slot: number): number | null {
  const block = infos[slot]?.morph;
  if (!block) return null;
  if (block.target !== slot && infos[block.target]?.modelCrc === block.modelCrc) return block.target;
  const k = infos.findIndex((info, i) => i !== slot && info?.modelCrc === block.modelCrc);
  return k < 0 ? null : k;
}
