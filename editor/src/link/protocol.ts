// The pedal's SysEx link (daisy/src/link.h; docs/MIDI.md is the reference): message builders, the septet and Pack7
// codecs, SysEx reassembly and parsing. Pure TypeScript (no DOM, no Bun APIs): shared by the editor and tools/pedal.ts.

import { crc32 } from "../crc32";
import { sanitiseName } from "../device";
import { imageSize } from "../preset/bank";
import { encodeGate, type GateSettings } from "../preset/gate";
import { MAX_WAYPOINTS } from "../preset/morph";

export const HEADER = [0xf0, 0x7d, 0x64, 0x01] as const;
export const DEVICE = 0x64;

/** Command codes (link.h). */
export const CMD = {
  hello: 0x01,
  list: 0x02,
  erase: 0x03,
  rename: 0x04,
  swap: 0x05,
  dfu: 0x06,
  watch: 0x07,
  uploadBegin: 0x08,
  uploadChunk: 0x09,
  uploadCommit: 0x0a,
  read: 0x0b,
  gate: 0x0c,
  gateTry: 0x0d,
  dump: 0x0e,
  set: 0x0f,
  assign: 0x10,
  save: 0x11,
  log: 0x12,
  slot: 0x13,
  result: 0x14,
  status: 0x15,
  data: 0x16,
  param: 0x17,
  values: 0x18,
  morph: 0x19,
  expChannel: 0x1a,
  fwBegin: 0x1b,
  fwChunk: 0x1c,
  fwCommit: 0x1d,
} as const;

/** docs/MIDI.md's command names, by code. */
export const CMD_NAMES: Readonly<Record<number, string>> = {
  [CMD.hello]: "HELLO",
  [CMD.list]: "LIST",
  [CMD.erase]: "ERASE",
  [CMD.rename]: "RENAME",
  [CMD.swap]: "SWAP",
  [CMD.dfu]: "DFU",
  [CMD.watch]: "WATCH",
  [CMD.uploadBegin]: "UPLOAD BEGIN",
  [CMD.uploadChunk]: "UPLOAD CHUNK",
  [CMD.uploadCommit]: "UPLOAD COMMIT",
  [CMD.read]: "READ",
  [CMD.gate]: "GATE",
  [CMD.gateTry]: "GATE TRY",
  [CMD.dump]: "DUMP",
  [CMD.set]: "SET",
  [CMD.assign]: "ASSIGN",
  [CMD.save]: "SAVE",
  [CMD.log]: "LOG",
  [CMD.slot]: "SLOT",
  [CMD.result]: "RESULT",
  [CMD.status]: "STATUS",
  [CMD.data]: "DATA",
  [CMD.param]: "PARAM",
  [CMD.values]: "VALUES",
  [CMD.morph]: "MORPH",
  [CMD.expChannel]: "EXP CHANNEL",
  [CMD.fwBegin]: "FIRMWARE BEGIN",
  [CMD.fwChunk]: "FIRMWARE CHUNK",
  [CMD.fwCommit]: "FIRMWARE COMMIT",
};

/** RESULT codes (link.h Result); the index is the code. */
export const RESULT_NAMES = [
  "ok",
  "bad-request",
  "no-upload",
  "offset",
  "crc",
  "incomplete",
  "invalid",
  "flash",
  "slot-state",
] as const;
export const RESULT = {
  ok: 0,
  badRequest: 1,
  noUpload: 2,
  offset: 3,
  crc: 4,
  incomplete: 5,
  invalid: 6,
  flash: 7,
  slotState: 8,
} as const;

/** SLOT's status byte (preset.h Status); the index is the value. */
export const SLOT_STATUS_NAMES = ["ok", "empty", "magic", "version", "size", "crc", "model", "ir", "morph"] as const;
/** The SLOT statuses the editor and tools tell apart; any other is corrupt (SLOT_STATUS_NAMES says why). */
export const SLOT_STATUS = { ok: 0, empty: 1 } as const;
/** An `invalid` FIRMWARE COMMIT's value (fw_image.h Check); the index is the value. */
export const FIRMWARE_CHECK_NAMES = ["ok", "crc", "vectors", "tag", "board"] as const;

