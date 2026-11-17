import { expect, test } from "bun:test";
import { cutDb, eqCurves, eqTotalDb, midDb, readEqSettings, resonanceInactive } from "./eqModel";
import { codeOf } from "./link/params";
import { FACTORY_PARAMS } from "./testFixtures";

const db = (x: number) => 20 * Math.log10(x);
const FREQS = [20, 50, 100, 515, 1000, 5000, 20000];

/** FACTORY_PARAMS' codes from each parameter's value by name: a position 0..1, or a choice's index. */
const codes = (values: Record<string, number>): number[] => FACTORY_PARAMS.map((p) => codeOf(p, values[p.name]!));
const NEUTRAL = {
  low_cut_freq: 0,
  low_cut_res: 0.5,
  low_cut_slope: 1,
  high_cut_freq: 1,
  high_cut_res: 0.5,
  high_cut_slope: 1,
  mid: 1,
};

test("the parameters' codes map to the firmware's sections", () => {
  const s = readEqSettings(
    FACTORY_PARAMS,
    codes({
      low_cut_freq: 0.5,
      low_cut_res: 1,
      low_cut_slope: 2,
      high_cut_freq: 0.5,
      high_cut_res: 0,
      high_cut_slope: 0,
      mid: 0,
    }),
    1,
  )!;
  // Noon is code 8192 of 16383: within 0.01 % of the frequency at exactly half the travel.
  expect(s.lowCut!.hz / 100).toBeCloseTo(1, 3);
  expect(s.lowCut?.q).toBeCloseTo(2, 9);
  expect(s.lowCut?.slope).toBe(6);
  expect(s.highCut!.hz / (1500 * Math.sqrt(20000 / 1500))).toBeCloseTo(1, 3);
  expect(s.highCut?.q).toBeCloseTo(0.5, 9);
  expect(s.highCut?.slope).toBe(24);
  expect(s.midDb).toBe(-7);
  expect(s.outDb).toBeCloseTo(12, 9);
  expect(readEqSettings(FACTORY_PARAMS, codes(NEUTRAL), 0)?.outDb).toBe(-Infinity);
});

test("a table without an EQ parameter gives no settings", () => {
  expect(readEqSettings(FACTORY_PARAMS.slice(0, 6), codes(NEUTRAL), 0.5)).toBeNull();
  expect(readEqSettings(FACTORY_PARAMS, codes(NEUTRAL).slice(0, 6), 0.5)).toBeNull();
});

test("neutral controls are flat at 0 dB", () => {
  const s = readEqSettings(FACTORY_PARAMS, codes(NEUTRAL), 0.5)!;
  expect(s.lowCut).toBeNull();
  expect(s.highCut).toBeNull();
  for (const v of eqCurves(s, FREQS).total) expect(v).toBe(0);
});

test("resonance is inactive at 6 dB/oct or with its filter off", () => {
  const on = { ...NEUTRAL, low_cut_freq: 0.5, high_cut_freq: 0.5 };
  const inactive = (values: Record<string, number>, name: string): boolean =>
    resonanceInactive(FACTORY_PARAMS, codes(values), name);
  expect(inactive(on, "low_cut_res")).toBe(false);
  expect(inactive({ ...on, low_cut_slope: 2 }, "low_cut_res")).toBe(true);
  expect(inactive({ ...on, low_cut_slope: 2 }, "high_cut_res")).toBe(false);
  expect(inactive({ ...on, low_cut_freq: 0.01, low_cut_slope: 0 }, "low_cut_res")).toBe(true);
  expect(inactive({ ...on, high_cut_freq: 0.99 }, "high_cut_res")).toBe(true);
  expect(inactive({ ...on, low_cut_slope: 2 }, "low_cut_freq")).toBe(false);
});

test("cut filters at their cutoff: -3 dB one-pole, Q for 12 dB, Q - 3 dB for 24 dB", () => {
  for (const hp of [true, false]) {
    const hz = hp ? 120 : 6000;
    for (const q of [0.5, Math.SQRT1_2, 1, 2]) {
      expect(cutDb({ hz, q, slope: 6 }, hp, hz)).toBeCloseTo(db(Math.SQRT1_2), 9);
      expect(cutDb({ hz, q, slope: 12 }, hp, hz)).toBeCloseTo(db(q), 9);
      expect(cutDb({ hz, q, slope: 24 }, hp, hz)).toBeCloseTo(db(q * Math.SQRT1_2), 9);
    }
  }
});

test("cut filters fall at their slope four octaves out and pass far in", () => {
  for (const slope of [6, 12, 24] as const) {
    const c = { hz: 400, q: Math.SQRT1_2, slope };
    expect(Math.abs(cutDb(c, true, 25) + 4 * slope)).toBeLessThan(0.5);
    expect(Math.abs(cutDb(c, true, 15000))).toBeLessThan(0.05);
    const h = { hz: 1500, q: Math.SQRT1_2, slope };
    expect(cutDb(h, false, 1500 / 16)).toBeGreaterThan(-0.05);
    // The bilinear zero at 24 kHz makes the low-pass fall faster than its slope near Nyquist.
    expect(cutDb(h, false, 12000)).toBeLessThan(-3 * slope);
  }
});

test("the mid bell reaches its gain at 515 Hz and is 0 dB bypassed", () => {
  expect(midDb(5, 515)).toBeCloseTo(5, 9);
  expect(midDb(-7, 515)).toBeCloseTo(-7, 9);
  expect(Math.abs(midDb(5, 20))).toBeLessThan(0.1);
  for (const f of FREQS) expect(midDb(0, f)).toBe(0);
});

test("the whole chain is the sections' sum plus the output volume", () => {
  const s = readEqSettings(
    FACTORY_PARAMS,
    codes({
      low_cut_freq: 0.6,
      low_cut_res: 0.4,
      low_cut_slope: 0,
      high_cut_freq: 0.3,
      high_cut_res: 0.7,
      high_cut_slope: 1,
      mid: 2,
    }),
    0.25,
  )!;
  const c = eqCurves(s, FREQS);
  FREQS.forEach((f, i) => {
    expect(c.total[i]).toBeCloseTo(c.low[i]! + c.mid[i]! + c.high[i]! + s.outDb, 9);
    expect(eqTotalDb(s, f)).toBe(c.total[i]!);
  });
  expect(s.outDb).toBeCloseTo(-18.06, 2);
});
