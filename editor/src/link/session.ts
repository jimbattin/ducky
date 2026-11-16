// A request/reply session with the pedal over any SysEx transport (docs/MIDI.md): slot listing and changes, the
// chunked upload with its retries, and the STATUS stream. Pure TypeScript: the transport supplies the I/O.

import {
  CHUNK_BYTES,
  CMD,
  CMD_NAMES,
  DEVICE,
  eraseMsg,
  type IdentityMessage,
  IMAGE_SIZES,
  identityRequest,
  listMsg,
  MAX_NAME,
  type PedalMessage,
  parseMessage,
  RESULT,
  RESULT_NAMES,
  type ResultMessage,
  renameMsg,
  SLOT_STATUS_NAMES,
  type SlotMessage,
  swapMsg,
  uploadBeginMsg,
  uploadChunkMsg,
  uploadCommitMsg,
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
  /** LIST, WATCH, Identity, BEGIN and CHUNK. */
  replyTimeoutMs?: number;
  /** COMMIT, ERASE, RENAME and SWAP: they write flash. */
  flashTimeoutMs?: number;
  /** Resends of one BEGIN or CHUNK. */
  retries?: number;
  /** LOG lines that arrive while waiting for a reply. */
  onLog?: (text: string) => void;
}

const SLOTS = 8;

function checkSlot(what: string, slot: number): void {
  if (!Number.isInteger(slot) || slot < 0 || slot >= SLOTS) throw new LinkError(`${what}: slot ${slot}: must be 0-7`);
}

function resultError(r: ResultMessage): LinkError {
  const name = RESULT_NAMES[r.code] ?? `result ${r.code}`;
  const detail = r.code === RESULT.invalid ? ` (${SLOT_STATUS_NAMES[r.value] ?? r.value})` : "";
  return new LinkError(`${CMD_NAMES[r.cmd] ?? r.cmd}: ${name}${detail}`);
}

export class PedalSession {
  private readonly replyTimeoutMs: number;
  private readonly flashTimeoutMs: number;
  private readonly retries: number;
  private readonly onLog: ((text: string) => void) | undefined;

  constructor(
    private readonly transport: Transport,
    options: SessionOptions = {},
  ) {
    this.replyTimeoutMs = options.replyTimeoutMs ?? 1000;
    this.flashTimeoutMs = options.flashTimeoutMs ?? 10000;
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
    if (slots.length !== SLOTS) throw new LinkError(`LIST: ${slots.length} slots, need ${SLOTS}`);
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
    if (name.length < 1 || name.length > MAX_NAME || !/^[\x20-\x7e]+$/.test(name))
      throw new LinkError(`RENAME: name must be 1-${MAX_NAME} printable ASCII characters`);
    return (await this.request(renameMsg(slot, name), CMD.rename, this.flashTimeoutMs)).value;
  }

  /** Exchanges two slots; the flash time in ms. */
  async swap(a: number, b: number): Promise<number> {
    checkSlot("SWAP", a);
    checkSlot("SWAP", b);
    return (await this.request(swapMsg(a, b), CMD.swap, this.flashTimeoutMs)).value;
  }

  /** STATUS every 100 ms while on. */
  async watch(on: boolean): Promise<void> {
    await this.request(watchMsg(on), CMD.watch, this.replyTimeoutMs);
  }

  /**
   * Writes a preset image (7732 or 16024 bytes) to the slot. BEGIN and each CHUNK are resent on no reply (and a CHUNK
   * on `crc`) up to `retries` times; COMMIT is not resent (the pedal has cleared the upload).
   */
  async upload(
    slot: number,
    image: Uint8Array,
    onProgress?: (sent: number, size: number) => void,
  ): Promise<{ flashMs: number; retries: number }> {
    checkSlot("UPLOAD", slot);
    const size = image.length;
    if (!(IMAGE_SIZES as readonly number[]).includes(size))
      throw new LinkError(`UPLOAD: image size ${size}, need ${IMAGE_SIZES.join(" or ")}`);
    let retries = 0;
    for (let tries = 0; ; tries++) {
      const r = await this.exchange(uploadBeginMsg(slot, size), CMD.uploadBegin, this.replyTimeoutMs);
      if (r !== null) {
        if (r.code !== RESULT.ok) throw resultError(r);
        break;
      }
      if (tries >= this.retries) throw new LinkError("UPLOAD BEGIN: no reply");
      retries++;
    }
    let off = 0;
    let tries = 0;
    while (off < size) {
      const r = await this.exchange(
        uploadChunkMsg(off, image.subarray(off, Math.min(off + CHUNK_BYTES, size))),
        CMD.uploadChunk,
        this.replyTimeoutMs,
      );
      if (r !== null && (r.code === RESULT.ok || r.code === RESULT.offset)) {
        off = r.value;
        tries = 0;
        onProgress?.(off, size);
        continue;
      }
      if (r !== null && r.code !== RESULT.crc) throw resultError(r);
      if (tries >= this.retries) throw new LinkError(`UPLOAD CHUNK at ${off}: ${r === null ? "no reply" : "crc"}`);
      tries++;
      retries++;
    }
    const r = await this.exchange(uploadCommitMsg(), CMD.uploadCommit, this.flashTimeoutMs);
    if (r === null) throw new LinkError("UPLOAD COMMIT: no reply; check with list");
    if (r.code !== RESULT.ok) throw resultError(r);
    return { flashMs: r.value, retries };
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