export const CHUNK_BYTES = 256;
/** A DATA's bytes, at most. */
export const READ_BYTES = 160;
/** ASSIGN's and VALUES's "no parameter" (params.h kNoParam). */
export const NO_PARAM = 0x7f;
/** EXP CHANNEL's and VALUES's "any channel", and the highest channel (midi.h kOmni, kChannels). */
export const OMNI = 0;
export const MAX_CHANNEL = 16;

/**
 * `size` is a preset image's (UPLOAD BEGIN takes no other): with or without an IR, and without a morph or with one
 * of 0-3 waypoints.
 */
export function isImageSize(size: number): boolean {
  for (const hasIr of [false, true]) {
    if (size === imageSize(hasIr, false)) return true;
    for (let k = 0; k <= MAX_WAYPOINTS; k++) if (size === imageSize(hasIr, true, k)) return true;
  }
  return false;
}

/** v as n 7-bit bytes, most significant first. */
export function septets(v: number, n: number): number[] {
  const out: number[] = [];
  for (let i = n - 1; i >= 0; i--) out.push(Math.floor(v / 2 ** (7 * i)) & 0x7f);
  return out;
}

/** The n septets at p[at]. */
export function fromSeptets(p: Uint8Array, at: number, n: number): number {
  let v = 0;
  for (let i = 0; i < n; i++) v = v * 128 + (p[at + i]! & 0x7f);
  return v;
}

/** Each group of up to 7 bytes: a header byte (bit i = the MSB of the group's byte i), then the bytes' low 7 bits. */
export function pack7(data: Uint8Array): Uint8Array {
  const out = new Uint8Array(data.length + Math.ceil(data.length / 7));
  let m = 0;
  for (let g = 0; g < data.length; g += 7) {
    const k = Math.min(7, data.length - g);
    let head = 0;
    for (let i = 0; i < k; i++) head |= (data[g + i]! >> 7) << i;
    out[m++] = head;
    for (let i = 0; i < k; i++) out[m++] = data[g + i]! & 0x7f;
  }
  return out;
}

/** pack7's inverse; null when a group has no data byte or a byte is >= 0x80. */
export function unpack7(packed: Uint8Array): Uint8Array | null {
  if (packed.length % 8 === 1 || packed.some((b) => b >= 0x80)) return null;
  const out = new Uint8Array(packed.length - Math.ceil(packed.length / 8));
  let n = 0;
  for (let g = 0; g < packed.length; g += 8) {
    const head = packed[g]!;
    for (let i = 1; i < 8 && g + i < packed.length; i++) out[n++] = packed[g + i]! | (((head >> (i - 1)) & 1) << 7);
  }
  return out;
}

/** F0 7D 64 01 <cmd> <payload> F7. */
export function pedalMsg(cmd: number, payload: ArrayLike<number> = []): Uint8Array {
  const out = new Uint8Array(HEADER.length + 2 + payload.length);
  out.set(HEADER);
  out[HEADER.length] = cmd;
  out.set(payload, HEADER.length + 1);
  out[out.length - 1] = 0xf7;
  return out;
}

export const identityRequest = (): Uint8Array => Uint8Array.of(0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7);
export const helloMsg = (): Uint8Array => pedalMsg(CMD.hello);
export const dfuMsg = (): Uint8Array => pedalMsg(CMD.dfu);
export const listMsg = (): Uint8Array => pedalMsg(CMD.list);
export const eraseMsg = (slot: number): Uint8Array => pedalMsg(CMD.erase, [slot]);
/** name as sanitiseName stores it: printable ASCII, '?' for anything else, at most NAME_MAX. */
export const renameMsg = (slot: number, name: string): Uint8Array =>
  pedalMsg(CMD.rename, [slot, ...Array.from(sanitiseName(name), (c) => c.charCodeAt(0))]);
