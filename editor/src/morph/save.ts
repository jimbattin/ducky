// Setting a stored preset's morph target: reads both models back from the pedal, fits the alignment (align.ts) and
// writes the morph block (MORPH), or removes it. Shared by the editor and tools/pedal.ts.

import type { PedalSession } from "../link/session";
import { modelWeights } from "../nam/convert";
import { encodeMorphBlock } from "../preset/morph";
import { fitAlignment } from "./align";

export interface MorphSaved {
  flashMs: number;
  /** The slot (0-7) the preset now morphs with; null: none. */
  target: number | null;
  /** With a target: how closely the blends morph between the two models, and unaligned (align.ts MorphFit). */
  fit?: number;
  unalignedFit?: number;
}

/** Makes slot's preset morph with target's (both ok slots, 0-7), or with nothing. */
export async function saveMorph(session: PedalSession, slot: number, target: number | "none"): Promise<MorphSaved> {
  if (target === "none") return { flashMs: await session.morph(slot, null), target: null };
  if (target === slot) throw new Error(`Slot ${slot + 1}: a preset cannot morph with itself`);
  const a = modelWeights(await session.readModel(slot));
  const b = modelWeights(await session.readModel(target));
  const fit = fitAlignment(a, b);
  const flashMs = await session.morph(slot, encodeMorphBlock({ ...fit, target, modelCrc: b.crc }));
  return { flashMs, target, fit: fit.fit, unalignedFit: fit.unalignedFit };
}
