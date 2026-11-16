import { expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import {
  CMD_NAMES,
  fromSeptets,
  pack7,
  parseMessage,
  RESULT_NAMES,
  SysExAssembler,
  septets,
  unpack7,
  uploadChunkMsg,
} from "./protocol";

const doc = readFileSync(join(import.meta.dir, "../../../docs/MIDI.md"), "utf8");

/** The rows of the markdown table under `## heading`. */
function tableRows(heading: string): string[][] {
  const section = doc.split(`## ${heading}\n`)[1]!.split("\n## ")[0]!;
  return section
    .split("\n")
    .filter((l) => l.startsWith("|") && !l.startsWith("|---"))
    .slice(1)
    .map((l) => l.split("|").slice(1, -1));
}

test("pack7 vector", () => {
  expect([...pack7(Uint8Array.of(0x80, 0x01, 0xff, 0x7f, 0x00, 0x81, 0x42, 0xc3))]).toEqual([
    0x25, 0x00, 0x01, 0x7f, 0x7f, 0x00, 0x01, 0x42, 0x01, 0x43,
  ]);
});

test("pack7 round trip for 0-20 bytes", () => {
  for (let n = 0; n <= 20; n++) {
    const data = Uint8Array.from({ length: n }, (_, i) => (i * 37 + (i & 1) * 0x80) & 0xff);
    const packed = pack7(data);
    expect(packed.length).toBe(n + Math.ceil(n / 7));
    expect(unpack7(packed)).toEqual(data);
  }
});

test("unpack7 rejects a lone header and high bytes", () => {
  expect(unpack7(new Uint8Array(9))).toBeNull();
  expect(unpack7(Uint8Array.of(0x00, 0x80))).toBeNull();
});

test("septets", () => {
  expect(septets(0xdeadbeef, 5)).toEqual([0x0d, 0x75, 0x36, 0x7d, 0x6f]);
  expect(septets(16024, 3)).toEqual([0x00, 0x7d, 0x18]);
  expect(fromSeptets(Uint8Array.from(septets(0xdeadbeef, 5)), 0, 5)).toBe(0xdeadbeef);
});

test("RESULT parses", () => {
  const m = Uint8Array.of(0xf0, 0x7d, 0x64, 0x01, 0x14, 0x09, 0x00, 0x00, 0x04, 0x00, 0xf7);
  expect(parseMessage(m)).toEqual({ type: "result", cmd: 0x09, code: 0, value: 512 });
});

test("STATUS parses", () => {
  const m = Uint8Array.of(
    0xf0,
    0x7d,
    0x64,
    0x01,
    0x15,
    0x03,
    0x74,
    0x03,
    0x77,
    0x00,
    0x00,
    0x00,
    0x03,
    0x03,
    0x00,
    0x00,
    0x00,
    0x01,
    0x7a,
    0x03,
    0x74,
    0x05,
    0x6e,
    0x07,
    0x68,
    0x00,
    0x7b,
    0x02,
    0x01,
    0x00,
    0x02,
    0xf7,
  );
  expect(parseMessage(m)).toEqual({
    type: "status",
    loadAvg: 500,
    loadMax: 503,
    underruns: 3,
    slot: 3,
    muted: false,
    knobs: [0, 250, 500, 750, 1000, 123],
    toggles: [2, 1, 0],
    fs1: false,
    fs2: true,
  });
});

test("SLOT parses", () => {
  const names = [..."Lead"].map((c) => c.charCodeAt(0));
  const m = Uint8Array.of(0xf0, 0x7d, 0x64, 0x01, 0x13, 2, 0, 3, ...names, 0, 0x41, 0, 0x42, 0xf7);
  expect(parseMessage(m)).toEqual({
    type: "slot",
    slot: 2,
    status: 0,
    hasIr: true,
    playing: true,
    name: "Lead",
    model: "A",
    ir: "B",
  });
});

test("uploadChunkMsg frame", () => {
  const m = uploadChunkMsg(
    512,
    Uint8Array.from({ length: 256 }, (_, i) => i),
  );
  expect(m.length).toBe(307);
  expect([...m.subarray(5, 8)]).toEqual([0x00, 0x04, 0x00]);
  expect([...m.subarray(8, 13)]).toEqual([0x02, 0x48, 0x16, 0x18, 0x73]);
});

test("SysExAssembler splits and skips realtime", () => {
  const a = new SysExAssembler();
  expect(a.feed(Uint8Array.of(0xf0, 0x7d, 0xf8))).toEqual([]);
  expect(a.feed(Uint8Array.of(0x64, 0xf7, 0x00, 0xf0, 0x01, 0xf7))).toEqual([
    Uint8Array.of(0xf0, 0x7d, 0x64, 0xf7),
    Uint8Array.of(0xf0, 0x01, 0xf7),
  ]);
});

test("CMD_NAMES match docs/MIDI.md's Commands table", () => {
  const fromDoc: Record<number, string> = {};
  for (const [, command] of tableRows("Commands")) {
    const m = /^`([0-9A-F]{2})` (.+)$/.exec(command!);
    if (m !== null) fromDoc[Number.parseInt(m[1]!, 16)] = m[2]!;
  }
  expect(fromDoc).toEqual({ ...CMD_NAMES });
});

test("RESULT_NAMES match docs/MIDI.md's Results table", () => {
  expect(tableRows("Results").map(([code, name]) => [Number(code), name])).toEqual(
    RESULT_NAMES.map((name, code) => [code, name]),
  );
});
