// Writes tests/fixtures/morph/: A2_shuffled.bin, the model of tests/fixtures/nam/A2.bin with its hidden channels
// permuted and scaled and its residual path mixed (the same network in other coordinates, head scale folded in), and
// A2_shuffled.morph, the morph block that aligns it back to A2 (targeting slot 1). The host tests
// (tests/morph_test.cpp, tests/preset_test.cpp) and the editor (editor/src/morph/align.test.ts) both check them,
// which pins the alignment transform and the block format across the two languages.
//
// Usage: bun tools/make_morph_fixture.ts

import { mkdir } from "node:fs/promises";
import { applyTransform, fitAlignment, type MorphTransform } from "../editor/src/morph/align";
import { modelWeights, withWeights } from "../editor/src/nam/convert";
import { encodeMorphBlock } from "../editor/src/preset/morph";

const fixtures = `${import.meta.dir}/../tests/fixtures`;
const SHUFFLE: MorphTransform = {
  perm: [2, 0, 1],
  scale: [0.5, 2, 1.25],
  m: [1, 0.2, 0, 0, 0.9, -0.1, 0.3, 0, 1.1],
};

const a2Bin = new Uint8Array(await Bun.file(`${fixtures}/nam/A2.bin`).arrayBuffer());
const a2 = modelWeights(a2Bin);
const shuffledBin = withWeights(a2Bin, applyTransform(SHUFFLE, a2.weights, a2.headScale), 1);
const shuffled = modelWeights(shuffledBin);
const fit = fitAlignment(a2, shuffled);
const block = encodeMorphBlock({ ...fit, target: 0, modelCrc: shuffled.crc });

await mkdir(`${fixtures}/morph`, { recursive: true });
await Bun.write(`${fixtures}/morph/A2_shuffled.bin`, shuffledBin);
await Bun.write(`${fixtures}/morph/A2_shuffled.morph`, block);
console.log(`${fixtures}/morph/A2_shuffled.bin: ${shuffledBin.length} bytes crc 0x${shuffled.crc.toString(16)}`);
console.log(
  `${fixtures}/morph/A2_shuffled.morph: ${block.length} bytes, perm ${fit.perm.join(" ")}, fit ${fit.fit}, unaligned fit ${fit.unalignedFit}`,
);
