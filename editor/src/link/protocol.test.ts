import { expect, test } from "bun:test";
import { readFileSync } from "node:fs";
import { join } from "node:path";
import { crc32 } from "../crc32";
import { PARAMS } from "../testFixtures";
import { choiceLabels } from "./params";
import {
  assignMsg,
  CMD,
  CMD_NAMES,
  expChannelMsg,
  fromSeptets,
  fwBeginMsg,
  gateMsg,
  midiInChannelMsg,
  morphMsg,
  pack7,
  parseMessage,
  pedalMsg,
  RESULT_NAMES,
  readMsg,
  SysExAssembler,
  septets,
  setMsg,
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
    0x04,
    0x78,
    0x01,
    0x75,
    0x00,
    0x25,
    0x05,
    0x00,
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
    gateLevelDb: -63.2,
    gateCutDb: 24.5,
    compCutDb: 3.7,
    pedal: 640,
  });
});

test("Identity Reply parses the morph from its flags byte", () => {
  const build = [..."abc1234"].map((c) => c.charCodeAt(0));
  const reply = (flags: number) =>
    Uint8Array.of(0xf0, 0x7e, 0x7f, 0x06, 0x02, 0x7d, 0x64, 0x00, 1, 0x00, flags, 0, 0, 0, ...build, 0xf7);
  expect(parseMessage(reply(0))).toEqual({
    type: "identity",
    device: 0x64,
    board: 1,
    morph: "crossfade",
    build: "abc1234",
  });
  expect(parseMessage(reply(0x01))).toMatchObject({ morph: "weight", build: "abc1234" });
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

test("fwBeginMsg frame", () => {
  expect([...fwBeginMsg(1040, 0x12345678)]).toEqual([
    0xf0, 0x7d, 0x64, 0x01, 0x1b, 0x00, 0x08, 0x10, 0x01, 0x11, 0x51, 0x2c, 0x78, 0xf7,
  ]);
});

test("readMsg frame", () => {
  expect([...readMsg(2, 300)]).toEqual([0xf0, 0x7d, 0x64, 0x01, 0x0b, 0x02, 0x00, 0x02, 0x2c, 0xf7]);
});

test("gateMsg frame", () => {
  const g = { enabled: true, thresholdDb: -55, attackMs: 1.5, holdMs: 30, releaseMs: 250, ratio: 6 };
  expect([...gateMsg(2, g)]).toEqual([
    0xf0, 0x7d, 0x64, 0x01, 0x0c, 0x02, 0x01, 0x37, 0x0f, 0x1e, 0x01, 0x7a, 0x3c, 0xf7,
  ]);
});

test("morphMsg frames", () => {
  expect([...morphMsg(4, null)]).toEqual([0xf0, 0x7d, 0x64, 0x01, 0x19, 0x04, 0xf7]);
  const block = Uint8Array.from({ length: 64 }, (_, i) => 0xc0 + i);
  const set = morphMsg(4, block);
  expect(set.length).toBe(5 + 75 + 1);
  expect(set[5]).toBe(4);
  expect(unpack7(set.subarray(6, -1))).toEqual(block);
});

test("DATA parses only with its CRC", () => {
  // tests/link_test.cpp Data.Bytes: CRC DEADBEEF does not match the data.
  const bad = Uint8Array.of(
    ...[0xf0, 0x7d, 0x64, 0x01, 0x16, 0x03, 0x00, 0x02, 0x2c, 0x0d, 0x75, 0x36, 0x7d, 0x6f, 0x01, 0x00, 0x01, 0xf7],
  );
  expect(parseMessage(bad)).toBeNull();
  const data = Uint8Array.of(0x80, 0x01);
  const good = pedalMsg(CMD.data, [3, ...septets(300, 3), ...septets(crc32(data), 5), ...pack7(data)]);
  expect(parseMessage(good)).toEqual({ type: "data", slot: 3, offset: 300, data });
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

test("PARAM parses", () => {
  // tests/link_test.cpp Param.Bytes.
  const text = Array.from("low_cut_freq\0Low-cut frequency\0Hz\0EQ", (c) => c.charCodeAt(0));
  const head = [0x01, 0x00, 0x01, 0x00, 0x00, 0x04, 0x0d, 0x00, 0x00, 0x00, 0x04, 0x1f, 0x68, 0x00, 0x00];
  expect(parseMessage(pedalMsg(CMD.param, [...head, ...text]))).toEqual({ type: "param", info: PARAMS[1]! });
  expect(parseMessage(pedalMsg(CMD.param, [...head, ...text.slice(0, 12)]))).toBeNull();
  expect(parseMessage(pedalMsg(CMD.param, [0x01, 0x03, ...head.slice(2), ...text]))).toBeNull();
});

test("PARAM parses a linear row", () => {
  // tests/link_test.cpp Param.LinearRow.
  const text = Array.from("bell1_gain\0Bell 1 gain\0dB\0EQ", (c) => c.charCodeAt(0));
  const head = [0x04, 0x02, 0x00, 0x40, 0x00, 0x0c, 0x0a, 0x00, 0x00, 0x00, 0x04, 0x0a, 0x00, 0x00, 0x00];
  expect(parseMessage(pedalMsg(CMD.param, [...head, ...text]))).toEqual({ type: "param", info: PARAMS[4]! });
});

test("VALUES parses", () => {
  // tests/link_test.cpp Values.Bytes.
  const codes = [
    1, 0, 8192, 1, 8192, 16383, 0, 8192, 8192, 0, 8192, 8192, 0, 16383, 8192, 1, 0, 12287, 1, 16383, 0, 8192, 8192, 0,
    0,
  ];
  const p = [...Array<number>(9).fill(0x7f), 0x02, 0x05, 0x03, 0x07, 0x19, ...codes.flatMap((c) => [c >> 7, c & 0x7f])];
  expect(parseMessage(pedalMsg(CMD.values, p))).toEqual({
    type: "values",
    assign: Array(9).fill(null),
    held: [true, false, true, false, false, false, false, false, true],
    expChannel: 3,
    midiInChannel: 7,
    codes,
  });
  expect(parseMessage(pedalMsg(CMD.values, p.slice(0, -1)))).toBeNull();
  const some = [...p];
  some[3] = 0x04;
  expect(parseMessage(pedalMsg(CMD.values, some))).toMatchObject({
    assign: [null, null, null, 4, null, null, null, null, null],
  });
  for (const at of [11, 12]) {
    const badChannel = [...p];
    badChannel[at] = 17;
    expect(parseMessage(pedalMsg(CMD.values, badChannel))).toBeNull();
  }
});

test("setMsg and assignMsg frames", () => {
  expect([...setMsg(3, 8192)]).toEqual([0xf0, 0x7d, 0x64, 0x01, 0x0f, 0x03, 0x40, 0x00, 0xf7]);
  expect([...assignMsg(4, 6)]).toEqual([0xf0, 0x7d, 0x64, 0x01, 0x10, 0x04, 0x06, 0xf7]);
  expect([...assignMsg(0, null)]).toEqual([0xf0, 0x7d, 0x64, 0x01, 0x10, 0x00, 0x7f, 0xf7]);
  expect([...expChannelMsg(5)]).toEqual([0xf0, 0x7d, 0x64, 0x01, 0x1a, 0x05, 0xf7]);
  expect([...midiInChannelMsg(5)]).toEqual([0xf0, 0x7d, 0x64, 0x01, 0x1e, 0x05, 0xf7]);
});

test("PARAMS match docs/MIDI.md's Parameters table", () => {
  // The table prints floats as C's %g: six significant digits.
  const g = (x: number): string => String(Number(x.toPrecision(6)));
  expect(tableRows("Parameters")).toEqual(
    PARAMS.map((p) => [
      String(p.id),
      `\`${p.name}\``,
      p.label,
      p.taper,
      p.taper === "choice"
        ? choiceLabels(p)
            .map((l, i) => `${i} ${l}`)
            .join(", ")
        : `${g(p.min)} to ${g(p.max)} ${p.unit}`,
      p.off === "none" ? "-" : p.off,
      String(p.defCode),
      p.group,
    ]),
  );
});
