// Writes tests/fixtures/morph/:
//   A2_shuffled.bin    the model of tests/fixtures/nam/A2.bin with its hidden channels permuted and scaled and its
//                      residual path mixed (the same network in other coordinates, head scale folded in)
//   A2_shuffled.morph  the morph block that aligns it back to A2 (targeting slot 1)
//   A2_path.preset     a preset of A2 whose morph (targeting slot 2, with that block's transform) has three
//                      waypoints: A2 in three other sets of coordinates. Each node of its path is A2's network, so
//                      the morph renders A2 at every knot and something else between them. The fits it stores are
//                      the block's, not this path's
// The host tests (tests/morph_test.cpp, tests/preset_test.cpp) and the editor (editor/src/morph/align.test.ts) check
// them, which pins the alignment transform, the block and the waypoints' layout across the two languages.
//
// Usage: bun tools/make_morph_fixture.ts

import { mkdir } from "node:fs/promises";
import { applyTransform, fitAlignment, type MorphTransform } from "../editor/src/morph/align";
import { modelWeights, withWeights } from "../editor/src/nam/convert";
import { buildSlot } from "../editor/src/preset/bank";
import { GATE_DEFAULT } from "../editor/src/preset/gate";
import { encodeMorphBlock, morphSection } from "../editor/src/preset/morph";

const fixtures = `${import.meta.dir}/../tests/fixtures`;
const SHUFFLE: MorphTransform = {
  perm: [2, 0, 1],
  scale: [0.5, 2, 1.25],
  m: [1, 0.2, 0, 0, 0.9, -0.1, 0.3, 0, 1.1],
};
/** The path's three waypoints: A2 through each of these. */
const WAYPOINTS: MorphTransform[] = [
  { perm: [1, 0, 2], scale: [1.5, 0.75, 1], m: [1.2, 0, 0.1, 0, 1, 0, -0.2, 0, 0.9] },
  { perm: [0, 2, 1], scale: [0.8, 1, 1.6], m: [0.9, 0.1, 0, -0.1, 1.1, 0, 0, 0.2, 1] },
  { perm: [2, 1, 0], scale: [1, 1.3, 0.7], m: [1, 0, 0, 0.15, 0.8, 0, 0, -0.1, 1.25] },
];

const a2Bin = new Uint8Array(await Bun.file(`${fixtures}/nam/A2.bin`).arrayBuffer());
const a2 = modelWeights(a2Bin);
const shuffledBin = withWeights(a2Bin, applyTransform(SHUFFLE, a2.weights, a2.headScale), 1);
const shuffled = modelWeights(shuffledBin);
const fit = fitAlignment(a2, shuffled);
const block = encodeMorphBlock({ ...fit, target: 0, modelCrc: shuffled.crc });

const waypoints = new Float32Array(WAYPOINTS.flatMap((t) => [...applyTransform(t, a2.weights, a2.headScale)]));
const path = buildSlot(
  a2Bin,
  null,
  "A2 path",
  GATE_DEFAULT,
  morphSection({ ...fit, target: 1, modelCrc: shuffled.crc }, new Uint8Array(waypoints.buffer)),
);

await mkdir(`${fixtures}/morph`, { recursive: true });
await Bun.write(`${fixtures}/morph/A2_shuffled.bin`, shuffledBin);
await Bun.write(`${fixtures}/morph/A2_shuffled.morph`, block);
await Bun.write(`${fixtures}/morph/A2_path.preset`, path);
console.log(`${fixtures}/morph/A2_shuffled.bin: ${shuffledBin.length} bytes crc 0x${shuffled.crc.toString(16)}`);
console.log(
  `${fixtures}/morph/A2_shuffled.morph: ${block.length} bytes, perm ${fit.perm.join(" ")}, fit ${fit.fit}, unaligned fit ${fit.unalignedFit}`,
);
console.log(`${fixtures}/morph/A2_path.preset: ${path.length} bytes, ${WAYPOINTS.length} waypoints`);
