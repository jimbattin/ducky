import { expect, test } from "bun:test";
import { inputCalDb, KNOBS, levelMatchDb, positionIndex, resonanceActive } from "./controls";

const readout = (id: string, permille: number): string => KNOBS.find((k) => k.id === id)!.readout(permille / 1000);

test("knob readouts follow the firmware's tapers", () => {
  expect(readout("K1", 500)).toBe("+0.0 dB");
  expect(readout("K2", 0)).toBe("Off");
  expect(readout("K2", 250)).toBe("−18.1 dB");
  expect(readout("K2", 500)).toBe("+0.0 dB");
  expect(readout("K2", 1000)).toBe("+12.0 dB");
  expect(readout("K5", 20)).toBe("Off");
  expect(readout("K5", 500)).toBe("100 Hz");
  expect(readout("K5", 1000)).toBe("500 Hz");
  expect(readout("K4", 0)).toBe("1.50 kHz");
  expect(readout("K4", 500)).toBe("5.48 kHz");
  expect(readout("K4", 980)).toBe("Off");
  expect(readout("K3", 500)).toBe("Q 1.00");
});

test("resonance is inactive at 6 dB/oct or with its filter off", () => {
  expect(resonanceActive(0.5, 1, true)).toBe(true);
  expect(resonanceActive(0.5, 2, true)).toBe(false);
  expect(resonanceActive(0.01, 0, true)).toBe(false);
  expect(resonanceActive(0.99, 0, false)).toBe(false);
});

test("switch positions list up first", () => {
  expect(positionIndex(2)).toBe(0);
});

test("level match and input calibration", () => {
  expect(levelMatchDb(-24)).toBe(6);
  expect(levelMatchDb(null)).toBeNull();
  expect(inputCalDb(14.76)).toBeCloseTo(-10.26, 6);
  expect(inputCalDb(null)).toBe(-7.5);
});
