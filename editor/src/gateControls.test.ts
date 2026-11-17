import { expect, test } from "bun:test";
import { GATE_PARAMS, gateStep } from "./gateControls";
import { GATE_DEFAULT, gateProblem } from "./preset/gate";

test("an arrow step moves every setting by its resolution, even at a log taper's low end, and stays in range", () => {
  for (const p of GATE_PARAMS) {
    const up = gateStep(p, p.min, 1);
    expect(up).toBeCloseTo(p.min + p.step, 9);
    expect(gateProblem({ ...GATE_DEFAULT, [p.key]: up })).toBeNull();
    expect(gateStep(p, up, -1)).toBe(p.min);
    expect(gateStep(p, p.min, -1)).toBe(p.min);
    expect(gateStep(p, p.max, 1)).toBe(p.max);
  }
});
