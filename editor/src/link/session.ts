// A request/reply session with the pedal over any SysEx transport (docs/MIDI.md): slot listing, reading and changes,
// the chunked upload with its retries, the firmware update, the parameter table, values and control assignments, and
// the STATUS stream.
// Pure TypeScript: the transport supplies the I/O.

import { crc32 } from "../crc32";
import { NAME_MAX } from "../device";
import { MODEL_BIN_SIZE } from "../nam/convert";
import {
  imageSize,
  OFF_FLAGS,
  OFF_SIZE,
  PRESET_FLAG_HAS_IR,
  PRESET_FLAG_HAS_MORPH,
  PRESET_HEADER_SIZE,
  SLOT_BYTES,
  SLOT_COUNT,
} from "../preset/bank";
import { type GateSettings, gateProblem } from "../preset/gate";
import { infoRanges, type SlotInfo, slotInfo } from "../preset/info";
import { firmwareProblem } from "./firmware";
import { CONTROLS } from "./params";
import {
  assignMsg,
  CHUNK_BYTES,
  CMD,
  CMD_NAMES,
  DEVICE,
  dumpMsg,
  eraseMsg,
  expChannelMsg,
  FIRMWARE_CHECK_NAMES,
  fwBeginMsg,
  fwChunkMsg,
  fwCommitMsg,
  gateMsg,
  gateTryMsg,
  type IdentityMessage,
  identityRequest,
  isImageSize,
  listMsg,
  MAX_CHANNEL,
  morphMsg,
  OMNI,
  type ParamInfo,
  type PedalMessage,
  parseMessage,
  RESULT,
  RESULT_NAMES,
  type ResultMessage,
  readMsg,
  renameMsg,
  SLOT_STATUS_NAMES,
  type SlotMessage,
  saveMsg,
  setMsg,
  swapMsg,
  uploadBeginMsg,
  uploadChunkMsg,
  uploadCommitMsg,
  type ValuesMessage,
  watchMsg,
} from "./protocol";

/** Complete SysEx messages both ways. */
export interface Transport {
  send(msg: Uint8Array): Promise<void>;
  /** The next complete message, or null when none arrives within timeoutMs. */
  receive(timeoutMs: number): Promise<Uint8Array | null>;
}

export class LinkError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "LinkError";
  }
}

export interface SessionOptions {
  /** LIST, WATCH, Identity, READ, BEGIN, CHUNK, DUMP, SET and ASSIGN. */
  replyTimeoutMs?: number;
  /** COMMIT, ERASE, RENAME, SWAP, GATE, MORPH and SAVE: they write flash. */
  flashTimeoutMs?: number;
  /** FIRMWARE COMMIT: erases and writes up to 480 KB. */
  firmwareTimeoutMs?: number;
  /** Resends of one READ, BEGIN or CHUNK. */
  retries?: number;
  /** LOG lines that arrive while waiting for a reply. */
  onLog?: (text: string) => void;
}

function checkSlot(what: string, slot: number): void {
  if (!Number.isInteger(slot) || slot < 0 || slot >= SLOT_COUNT)
    throw new LinkError(`${what}: slot ${slot}: must be 0-${SLOT_COUNT - 1}`);
}

function checkGate(what: string, slot: number, g: GateSettings): void {
  checkSlot(what, slot);
  const problem = gateProblem(g);
  if (problem !== null) throw new LinkError(`${what}: ${problem}`);
}

function resultError(r: ResultMessage): LinkError {
  const name = RESULT_NAMES[r.code] ?? `result ${r.code}`;
  const values = r.cmd === CMD.fwCommit ? FIRMWARE_CHECK_NAMES : SLOT_STATUS_NAMES;
  const detail = r.code === RESULT.invalid ? ` (${values[r.value] ?? r.value})` : "";
  return new LinkError(`${CMD_NAMES[r.cmd] ?? r.cmd}: ${name}${detail}`);
}

export class PedalSession {
  private readonly replyTimeoutMs: number;
  private readonly flashTimeoutMs: number;
  private readonly firmwareTimeoutMs: number;
  private readonly retries: number;
  private readonly onLog: ((text: string) => void) | undefined;

  constructor(
    private readonly transport: Transport,
    options: SessionOptions = {},
  ) {
    this.replyTimeoutMs = options.replyTimeoutMs ?? 1000;
    this.flashTimeoutMs = options.flashTimeoutMs ?? 10000;
    this.firmwareTimeoutMs = options.firmwareTimeoutMs ?? 30000;
    this.retries = options.retries ?? 5;
    this.onLog = options.onLog;
  }