export const swapMsg = (a: number, b: number): Uint8Array => pedalMsg(CMD.swap, [a, b]);
export const watchMsg = (on: boolean): Uint8Array => pedalMsg(CMD.watch, [on ? 1 : 0]);
export const uploadBeginMsg = (slot: number, size: number): Uint8Array =>
  pedalMsg(CMD.uploadBegin, [slot, ...septets(size, 3)]);
/** UPLOAD CHUNK's and FIRMWARE CHUNK's payload: the offset, the data's CRC-32, the data packed. */
const chunkPayload = (offset: number, bytes: Uint8Array): number[] => [
  ...septets(offset, 3),
  ...septets(crc32(bytes), 5),
  ...pack7(bytes),
];
export const uploadChunkMsg = (offset: number, bytes: Uint8Array): Uint8Array =>
  pedalMsg(CMD.uploadChunk, chunkPayload(offset, bytes));
export const uploadCommitMsg = (): Uint8Array => pedalMsg(CMD.uploadCommit);
/** size: the firmware image's bytes; crc: the whole image's CRC-32. */
export const fwBeginMsg = (size: number, crc: number): Uint8Array =>
  pedalMsg(CMD.fwBegin, [...septets(size, 3), ...septets(crc, 5)]);
export const fwChunkMsg = (offset: number, bytes: Uint8Array): Uint8Array =>
  pedalMsg(CMD.fwChunk, chunkPayload(offset, bytes));
export const fwCommitMsg = (): Uint8Array => pedalMsg(CMD.fwCommit);
export const readMsg = (slot: number, offset: number): Uint8Array => pedalMsg(CMD.read, [slot, ...septets(offset, 3)]);

/** GATE's and GATE TRY's payload: the slot, then the gate block's fields with the release as a u14. */
function gatePayload(slot: number, g: GateSettings): number[] {
  const b = encodeGate(g);
  return [slot, b[0]!, b[1]!, b[2]!, b[3]!, ...septets(b[4]! | (b[5]! << 8), 2), b[6]!];
}
export const gateMsg = (slot: number, g: GateSettings): Uint8Array => pedalMsg(CMD.gate, gatePayload(slot, g));
export const gateTryMsg = (slot: number, g: GateSettings): Uint8Array => pedalMsg(CMD.gateTry, gatePayload(slot, g));
export const dumpMsg = (): Uint8Array => pedalMsg(CMD.dump);
/** code: the value as a u14 (link/params.ts codeOf). */
export const setMsg = (id: number, code: number): Uint8Array => pedalMsg(CMD.set, [id, ...septets(code, 2)]);
/** control 0-7 (Knobs 3-6, SW1-SW3, EXP); id null: none. */
export const assignMsg = (control: number, id: number | null): Uint8Array =>
  pedalMsg(CMD.assign, [control, id ?? NO_PARAM]);
export const saveMsg = (): Uint8Array => pedalMsg(CMD.save);
/** channel: OMNI (any) or 1-16. */
export const expChannelMsg = (channel: number): Uint8Array => pedalMsg(CMD.expChannel, [channel]);
/** block: the preset's 64-byte morph block (preset/morph.ts); null removes it. */
export const morphMsg = (slot: number, block: Uint8Array | null): Uint8Array =>
  pedalMsg(CMD.morph, block ? [slot, ...pack7(block)] : [slot]);

/**
 * Reassembles SysEx messages from a MIDI byte stream (link.h SysExAssembler): realtime bytes are ignored, F0 restarts,
 * any other status byte aborts, a message longer than max bytes is dropped.
 */
export class SysExAssembler {
  private buf: number[] = [];
  private inMsg = false;

  constructor(private readonly max = 4096) {}

  feed(bytes: Uint8Array): Uint8Array[] {
    const out: Uint8Array[] = [];
    for (const b of bytes) {
      if (b >= 0xf8) continue;
      if (b === 0xf0) {
        this.buf = [b];
        this.inMsg = true;
        continue;
      }
      if (!this.inMsg) continue;
      if ((b >= 0x80 && b !== 0xf7) || this.buf.length === this.max) {
        this.inMsg = false;
        continue;
      }
      this.buf.push(b);
      if (b === 0xf7) {
        this.inMsg = false;
        out.push(Uint8Array.from(this.buf));
      }
    }
    return out;
  }
}

