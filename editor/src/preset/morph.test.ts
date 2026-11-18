import { expect, test } from "bun:test";
import { IDENTITY } from "../morph/align";
import { pathFile, waypointBytes } from "../testFixtures";
import { decodeMorphBlock, decodePathFile, type MorphBlock, morphSection, sectionWaypoints, withTarget } from "./morph";

test("a path file decodes to its CRCs, fits and waypoint bytes", () => {
  const waypoints = waypointBytes(2);
  const path = decodePathFile(pathFile(0xdeadbeef, 0x01020304, 0.75, 0.25, waypoints));
  expect(path).toEqual({ crcA: 0xdeadbeef, crcB: 0x01020304, fit: 0.75, straightFit: 0.25, count: 2, waypoints });
});

test("a path file is decoded in place from a view into a larger buffer", () => {
  const file = pathFile(1, 2, 0.5, 0.5, waypointBytes(1));
  const padded = new Uint8Array(file.length + 6);
  padded.set(file, 3);
  expect(decodePathFile(padded.subarray(3, 3 + file.length))).toEqual(decodePathFile(file));
});

test("a path file with a bad magic, version, count, length, fit or weight is rejected", () => {
  const good = pathFile(1, 2, 0.5, 0.5, waypointBytes(3));
  const edit = (f: (dv: DataView) => void): Uint8Array => {
    const out = good.slice();
    f(new DataView(out.buffer));
    return out;
  };
  expect(() => decodePathFile(good.subarray(0, 20))).toThrow("path file: 20 bytes");
  expect(() => decodePathFile(edit((dv) => dv.setUint8(3, 0x53)))).toThrow("path file: magic");
  expect(() => decodePathFile(edit((dv) => dv.setUint32(4, 2, true)))).toThrow("path file: version 2");
  expect(() => decodePathFile(edit((dv) => dv.setUint32(8, 0, true)))).toThrow("path file: 0 waypoints");
  expect(() => decodePathFile(edit((dv) => dv.setUint32(8, 4, true)))).toThrow("path file: 4 waypoints");
  // The count says 2, the file holds 3; and a file cut short.
  expect(() => decodePathFile(edit((dv) => dv.setUint32(8, 2, true)))).toThrow("need 14988 for 2 waypoints");
  expect(() => decodePathFile(good.subarray(0, good.length - 1))).toThrow("path file: size");
  expect(() => decodePathFile(edit((dv) => dv.setFloat32(20, 1.5, true)))).toThrow("path file: fit");
  expect(() => decodePathFile(edit((dv) => dv.setFloat32(24, Number.NaN, true)))).toThrow("path file: straight fit");
  expect(() => decodePathFile(edit((dv) => dv.setFloat32(good.length - 4, Number.POSITIVE_INFINITY, true)))).toThrow(
    "not finite",
  );
});

const block: MorphBlock = { ...IDENTITY, fit: 0.75, unalignedFit: 0.25, target: 5, modelCrc: 0x89abcdef };

test("a morph section is the block followed by the waypoints", () => {
  const waypoints = waypointBytes(3);
  const section = morphSection(block, waypoints);
  expect(section.length).toBe(64 + 3 * 7480);
  expect(decodeMorphBlock(section, 0)).toEqual(block);
  expect(section.subarray(64)).toEqual(waypoints);
  expect(morphSection(block).length).toBe(64);
  expect(() => morphSection(block, waypoints.subarray(1))).toThrow("morph:");
  expect(() => morphSection(block, waypointBytes(4))).toThrow("morph:");
});

test("a section's length gives its waypoint count", () => {
  expect([64, 64 + 7480, 64 + 3 * 7480].map(sectionWaypoints)).toEqual([0, 1, 3]);
  expect([0, 60, 64 + 7476, 64 + 4 * 7480].map(sectionWaypoints)).toEqual([null, null, null, null]);
});

test("withTarget changes the target alone and leaves the section it was given", () => {
  const section = morphSection(block, waypointBytes(1));
  const moved = withTarget(section, 2);
  expect(decodeMorphBlock(moved, 0)).toEqual({ ...block, target: 2 });
  expect(moved.subarray(1)).toEqual(section.subarray(1));
  expect(section[0]).toBe(5);
});
