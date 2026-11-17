import { expect, test } from "bun:test";
import { FIXED_KNOBS, inputCalDb, levelMatchDb } from "./controls";

const readout = (id: string, permille: number): string =>
  FIXED_KNOBS.find((k) => k.id === id)!.readout(permille / 1000);

test("knob readouts follow the firmware's tapers", () => {
  expect(readout("K1", 500)).toBe("+0.0 dB");
  expect(readout("K2", 0)).toBe("Off");
  expect(readout("K2", 250)).toBe("−18.1 dB");
  expect(readout("K2", 500)).toBe("+0.0 dB");
  expect(readout("K2", 1000)).toBe("+12.0 dB");
});

test("level match and input calibration", () => {
  expect(levelMatchDb(-24)).toBe(6);
  expect(levelMatchDb(null)).toBeNull();
  expect(inputCalDb(14.76)).toBeCloseTo(-10.26, 6);
  expect(inputCalDb(null)).toBe(-7.5);
});