export interface LogMessage {
  type: "log";
  text: string;
}
export interface SlotMessage {
  type: "slot";
  /** 0-7. */
  slot: number;
  /** SLOT_STATUS_NAMES index. */
  status: number;
  hasIr: boolean;
  playing: boolean;
  name: string;
  model: string;
  ir: string;
}
export interface ResultMessage {
  type: "result";
  cmd: number;
  code: number;
  value: number;
}
export interface StatusMessage {
  type: "status";
  /** Permille. */
  loadAvg: number;
  loadMax: number;
  underruns: number;
  /** 0-7, null when dry. */
  slot: number | null;
  muted: boolean;
  /** Knobs 1-6 in permille. */
  knobs: number[];
  /** SW1-SW3: 0 down, 1 middle, 2 up. */
  toggles: number[];
  fs1: boolean;
  fs2: boolean;
  /** The gate detector's highest input level since the previous STATUS, dBFS (<= 0, -120 for silence). */
  gateLevelDb: number;
  /** The gate's largest gain reduction since the previous STATUS, dB (>= 0, 120 closed). */
  gateCutDb: number;
  /** The compressor's largest gain reduction since the previous STATUS, dB (>= 0). */
  compCutDb: number;
}
/** The morph the firmware plays: both models crossfaded (the default image), or their weights blended (APP=weight). */
export type MorphKind = "crossfade" | "weight";
export interface IdentityMessage {
  type: "identity";
  device: number;
  board: number;
  /** From the flags byte (the first version byte): bit 0 the weight morph. */
  morph: MorphKind;
  build: string;
}
export interface DataMessage {
  type: "data";
  /** 0-7. */
  slot: number;
  /** The image offset of data[0]. */
  offset: number;
  data: Uint8Array;
}
/** One row of the pedal's parameter table (params.h ParamDef; docs/MIDI.md "Parameters"). */
export interface ParamInfo {
  id: number;
  /**
   * log: continuous, min at code 0 to max at code 16383 logarithmically; linear: the same, linearly; choice: the code
   * is an index 0..max.
   */
  taper: "log" | "choice" | "linear";
  /** The end at which the parameter's filter is bypassed. */
  off: "none" | "min" | "max";
  /** The default value's code. */
  defCode: number;
  min: number;
  max: number;
  /** The log's and tools/pedal.ts's name. */
  name: string;
  label: string;
  /** A choice's labels, comma-separated, in index order. */
  unit: string;
  group: string;
}
export interface ParamMessage {
  type: "param";
  info: ParamInfo;
}
export interface ValuesMessage {
  type: "values";
  /** Per control 0-7 (Knobs 3-6, SW1-SW3, EXP): its parameter's id, or null. */
  assign: (number | null)[];
  /** Per control 0-7: held, its parameter waits for the control to move. */
  held: boolean[];
  /** The MIDI channel EXP's CC 11 and the preset CC 102 are taken from: 0 any (omni), 1-16. */
  expChannel: number;
  /** Per parameter id: its value's code. */
  codes: number[];
}
export type PedalMessage =
  | LogMessage
  | SlotMessage
  | ResultMessage
  | StatusMessage
  | IdentityMessage
  | DataMessage
  | ParamMessage
  | ValuesMessage;

const CONTROL_COUNT = 8;
const PARAM_HEAD = 15;
const TAPERS = ["log", "choice", "linear"] as const;
const OFF_ENDS = ["none", "min", "max"] as const;
/** The float32 whose bits are the u32 at p[at] (5 septets). */
function f32At(p: Uint8Array, at: number): number {
  const view = new DataView(new ArrayBuffer(4));
  view.setUint32(0, fromSeptets(p, at, 5));
  return view.getFloat32(0);
}

const ascii = (bytes: Uint8Array): string => String.fromCharCode(...bytes);

