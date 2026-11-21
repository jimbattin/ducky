import { expect, test } from "bun:test";
import { readWav } from "../ir/wav";
import { modelWeights } from "../nam/convert";
import { readFixture } from "../testFixtures";
import { A2Net } from "./a2";

test("A2Net renders A2.bin on input.wav as NeuralAmpModelerCore does", async () => {
  const model = modelWeights(await readFixture("nam/A2.bin"));
  const input = readWav(await readFixture("nam/input.wav")).samples;
  const ref = readWav(await readFixture("nam/A2.input.ref.wav")).samples;
  expect(ref.length).toBe(input.length);
  const net = new A2Net(model.weights, model.headScale);
  let worst = 0;
  for (let i = 0; i < input.length; i++) worst = Math.max(worst, Math.abs(net.step(input[i]!) - ref[i]!));
  expect(worst).toBeLessThanOrEqual(1e-4);
});
