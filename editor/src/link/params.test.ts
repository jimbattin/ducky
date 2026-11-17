import { expect, test } from "bun:test";
import { FACTORY_PARAMS } from "../testFixtures";
import {
  accepts,
  CODE_MAX,
  codeOf,
  factoryAssign,
  formatParam,
  paramValue,
  parseParamValue,
  switchLabels,
} from "./params";
import type { ParamInfo } from "./protocol";

const param = (name: string): ParamInfo => FACTORY_PARAMS.find((p) => p.name === name)!;
const ON_OFF: ParamInfo = { ...param("mid"), max: 1, unit: "off,on" };

test("formatParam follows the firmware's tapers and off ends", () => {
  expect(formatParam(param("low_cut_freq"), 0)).toBe("Off");
  expect(formatParam(param("low_cut_freq"), 8192)).toBe("100 Hz");
  expect(formatParam(param("low_cut_freq"), CODE_MAX)).toBe("500 Hz");
  expect(formatParam(param("high_cut_freq"), 0)).toBe("1.50 kHz");
  expect(formatParam(param("high_cut_freq"), 8192)).toBe("5.48 kHz");
  expect(formatParam(param("high_cut_freq"), CODE_MAX)).toBe("Off");
  expect(formatParam(param("high_cut_res"), 8192)).toBe("Q 1.00");
  expect(formatParam(param("mid"), 2)).toBe("+5 dB");
  expect(formatParam(param("low_cut_slope"), 0)).toBe("24 dB/oct");
});

test("codes and values round trip", () => {
  expect(paramValue(param("low_cut_res"), CODE_MAX)).toBe(1);
  expect(codeOf(param("low_cut_res"), 0.5)).toBe(8192);
  expect(codeOf(param("low_cut_res"), 7)).toBe(CODE_MAX);
  expect(paramValue(param("mid"), 9)).toBe(2);
  expect(codeOf(param("mid"), 1.4)).toBe(1);
});

test("switchLabels list up first; a two-value choice is off in the middle", () => {
  expect(switchLabels(param("mid"))).toEqual(["+5 dB", "off", "-7 dB"]);
  expect(switchLabels(ON_OFF)).toEqual(["on", "off", "on"]);
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
  expect(parseParamValue(param("mid"), "−7 db")).toBe(0);
  expect(parseParamValue(param("mid"), "loud")).toBeNull();
});

test("a switch accepts choices only; the factory layout is the table's", () => {
  expect(accepts(4, param("low_cut_freq"))).toBe(false);
  expect(accepts(4, param("mid"))).toBe(true);
  expect(accepts(0, param("mid"))).toBe(true);
  expect(accepts(6, null)).toBe(true);
  expect(factoryAssign(FACTORY_PARAMS)).toEqual([4, 3, 0, 1, 2, 5, 6]);
});