/** A complete F0 ... F7 message from the pedal; null for anything else (or a malformed one). */
export function parseMessage(msg: Uint8Array): PedalMessage | null {
  const n = msg.length;
  if (n < 2 || msg[0] !== 0xf0 || msg[n - 1] !== 0xf7) return null;
  if (n >= 15 && msg[1] === 0x7e && msg[3] === 0x06 && msg[4] === 0x02)
    return {
      type: "identity",
      device: msg[6]!,
      board: msg[8]!,
      morph: (msg[10]! & 0x01) !== 0 ? "weight" : "crossfade",
      build: ascii(msg.subarray(14, n - 1)),
    };
  if (n < 6 || HEADER.some((b, i) => msg[i] !== b)) return null;
  const p = msg.subarray(5, n - 1);
  switch (msg[4]) {
    case CMD.log:
      return { type: "log", text: ascii(p) };
    case CMD.slot: {
      if (p.length < 5) return null;
      const names = ascii(p.subarray(3)).split("\0");
      if (names.length !== 3) return null;
      return {
        type: "slot",
        slot: p[0]!,
        status: p[1]!,
        hasIr: (p[2]! & 1) !== 0,
        playing: (p[2]! & 2) !== 0,
        name: names[0]!,
        model: names[1]!,
        ir: names[2]!,
      };
    }
    case CMD.result:
      if (p.length !== 5) return null;
      return { type: "result", cmd: p[0]!, code: p[1]!, value: fromSeptets(p, 2, 3) };
    case CMD.status:
      if (p.length !== 32) return null;
      return {
        type: "status",
        loadAvg: fromSeptets(p, 0, 2),
        loadMax: fromSeptets(p, 2, 2),
        underruns: fromSeptets(p, 4, 4),
        slot: p[8] === 0x7f ? null : p[8]!,
        muted: p[9] === 1,
        knobs: Array.from({ length: 6 }, (_, k) => fromSeptets(p, 10 + 2 * k, 2)),
        toggles: [p[22]!, p[23]!, p[24]!],
        fs1: (p[25]! & 1) !== 0,
        fs2: (p[25]! & 2) !== 0,
        gateLevelDb: -fromSeptets(p, 26, 2) / 10,
        gateCutDb: fromSeptets(p, 28, 2) / 10,
        compCutDb: fromSeptets(p, 30, 2) / 10,
      };
    case CMD.data: {
      // A corrupt DATA reads as missing, so the session resends its READ.
      if (p.length < 10) return null;
      const data = unpack7(p.subarray(9));
      if (data === null || data.length === 0 || data.length > READ_BYTES) return null;
      if (crc32(data) !== fromSeptets(p, 4, 5)) return null;
      return { type: "data", slot: p[0]!, offset: fromSeptets(p, 1, 3), data };
    }
    case CMD.param: {
      if (p.length < PARAM_HEAD) return null;
      const taper = TAPERS[p[1]!];
      const off = OFF_ENDS[p[2]!];
      const text = ascii(p.subarray(PARAM_HEAD)).split("\0");
      if (taper === undefined || off === undefined || text.length !== 4) return null;
      return {
        type: "param",
        info: {
          id: p[0]!,
          taper,
          off,
          defCode: fromSeptets(p, 3, 2),
          min: f32At(p, 5),
          max: f32At(p, 10),
          name: text[0]!,
          label: text[1]!,
          unit: text[2]!,
          group: text[3]!,
        },
      };
    }
    case CMD.values: {
      // The assignments, held u14, the channel, N, then the codes (link.h kValuesHead).
      const head = CONTROL_COUNT + 4;
      if (p.length < head || p.length !== head + 2 * p[head - 1]! || p[CONTROL_COUNT + 2]! > MAX_CHANNEL) return null;
      const held = fromSeptets(p, CONTROL_COUNT, 2);
      return {
        type: "values",
        assign: Array.from(p.subarray(0, CONTROL_COUNT), (id) => (id === NO_PARAM ? null : id)),
        held: Array.from({ length: CONTROL_COUNT }, (_, c) => ((held >> c) & 1) !== 0),
        expChannel: p[CONTROL_COUNT + 2]!,
        codes: Array.from({ length: p[head - 1]! }, (_, i) => fromSeptets(p, head + 2 * i, 2)),
      };
    }
    default:
      return null;
  }
}
