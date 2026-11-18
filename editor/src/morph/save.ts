// Setting a stored preset's morph target. A straight blend (saveMorph): reads both models back from the pedal, fits
// the alignment (align.ts) and writes the morph block (MORPH), or removes the morph. A trained path (saveMorphPath):
// checks the path against the two stored models and uploads the preset again with the path's block and waypoints.
// Shared by the editor and tools/pedal.ts.

import type { PedalSession } from "../link/session";
import { MODEL_BIN_SIZE, modelWeights, OFF_CRC } from "../nam/convert";
import { buildSlot, PRESET_HEADER_SIZE, splitSlot } from "../preset/bank";
import { encodeMorphBlock, type MorphPath, morphSection } from "../preset/morph";
import { fitAlignment, IDENTITY } from "./align";

export interface MorphSaved {
  flashMs: number;
  /** The slot (0-7) the preset now morphs with; null: none. */
  target: number | null;
  /** With a target: how closely the blends morph between the two models, and unaligned (align.ts MorphFit). */
  fit?: number;
  unalignedFit?: number;
}

/** saveMorphPath's result: the path's fit, the straight unaligned blend's, and the waypoint count. */
export interface MorphPathSaved extends MorphSaved {
  target: number;
  fit: number;
  unalignedFit: number;
  waypoints: number;
}

/** Makes slot's preset morph with target's (both ok slots, 0-7) by a straight blend, or with nothing. */
export async function saveMorph(session: PedalSession, slot: number, target: number | "none"): Promise<MorphSaved> {
  if (target === "none") return { flashMs: await session.morph(slot, null), target: null };
  if (target === slot) throw new Error(`Slot ${slot + 1}: a preset cannot morph with itself`);
  const a = modelWeights(await session.readModel(slot));
  const b = modelWeights(await session.readModel(target));
  const fit = fitAlignment(a, b);
  const flashMs = await session.morph(slot, encodeMorphBlock({ ...fit, target, modelCrc: b.crc }));
  return { flashMs, target, fit: fit.fit, unalignedFit: fit.unalignedFit };
}

/**
 * Makes slot's preset morph with target's (both ok slots, 0-7) along a trained path. The path must have been trained
 * from the model now in slot to the one now in target (their stored CRCs). The slot's preset is read back and
 * uploaded again with the path's block (the identity transform: the waypoints and the target's model are already in
 * the slot's model's coordinates) and its waypoints, so a playing preset reloads.
 */
export async function saveMorphPath(
  session: PedalSession,
  slot: number,
  target: number,
  path: MorphPath,
  onProgress?: (fraction: number) => void,
): Promise<MorphPathSaved> {
  if (target === slot) throw new Error(`Slot ${slot + 1}: a preset cannot morph with itself`);
  const parts = splitSlot(await session.readImage(slot));
  const crcOf = (crcField: Uint8Array): number =>
    new DataView(crcField.buffer, crcField.byteOffset, 4).getUint32(0, true);
  const differs = (k: number): Error => new Error(`Slot ${k + 1}: its model is not the one the path was trained for`);
  if (crcOf(parts.model.subarray(OFF_CRC)) !== path.crcA) throw differs(slot);
  if (crcOf(await session.read(target, PRESET_HEADER_SIZE + OFF_CRC, MODEL_BIN_SIZE - OFF_CRC)) !== path.crcB)
    throw differs(target);
  const block = { ...IDENTITY, fit: path.fit, unalignedFit: path.straightFit, target, modelCrc: path.crcB };
  const image = buildSlot(parts.model, parts.ir, parts.name, parts.gate, morphSection(block, path.waypoints));
  const { flashMs } = await session.upload(slot, image, onProgress && ((sent, size) => onProgress(sent / size)));
  return { flashMs, target, fit: path.fit, unalignedFit: path.straightFit, waypoints: path.count };
}
