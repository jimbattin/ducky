import { expect, test } from "bun:test";
import { PARAMS } from "../testFixtures";
import {
  accepts,
  CODE_MAX,
  codeOf,
  codeOfUnits,
  formatParam,
  paramValue,
  parseParamValue,
  switchLabels,
  unitsOf,
} from "./params";
import type { ParamInfo } from "./protocol";

const param = (name: string): ParamInfo => PARAMS.find((p) => p.name === name)!;

test("formatParam follows the firmware's tapers and off ends", () => {
  expect(formatParam(param("low_cut_freq"), 0)).toBe("Off");
  expect(formatParam(param("low_cut_freq"), 8192)).toBe("100 Hz");
  expect(formatParam(param("low_cut_freq"), CODE_MAX)).toBe("500 Hz");
  expect(formatParam(param("high_cut_freq"), 0)).toBe("1.50 kHz");
  expect(formatParam(param("high_cut_freq"), 8192)).toBe("5.48 kHz");
  expect(formatParam(param("high_cut_freq"), CODE_MAX)).toBe("Off");
  expect(formatParam(param("high_cut_res"), 8192)).toBe("Q 1.00");
  expect(formatParam(param("bell1_q"), param("bell1_q").defCode)).toBe("Q 1.00");
  expect(formatParam(param("bell1_gain"), codeOfUnits(param("bell1_gain"), 3))).toBe("+3.0 dB");
  expect(formatParam(param("bell1_gain"), 0)).toBe("−12.0 dB");
  expect(formatParam(param("bell1_gain"), 8192)).toBe("+0.0 dB");
  expect(formatParam(param("low_cut_slope"), 0)).toBe("24 dB/oct");
  expect(formatParam(param("eq_enable"), 1)).toBe("on");
});

test("codes and values round trip", () => {
  expect(paramValue(param("low_cut_res"), CODE_MAX)).toBe(1);
  expect(codeOf(param("low_cut_res"), 0.5)).toBe(8192);
  expect(codeOf(param("low_cut_res"), 7)).toBe(CODE_MAX);
  expect(paramValue(param("low_cut_slope"), 9)).toBe(2);
  expect(codeOf(param("low_cut_slope"), 1.4)).toBe(1);
});

test("codeOfUnits inverts unitsOf on log and linear tapers, clamped", () => {
  for (const name of ["bell2_freq", "bell3_q", "bell1_gain", "high_cut_freq"]) {
    const p = param(name);
    for (const code of [0, 1, 5011, 8192, 12000, CODE_MAX])
      expect(codeOfUnits(p, unitsOf(p, code / CODE_MAX))).toBe(code);
    expect(codeOfUnits(p, p.min - 1)).toBe(0);
    expect(codeOfUnits(p, p.max * 2)).toBe(CODE_MAX);
  }
  expect(unitsOf(param("bell1_gain"), 0.5)).toBe(0);
  expect(codeOfUnits(param("bell2_freq"), 1000)).toBe(8192);
});

test("switchLabels list up first; a two-value choice is off in the middle", () => {
  expect(switchLabels(param("low_cut_slope"))).toEqual(["6 dB/oct", "12 dB/oct", "24 dB/oct"]);
  expect(switchLabels(param("eq_enable"))).toEqual(["on", "off", "on"]);
});

test("parseParamValue takes units, labels, indexes and off", () => {
  const high = param("high_cut_freq");
  const code = parseParamValue(high, "5000")!;
  expect(formatParam(high, code)).toBe("5.00 kHz");
  expect(code).toBe(Math.round((Math.log(5000 / 1500) / Math.log(20000 / 1500)) * CODE_MAX));
  expect(parseParamValue(high, "off")).toBe(CODE_MAX);
  expect(parseParamValue(param("low_cut_freq"), "OFF")).toBe(0);
  expect(parseParamValue(param("low_cut_res"), "off")).toBeNull();
  expect(parseParamValue(param("low_cut_freq"), "10")).toBeNull();
  expect(parseParamValue(param("low_cut_freq"), "")).toBeNull();
  expect(parseParamValue(param("low_cut_slope"), "6dB/oct")).toBe(2);
  expect(parseParamValue(param("low_cut_slope"), "1")).toBe(1);
  expect(parseParamValue(param("low_cut_slope"), "7")).toBeNull();
  expect(parseParamValue(param("eq_enable"), "Off")).toBe(0);
  expect(parseParamValue(param("bell2_gain"), "-12")).toBe(0);
  expect(parseParamValue(param("bell2_gain"), "−6")).toBe(codeOfUnits(param("bell2_gain"), -6));
  expect(parseParamValue(param("bell2_gain"), "13")).toBeNull();
  expect(parseParamValue(param("bell1_q"), "0.4")).toBe(0);
});

test("a switch accepts choices only", () => {
  expect(accepts(4, param("low_cut_freq"))).toBe(false);
  expect(accepts(4, param("bell1_gain"))).toBe(false);
  expect(accepts(6, param("eq_enable"))).toBe(true);
  expect(accepts(0, param("low_cut_slope"))).toBe(true);
  expect(accepts(6, null)).toBe(true);
});
