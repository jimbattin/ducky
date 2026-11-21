// A preset's morph section (daisy/src/morph.h, the slot layout in daisy/src/preset.h): the morph block, which names
// the preset it morphs with and the alignment of that preset's model to its own (morph/align.ts), then the waypoints
// of a trained path, if any. Pure TypeScript: shared by the slot reader, the link and tools/pedal.ts.
//
// The 64-byte block, little-endian: u8 target (slot 0-7), u8 perm[3], u32 the target model binary's stored CRC,
// f32 scale[3], f32 m[9] (row-major), f32 fit, f32 unalignedFit.
//
// A waypoint is a network between the two models: 1870 little-endian float32, a weight stream in the model binary's
// order with the head scale folded into the head, in the preset's own model's coordinates (what align.ts
// applyTransform gives for the identity). With K waypoints the morph sweeps own model -> waypoint 1 -> .. ->
// waypoint K -> target model in K + 1 equal segments; with none, the straight blend. A trained path's block holds
// the identity transform, the path's fit and the straight unaligned blend's.
//
// The trainer's path file (tools/morph_train.py), little-endian: "DKMP", u32 version 1, u32 K (1-3), u32 model A's
// stored CRC, u32 model B's, f32 the path's fit, f32 the straight unaligned blend's fit, then the K waypoints.

import type { MorphFit } from "../morph/align";
import type { SlotInfo } from "./info";

export const MORPH_BLOCK_SIZE = 64;
/** A waypoint: the model binary's 1870 network weights as float32. */
export const MORPH_WAYPOINT_BYTES = 7480;
export const MAX_WAYPOINTS = 3;
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

/** A stored morph: its block and how many waypoints follow it (0: a straight blend). */
export interface StoredMorph extends MorphBlock {
  waypoints: number;
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

/** The waypoint count of a morph section of `length` bytes; null when it is not a block and 0-3 waypoints. */
export function sectionWaypoints(length: number): number | null {
  const k = (length - MORPH_BLOCK_SIZE) / MORPH_WAYPOINT_BYTES;
  return Number.isInteger(k) && k >= 0 && k <= MAX_WAYPOINTS ? k : null;
}

/** A slot's morph section: the block, then the waypoints' bytes (a whole number of them, at most MAX_WAYPOINTS). */
export function morphSection(block: MorphBlock, waypoints: Uint8Array = new Uint8Array(0)): Uint8Array {
  const out = new Uint8Array(MORPH_BLOCK_SIZE + waypoints.length);
  if (sectionWaypoints(out.length) === null) throw new Error(`morph: ${waypoints.length} bytes of waypoints`);
  out.set(encodeMorphBlock(block));
  out.set(waypoints, MORPH_BLOCK_SIZE);
  return out;
}

/** A copy of a morph section with its block's target set to `target` (slot 0-7). */
export function withTarget(section: Uint8Array, target: number): Uint8Array {
  const out = section.slice();
  out[0] = target;
  return out;
}

const PATH_MAGIC = "DKMP";
const PATH_VERSION = 1;
const PATH_HEADER_SIZE = 28;

/** A trained path, as the trainer's path file holds it. */
export interface MorphPath {
  /** The stored CRCs of the model binaries the path was trained between: the preset's own and its target's. */
  crcA: number;
  crcB: number;
  /** The path's fit and the straight unaligned blend's (0..1). */
  fit: number;
  straightFit: number;
  /** The waypoint count (1-3) and their bytes as a slot stores them (count * MORPH_WAYPOINT_BYTES). */
  count: number;
  waypoints: Uint8Array;
}

/** Decodes a path file; throws an Error naming what is wrong with it. */
export function decodePathFile(bytes: Uint8Array): MorphPath {
  if (bytes.length < PATH_HEADER_SIZE) throw new Error(`path file: ${bytes.length} bytes`);
  if (String.fromCharCode(...bytes.subarray(0, 4)) !== PATH_MAGIC)
    throw new Error(`path file: magic, need ${PATH_MAGIC}`);
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.length);
  const version = dv.getUint32(4, true);
  if (version !== PATH_VERSION) throw new Error(`path file: version ${version}, need ${PATH_VERSION}`);
  const count = dv.getUint32(8, true);
  if (count < 1 || count > MAX_WAYPOINTS) throw new Error(`path file: ${count} waypoints, need 1-${MAX_WAYPOINTS}`);
  const size = PATH_HEADER_SIZE + count * MORPH_WAYPOINT_BYTES;
  if (bytes.length !== size) throw new Error(`path file: size ${bytes.length}, need ${size} for ${count} waypoints`);
  const fit = dv.getFloat32(20, true);
  const straightFit = dv.getFloat32(24, true);
  for (const [what, v] of [
    ["fit", fit],
    ["straight fit", straightFit],
  ] as const)
    if (!(v >= 0 && v <= 1)) throw new Error(`path file: ${what} ${v}, need 0..1`);
  for (let at = PATH_HEADER_SIZE; at < size; at += 4)
    if (!Number.isFinite(dv.getFloat32(at, true))) throw new Error("path file: a weight is not finite");
  return {
    crcA: dv.getUint32(12, true),
    crcB: dv.getUint32(16, true),
    fit,
    straightFit,
    count,
    waypoints: bytes.slice(PATH_HEADER_SIZE),
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
