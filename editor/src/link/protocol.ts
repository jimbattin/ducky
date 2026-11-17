// The pedal's SysEx link (daisy/src/link.h; docs/MIDI.md is the reference): message builders, the septet and Pack7
// codecs, SysEx reassembly and parsing. Pure TypeScript (no DOM, no Bun APIs): shared by the editor and tools/pedal.ts.

import { IR_BIN_SIZE } from "../ir/convert";
import { MODEL_BIN_SIZE } from "../nam/convert";
import { crc32 } from "../nam/crc32";
import { PRESET_HEADER_SIZE } from "../preset/bank";
import { encodeGate, type GateSettings } from "../preset/gate";

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
  log: 0x12,
  slot: 0x13,
  result: 0x14,
  status: 0x15,
  data: 0x16,
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
  [CMD.log]: "LOG",
  [CMD.slot]: "SLOT",
  [CMD.result]: "RESULT",
  [CMD.status]: "STATUS",
  [CMD.data]: "DATA",
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
export const SLOT_STATUS_NAMES = ["ok", "empty", "magic", "version", "size", "crc", "model", "ir"] as const;

export const CHUNK_BYTES = 256;
/** A DATA's bytes, at most. */
export const READ_BYTES = 160;
export const MAX_NAME = 63;
/** The two preset image sizes: without and with an IR. */
export const IMAGE_SIZES = [
  PRESET_HEADER_SIZE + MODEL_BIN_SIZE + 4,
  PRESET_HEADER_SIZE + MODEL_BIN_SIZE + IR_BIN_SIZE + 4,
] as const;

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
/** name: printable ASCII; other characters become '?'. */
export const renameMsg = (slot: number, name: string): Uint8Array =>
  pedalMsg(CMD.rename, [slot, ...Array.from(name, (c) => (c >= " " && c <= "~" ? c.charCodeAt(0) : 0x3f))]);
export const swapMsg = (a: number, b: number): Uint8Array => pedalMsg(CMD.swap, [a, b]);
export const watchMsg = (on: boolean): Uint8Array => pedalMsg(CMD.watch, [on ? 1 : 0]);
export const uploadBeginMsg = (slot: number, size: number): Uint8Array =>
  pedalMsg(CMD.uploadBegin, [slot, ...septets(size, 3)]);
export const uploadChunkMsg = (offset: number, bytes: Uint8Array): Uint8Array =>
  pedalMsg(CMD.uploadChunk, [...septets(offset, 3), ...septets(crc32(bytes), 5), ...pack7(bytes)]);
export const uploadCommitMsg = (): Uint8Array => pedalMsg(CMD.uploadCommit);
export const readMsg = (slot: number, offset: number): Uint8Array => pedalMsg(CMD.read, [slot, ...septets(offset, 3)]);

/** GATE's and GATE TRY's payload: the slot, then the gate block's fields with the release as a u14. */
function gatePayload(slot: number, g: GateSettings): number[] {
  const b = encodeGate(g);
  return [slot, b[0]!, b[1]!, b[2]!, b[3]!, ...septets(b[4]! | (b[5]! << 8), 2), b[6]!];
}
export const gateMsg = (slot: number, g: GateSettings): Uint8Array => pedalMsg(CMD.gate, gatePayload(slot, g));
export const gateTryMsg = (slot: number, g: GateSettings): Uint8Array => pedalMsg(CMD.gateTry, gatePayload(slot, g));

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
}
export interface IdentityMessage {
  type: "identity";
  device: number;
  board: number;
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
export type PedalMessage = LogMessage | SlotMessage | ResultMessage | StatusMessage | IdentityMessage | DataMessage;

const ascii = (bytes: Uint8Array): string => String.fromCharCode(...bytes);

/** A complete F0 ... F7 message from the pedal; null for anything else (or a malformed one). */
export function parseMessage(msg: Uint8Array): PedalMessage | null {
  const n = msg.length;
  if (n < 2 || msg[0] !== 0xf0 || msg[n - 1] !== 0xf7) return null;
  if (n >= 15 && msg[1] === 0x7e && msg[3] === 0x06 && msg[4] === 0x02)
    return { type: "identity", device: msg[6]!, board: msg[8]!, build: ascii(msg.subarray(14, n - 1)) };
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
      if (p.length !== 30) return null;
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
      };
    case CMD.data: {
      // A corrupt DATA reads as missing, so the session resends its READ.
      if (p.length < 10) return null;
      const data = unpack7(p.subarray(9));
      if (data === null || data.length === 0 || data.length > READ_BYTES) return null;
      if (crc32(data) !== fromSeptets(p, 4, 5)) return null;
      return { type: "data", slot: p[0]!, offset: fromSeptets(p, 1, 3), data };
    }
    default:
      return null;
  }
}
