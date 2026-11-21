import { expect, test } from "bun:test";
import { modelWeights } from "../nam/convert";
import { readFixture } from "../testFixtures";
import { applyTransform, fitAlignment, IDENTITY } from "./align";

test("fitAlignment recovers a model whose channels were permuted, scaled and mixed", async () => {
  const a2 = modelWeights(await readFixture("nam/A2.bin"));
  const shuffled = modelWeights(await readFixture("morph/A2_shuffled.bin"));
  const fit = fitAlignment(a2, shuffled);
  expect(fit.fit).toBeGreaterThanOrEqual(0.999999);
  expect(fit.unalignedFit).toBeLessThan(0.5);

  const want = applyTransform(IDENTITY, a2.weights, a2.headScale);
  const got = applyTransform(fit, shuffled.weights, 1);
  const peak = want.reduce((m, v) => Math.max(m, Math.abs(v)), 0);
  const worst = want.reduce((m, v, i) => Math.max(m, Math.abs(got[i]! - v)), 0);
  expect(worst).toBeLessThanOrEqual(1e-5 * peak);
});

// The development models are git-ignored; the test skips when they are absent.
const MODELS = `${import.meta.dir}/../../../models`;
const fender = Bun.file(`${MODELS}/fender_super_eob_with_cab.bin`);
const sss = Bun.file(`${MODELS}/sss_with_cab_clean.bin`);

test.skipIf(!((await fender.exists()) && (await sss.exists())))(
  "captures of two amps keep the identity: aligning their uncorrelated channels would scramble the target",
  async () => {
    const a = modelWeights(new Uint8Array(await fender.arrayBuffer()));
    const b = modelWeights(new Uint8Array(await sss.arrayBuffer()));
    const fit = fitAlignment(a, b);
    expect(fit).toMatchObject(IDENTITY);
    expect(fit.fit).toBe(fit.unalignedFit);
    expect(fit.fit).toBeGreaterThan(0.4);
  },
);
