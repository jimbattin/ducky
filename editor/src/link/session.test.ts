import { expect, test } from "bun:test";
import { crc32 } from "../nam/crc32";
import { CHUNK_BYTES, CMD, fromSeptets, HEADER, pedalMsg, RESULT, septets, unpack7 } from "./protocol";
import { PedalSession, type Transport } from "./session";

/** link.h/upload.h's semantics for BEGIN, CHUNK and COMMIT; hooks may drop or replace a CHUNK's reply. */
class FakePedal implements Transport {
  staged = new Uint8Array(0);
  chunks = 0;
  private next = 0;
  private active = false;
  private replies: Uint8Array[] = [];

  /** The CHUNK reply for chunk number n (0-based): "drop", a forced [code, value], or undefined for the real one. */
  constructor(private readonly hook: (n: number) => "drop" | [number, number] | undefined = () => undefined) {}

  async send(msg: Uint8Array): Promise<void> {
    if (HEADER.some((b, i) => msg[i] !== b)) return;
    const cmd = msg[4]!;
    const p = msg.subarray(5, msg.length - 1);
    if (cmd === CMD.uploadBegin) {
      this.staged = new Uint8Array(fromSeptets(p, 1, 3));
      this.next = 0;
      this.active = true;
      this.reply(cmd, RESULT.ok, 0);
    } else if (cmd === CMD.uploadChunk) {
      const n = this.chunks++;
      const hooked = this.hook(n);
      if (Array.isArray(hooked)) return this.reply(cmd, hooked[0], hooked[1]);
      const off = fromSeptets(p, 0, 3);
      const data = unpack7(p.subarray(8))!;
      let code: number = RESULT.ok;
      if (!this.active) code = RESULT.noUpload;
      else if (crc32(data) !== fromSeptets(p, 3, 5)) code = RESULT.crc;
      else if (off > this.next) code = RESULT.offset;
      else {
        this.staged.set(data, off);
        this.next = Math.max(this.next, off + data.length);
      }
      if (hooked !== "drop") this.reply(cmd, code, this.next);
    } else if (cmd === CMD.uploadCommit) {
      const done = this.active && this.next === this.staged.length;
      this.active = false;
      this.reply(cmd, done ? RESULT.ok : RESULT.incomplete, done ? 321 : 0);
    }
  }

  /** No reply queued: the timeout, without waiting for it. */
  async receive(_timeoutMs: number): Promise<Uint8Array | null> {
    return this.replies.shift() ?? null;
  }

  private reply(cmd: number, code: number, value: number): void {
    this.replies.push(pedalMsg(CMD.result, [cmd, code, ...septets(value, 3)]));
  }
}

const image = Uint8Array.from({ length: 16024 }, (_, i) => (i * 7 + 3) & 0xff);

test("a dropped CHUNK reply is resent once and the upload completes", async () => {
  const pedal = new FakePedal((n) => (n === 0 ? "drop" : undefined));
  const s = new PedalSession(pedal, { replyTimeoutMs: 10 });
  const r = await s.upload(2, image);
  expect(r).toEqual({ flashMs: 321, retries: 1 });
  expect(pedal.staged).toEqual(image);
});

test("an `offset` reply restarts the upload from its value", async () => {
  const pedal = new FakePedal((n) => (n === 3 ? [RESULT.offset, 0] : undefined));
  const s = new PedalSession(pedal, { replyTimeoutMs: 10 });
  const r = await s.upload(0, image);
  expect(r.retries).toBe(0);
  expect(pedal.staged).toEqual(image);
  // Chunk 3 answered `offset` 0; the resent chunk 0 is answered with the next expected offset (768), so the upload
  // resumes at chunk 3: two extra sends.
  expect(pedal.chunks).toBe(Math.ceil(image.length / CHUNK_BYTES) + 2);
});
