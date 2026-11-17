import { expect, test } from "bun:test";
import {
  BANDS,
  bellDb,
  cutDb,
  type EqSettings,
  eqCurves,
  eqTotalDb,
  readEqSettings,
  resonanceInactive,
} from "./eqModel";
import { codeOf, codeOfUnits } from "./link/params";
import { PARAMS } from "./testFixtures";

const db = (x: number) => 20 * Math.log10(x);
const FREQS = [20, 50, 100, 515, 1000, 5000, 20000];

/** PARAMS' codes: each parameter's default, then the given positions 0..1 or choice indexes by name. */
const codes = (values: Record<string, number> = {}): number[] =>
  PARAMS.map((p) => (p.name in values ? codeOf(p, values[p.name]!) : p.defCode));
const param = (name: string) => PARAMS.find((p) => p.name === name)!;

/** Setting A (docs/EQ.md section 3; tests/eq_test.cpp SettingA). */
const SETTING_A: EqSettings = {
  enabled: true,
  low: { hz: 80, q: Math.SQRT1_2, slope: 12 },
  bells: [
    { gain: -4, hz: 250, q: 1.4 },
    { gain: 3, hz: 1200, q: 0.7 },
    { gain: 5, hz: 3500, q: 2 },
  ],
  high: { hz: 8000, q: Math.SQRT1_2, slope: 12 },
  outDb: 0,
};

test("the model's laws: a bell reaches its gain at f0, cut mirrors boost, a 12 dB cut is -3.01 dB at its cutoff", () => {
  for (const gain of [-12, -6, 6, 12])
    for (const hz of [40, 1000, 12000]) {
      expect(bellDb({ gain, hz, q: 1.4 }, hz)).toBeCloseTo(gain, 9);
      for (const f of [20, hz, 10000])
        expect(bellDb({ gain, hz, q: 1.4 }, f) + bellDb({ gain: -gain, hz, q: 1.4 }, f)).toBeCloseTo(0, 9);
    }
  for (const hp of [true, false])
    expect(cutDb({ hz: 1000, q: Math.SQRT1_2, slope: 12 }, hp, 1000)).toBeCloseTo(-3.0103, 4);
});

test("setting A matches docs/EQ.md's table", () => {
  const f = [30, 50, 80, 120, 250, 500, 1000, 1200, 2000, 3500, 5000, 8000, 12000, 16000];
  const a = [-17.15, -8.86, -3.23, -1.39, -3.77, 0.3, 2.79, 3.08, 2.62, 5.57, 1.35, -2.61, -9.88, -19.1];
  f.forEach((hz, i) => {
    expect(Math.abs(eqTotalDb(SETTING_A, hz) - a[i]!)).toBeLessThanOrEqual(0.0051);
  });
});

test("the parameters' codes map to the firmware's sections", () => {
  const s = readEqSettings(
    PARAMS,
    codes({
      low_cut_freq: 0.5,
      low_cut_res: 1,
      low_cut_slope: 2,
      high_cut_freq: 0.5,
      high_cut_res: 0,
      high_cut_slope: 0,
    }),
    1,
  )!;
  // Noon is code 8192 of 16383: within 0.01 % of the frequency at exactly half the travel.
  expect(s.low!.hz / 100).toBeCloseTo(1, 3);
  expect(s.low?.q).toBeCloseTo(2, 9);
  expect(s.low?.slope).toBe(6);
  expect(s.high!.hz / (1500 * Math.sqrt(20000 / 1500))).toBeCloseTo(1, 3);
  expect(s.high?.q).toBeCloseTo(0.5, 9);
  expect(s.high?.slope).toBe(24);
  expect(s.enabled).toBe(true);
  expect(s.outDb).toBeCloseTo(12, 9);
  // The defaults: 0 dB (code 8192 reads as exactly 0), 250 Hz, 1 kHz and 3 kHz at Q 1.
  expect(s.bells.map((b) => b.gain)).toEqual([0, 0, 0]);
  s.bells.forEach((b, i) => {
    expect(b.hz / [250, 1000, 3000][i]!).toBeCloseTo(1, 3);
    expect(b.q).toBeCloseTo(1, 3);
  });
  const boostedCodes = codes();
  boostedCodes[param("bell2_gain").id] = codeOfUnits(param("bell2_gain"), 9);
  const boosted = readEqSettings(PARAMS, boostedCodes, 0.5)!;
  expect(boosted.bells[1].gain).toBeCloseTo(9, 2);
  expect(readEqSettings(PARAMS, codes(), 0)?.outDb).toBe(-Infinity);
});

test("readEqSettings needs every EQ parameter", () => {
  for (let i = 0; i < PARAMS.length; ++i) {
    if (PARAMS[i]!.group !== "EQ") continue;
    expect(
      readEqSettings(
        PARAMS.filter((_, j) => j !== i),
        codes(),
        0.5,
      ),
    ).toBeNull();
    expect(readEqSettings(PARAMS, codes().slice(0, i), 0.5)).toBeNull();
  }
});

test("the defaults are flat at 0 dB; disabled, the EQ is flat at the output volume", () => {
  const s = readEqSettings(PARAMS, codes(), 0.5)!;
  expect(s.low).toBeNull();
  expect(s.high).toBeNull();
  for (const v of eqCurves(s, FREQS).total) expect(v).toBe(0);
  const off = { ...SETTING_A, enabled: false, outDb: -6 };
  const c = eqCurves(off, FREQS);
  for (const b of BANDS) expect(c[b.id]).toEqual(FREQS.map(() => 0));
  expect(c.total).toEqual(FREQS.map(() => -6));
  expect(readEqSettings(PARAMS, codes({ eq_enable: 0 }), 0.5)?.enabled).toBe(false);
});

test("resonance is inactive at 6 dB/oct or with its filter off", () => {
  const on = { low_cut_freq: 0.5, high_cut_freq: 0.5 };
  const inactive = (values: Record<string, number>, name: string): boolean =>
    resonanceInactive(PARAMS, codes(values), name);
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

test("the whole chain is the bands' sum plus the output volume", () => {
  const s = { ...SETTING_A, outDb: -18.06 };
  const c = eqCurves(s, FREQS);
  FREQS.forEach((f, i) => {
    expect(c.total[i]).toBeCloseTo(
      BANDS.reduce((sum, b) => sum + c[b.id][i]!, s.outDb),
      9,
    );
    expect(eqTotalDb(s, f)).toBe(c.total[i]!);
  });
});