  /** The next message from the pedal (LOG included), or null after timeoutMs. */
  async next(timeoutMs: number): Promise<PedalMessage | null> {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const left = deadline - Date.now();
      if (left <= 0) return null;
      const raw = await this.transport.receive(left);
      if (raw === null) return null;
      const m = parseMessage(raw);
      if (m !== null) return m;
    }
  }

  /** The Identity Reply; LinkError when none arrives or it is not a Ducky's. */
  async identity(): Promise<IdentityMessage> {
    await this.transport.send(identityRequest());
    const deadline = Date.now() + this.replyTimeoutMs;
    for (;;) {
      const m = await this.next(deadline - Date.now());
      if (m === null) throw new LinkError("Identity: no reply");
      if (m.type === "log") this.onLog?.(m.text);
      else if (m.type === "identity") {
        if (m.device !== DEVICE) throw new LinkError(`Identity: device ${m.device.toString(16)}, not a Ducky`);
        return m;
      }
    }
  }

  /** The 8 slots, in order. */
  async list(): Promise<SlotMessage[]> {
    const slots: SlotMessage[] = [];
    await this.request(listMsg(), CMD.list, this.replyTimeoutMs, (m) => {
      if (m.type === "slot") slots.push(m);
    });
    if (slots.length !== SLOT_COUNT) throw new LinkError(`LIST: ${slots.length} slots, need ${SLOT_COUNT}`);
    return slots;
  }

  /** Erases the slot; the flash time in ms. */
  async erase(slot: number): Promise<number> {
    checkSlot("ERASE", slot);
    return (await this.request(eraseMsg(slot), CMD.erase, this.flashTimeoutMs)).value;
  }

  /** Renames the slot's preset (1-63 printable ASCII characters); the flash time in ms. */
  async rename(slot: number, name: string): Promise<number> {
    checkSlot("RENAME", slot);
    if (name.length < 1 || name.length > NAME_MAX || !/^[\x20-\x7e]+$/.test(name))
      throw new LinkError(`RENAME: name must be 1-${NAME_MAX} printable ASCII characters`);
    return (await this.request(renameMsg(slot, name), CMD.rename, this.flashTimeoutMs)).value;
  }

  /** Exchanges two slots; the flash time in ms. */
  async swap(a: number, b: number): Promise<number> {
    checkSlot("SWAP", a);
    checkSlot("SWAP", b);
    return (await this.request(swapMsg(a, b), CMD.swap, this.flashTimeoutMs)).value;
  }

  /** Rewrites the slot's noise gate (the slot must be ok); the flash time in ms. */
  async gate(slot: number, g: GateSettings): Promise<number> {
    checkGate("GATE", slot, g);
    return (await this.request(gateMsg(slot, g), CMD.gate, this.flashTimeoutMs)).value;
  }

  /**
   * Rewrites the slot's morph block (64 bytes, preset/morph.ts; null removes it): the slot and the block's target
   * must be ok. The flash time in ms.
   */
  async morph(slot: number, block: Uint8Array | null): Promise<number> {
    checkSlot("MORPH", slot);
    return (await this.request(morphMsg(slot, block), CMD.morph, this.flashTimeoutMs)).value;
  }

  /** Sets the playing slot's running gate only (an audition): nothing is written, the next preset load undoes it. */
  async gateTry(slot: number, g: GateSettings): Promise<void> {
    checkGate("GATE TRY", slot, g);
    await this.request(gateTryMsg(slot, g), CMD.gateTry, this.replyTimeoutMs);
  }

  /**
   * length bytes of the slot's preset image from offset, one READ per DATA. A READ is resent on no reply or no
   * matching DATA up to `retries` times.
   */
  async read(slot: number, offset: number, length: number): Promise<Uint8Array> {
    checkSlot("READ", slot);
    const out = new Uint8Array(length);
    let got = 0;
    let tries = 0;
    while (got < length) {
      const off = offset + got;
      let data = null as Uint8Array | null;
      const r = await this.exchange(readMsg(slot, off), CMD.read, this.replyTimeoutMs, (m) => {
        if (m.type === "data" && m.slot === slot && m.offset === off) data = m.data;
      });
      if (r !== null && r.code !== RESULT.ok) throw resultError(r);
      if (r === null || data === null) {
        if (tries >= this.retries) throw new LinkError(`READ at ${off}: ${r === null ? "no reply" : "no data"}`);
        tries++;
        continue;
      }
      const n = Math.min(data.length, length - got);
      out.set(data.subarray(0, n), got);
      got += n;
      tries = 0;
    }
    return out;
  }

  /** The metadata of the preset in an ok slot, read with infoRanges (a morph's waypoints are not read). */
  async readInfo(slot: number): Promise<SlotInfo> {
    const [head] = infoRanges(false, false);
    const first = await this.read(slot, head![0], head![1]);
    const hasIr = (first[OFF_FLAGS]! & PRESET_FLAG_HAS_IR) !== 0;
    const hasMorph = (first[OFF_FLAGS]! & PRESET_FLAG_HAS_MORPH) !== 0;
    const image = new Uint8Array(imageSize(hasIr, hasMorph));
    image.set(first, head![0]);
    for (const [off, len] of infoRanges(hasIr, hasMorph).slice(1)) image.set(await this.read(slot, off, len), off);
    return slotInfo(image);
  }

  /** The whole preset image of an ok slot: its size field's bytes. */
  async readImage(slot: number): Promise<Uint8Array> {
    const head = await this.read(slot, 0, OFF_SIZE + 4);
    const size = new DataView(head.buffer).getUint32(OFF_SIZE, true);
    if (size < head.length || size > SLOT_BYTES) throw new LinkError(`READ: slot ${slot}: image size ${size}`);
    const image = new Uint8Array(size);
    image.set(head);
    image.set(await this.read(slot, head.length, size - head.length), head.length);
    return image;
  }

  /** The model binary of the preset in an ok slot. */
  readModel(slot: number): Promise<Uint8Array> {
    return this.read(slot, PRESET_HEADER_SIZE, MODEL_BIN_SIZE);
  }

  /** STATUS and VALUES every 100 ms while on. */
  async watch(on: boolean): Promise<void> {
    await this.request(watchMsg(on), CMD.watch, this.replyTimeoutMs);
  }

  /** The parameter table (by id) and the live assignments and values. */
  async dump(): Promise<{ params: ParamInfo[]; values: ValuesMessage }> {
    const params: ParamInfo[] = [];
    let values = null as ValuesMessage | null;
    const r = await this.request(dumpMsg(), CMD.dump, this.replyTimeoutMs, (m) => {
      if (m.type === "param") params.push(m.info);
      else if (m.type === "values") values = m;
    });
    if (params.length !== r.value || params.some((p, i) => p.id !== i))
      throw new LinkError(`DUMP: ${params.length} parameters, need ${r.value} in id order`);
    if (values === null || values.codes.length !== params.length) throw new LinkError("DUMP: no values");
    return { params, values };
  }

  /** Sets a parameter's value from its code (link/params.ts codeOf); the code the pedal took. */
  async set(id: number, code: number): Promise<number> {
    return (await this.request(setMsg(id, code), CMD.set, this.replyTimeoutMs)).value;
  }

  /** Puts a parameter (null: none) on control 0-7; it keeps its value until the control moves. */
  async assign(control: number, id: number | null): Promise<void> {
    if (!Number.isInteger(control) || control < 0 || control >= CONTROLS.length)
      throw new LinkError(`ASSIGN: control ${control}: must be 0-${CONTROLS.length - 1}`);
    await this.request(assignMsg(control, id), CMD.assign, this.replyTimeoutMs);
  }

  /** The MIDI channel EXP's CC 11 and the preset CC 102 are taken from: 0 any (omni), 1-16. Kept by save(). */
  async expChannel(channel: number): Promise<void> {
    if (!Number.isInteger(channel) || channel < OMNI || channel > MAX_CHANNEL)
      throw new LinkError(`EXP CHANNEL: channel ${channel}: must be 0-${MAX_CHANNEL}`);
    await this.request(expChannelMsg(channel), CMD.expChannel, this.replyTimeoutMs);
  }

  /** Writes the assignments, the expression channel and the values to the settings record; the flash time in ms. */
  async save(): Promise<number> {
    return (await this.request(saveMsg(), CMD.save, this.flashTimeoutMs)).value;
  }

  /**
   * Writes a preset image (protocol.ts isImageSize) to the slot. BEGIN and each CHUNK are resent on no reply (and a CHUNK
   * on `crc`) up to `retries` times; COMMIT is not resent (the pedal has cleared the upload).
   */
  async upload(
    slot: number,
    image: Uint8Array,
    onProgress?: (sent: number, size: number) => void,
  ): Promise<{ flashMs: number; retries: number }> {
    checkSlot("UPLOAD", slot);
    const size = image.length;
    if (!isImageSize(size)) throw new LinkError(`UPLOAD: ${size} bytes is not a preset image's size`);
    const retries = await this.transfer(
      uploadBeginMsg(slot, size),
      CMD.uploadBegin,
      uploadChunkMsg,
      CMD.uploadChunk,
      image,
      onProgress,
    );
    const r = await this.exchange(uploadCommitMsg(), CMD.uploadCommit, this.flashTimeoutMs);
    if (r === null) throw new LinkError("UPLOAD COMMIT: no reply; check with list");
    if (r.code !== RESULT.ok) throw resultError(r);
    return { flashMs: r.value, retries };
  }

  /**
   * Writes a firmware image (firmware.ts firmwareProblem) to the pedal, which then restarts into it: its link drops
   * for a few seconds and it comes back with the new Identity. BEGIN and CHUNK are resent as for upload; COMMIT only on
   * `flash` (the pedal keeps the staged image then).
   */
  async updateFirmware(
    image: Uint8Array,
    onProgress?: (sent: number, size: number) => void,
  ): Promise<{ flashMs: number; retries: number }> {
    const problem = firmwareProblem(image);
    if (problem !== null) throw new LinkError(`FIRMWARE: ${problem}`);
    let retries = await this.transfer(
      fwBeginMsg(image.length, crc32(image)),
      CMD.fwBegin,
      fwChunkMsg,
      CMD.fwChunk,
      image,
      onProgress,
    );
    for (let sends = 1; ; sends++) {
      const r = await this.exchange(fwCommitMsg(), CMD.fwCommit, this.firmwareTimeoutMs);
      if (r === null) throw new LinkError("FIRMWARE COMMIT: no reply; check the pedal's Identity build");
      if (r.code === RESULT.ok) return { flashMs: r.value, retries };
      if (r.code !== RESULT.flash || sends >= 3) throw resultError(r);
      retries++;
    }
  }

  /**
   * BEGIN, then image in CHUNK_BYTES chunks. BEGIN and each CHUNK are resent on no reply (and a CHUNK on `crc`) up to
   * `retries` times; an `offset` RESULT continues from its offset. Returns the resends.
   */
  private async transfer(
    begin: Uint8Array,
    beginCmd: number,
    chunkMsg: (offset: number, bytes: Uint8Array) => Uint8Array,
    chunkCmd: number,
    image: Uint8Array,
    onProgress?: (sent: number, size: number) => void,
  ): Promise<number> {
    const size = image.length;
    let retries = 0;
    for (let tries = 0; ; tries++) {
      const r = await this.exchange(begin, beginCmd, this.replyTimeoutMs);
      if (r !== null) {
        if (r.code !== RESULT.ok) throw resultError(r);
        break;
      }
      if (tries >= this.retries) throw new LinkError(`${CMD_NAMES[beginCmd]}: no reply`);
      retries++;
    }
    let off = 0;
    let tries = 0;
    while (off < size) {
      const r = await this.exchange(
        chunkMsg(off, image.subarray(off, Math.min(off + CHUNK_BYTES, size))),
        chunkCmd,
        this.replyTimeoutMs,
      );
      if (r !== null && (r.code === RESULT.ok || r.code === RESULT.offset)) {
        off = r.value;
        tries = 0;
        onProgress?.(off, size);
        continue;
      }
      if (r !== null && r.code !== RESULT.crc) throw resultError(r);
      if (tries >= this.retries)
        throw new LinkError(`${CMD_NAMES[chunkCmd]} at ${off}: ${r === null ? "no reply" : "crc"}`);
      tries++;
      retries++;
    }
    return retries;
  }

  /** Sends msg; its ok RESULT, LinkError on any other code or no reply. */
  private async request(
    msg: Uint8Array,
    cmd: number,
    timeoutMs: number,
    onOther?: (m: PedalMessage) => void,
  ): Promise<ResultMessage> {
    const r = await this.exchange(msg, cmd, timeoutMs, onOther);
    if (r === null) throw new LinkError(`${CMD_NAMES[cmd]}: no reply`);
    if (r.code !== RESULT.ok) throw resultError(r);
    return r;
  }

  /** Sends msg; the RESULT for cmd, or null after timeoutMs. LOG goes to onLog; other messages to onOther. */
  private async exchange(
    msg: Uint8Array,
    cmd: number,
    timeoutMs: number,
    onOther?: (m: PedalMessage) => void,
  ): Promise<ResultMessage | null> {
    await this.transport.send(msg);
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const m = await this.next(deadline - Date.now());
      if (m === null) return null;
      if (m.type === "result" && m.cmd === cmd) return m;
      if (m.type === "log") this.onLog?.(m.text);
      else onOther?.(m);
    }
  }
}
