import { expect, test } from "bun:test";
import { cutDb, eqCurves, eqTotalDb, midDb, readEqSettings } from "./eqModel";

const db = (x: number) => 20 * Math.log10(x);
const FREQS = [20, 50, 100, 515, 1000, 5000, 20000];

test("STATUS's knobs and toggles map to the firmware's sections", () => {
  // K1 in, K2 out, K3 high-cut Q, K4 high-cut, K5 low-cut, K6 low-cut Q; SW1 low slope, SW2 high slope, SW3 mid.
  const s = readEqSettings([0, 1000, 0, 500, 500, 1000], [2, 0, 0]);
  expect(s.lowCut?.hz).toBeCloseTo(100, 9);
  expect(s.lowCut?.q).toBeCloseTo(2, 9);
  expect(s.lowCut?.slope).toBe(6);
  expect(s.highCut?.hz).toBeCloseTo(1500 * Math.sqrt(20000 / 1500), 9);
  expect(s.highCut?.q).toBeCloseTo(0.5, 9);
  expect(s.highCut?.slope).toBe(24);
  expect(s.midDb).toBe(-7);
  expect(s.outDb).toBeCloseTo(12, 9);
  expect(readEqSettings([500, 0, 500, 500, 500, 500], [1, 1, 2]).outDb).toBe(-Infinity);
});

test("neutral controls are flat at 0 dB", () => {
  const s = readEqSettings([0, 500, 500, 1000, 0, 500], [1, 1, 1]);
  expect(s.lowCut).toBeNull();
  expect(s.highCut).toBeNull();
  for (const v of eqCurves(s, FREQS).total) expect(v).toBe(0);
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
  const s = readEqSettings([500, 250, 700, 300, 600, 400], [0, 1, 2]);
  const c = eqCurves(s, FREQS);
  FREQS.forEach((f, i) => {
    expect(c.total[i]).toBeCloseTo(c.low[i]! + c.mid[i]! + c.high[i]! + s.outDb, 9);
    expect(eqTotalDb(s, f)).toBe(c.total[i]!);
  });
  expect(s.outDb).toBeCloseTo(-18.06, 2);
});
