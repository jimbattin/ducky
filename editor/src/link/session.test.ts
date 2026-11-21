import { expect, test } from "bun:test";
import { crc32 } from "../crc32";
import { firmwareImage } from "../testFixtures";
import {
  CHUNK_BYTES,
  CMD,
  fromSeptets,
  HEADER,
  pack7,
  pedalMsg,
  READ_BYTES,
  RESULT,
  septets,
  unpack7,
} from "./protocol";
import { LinkError, PedalSession, type Transport } from "./session";

/**
 * link.h/upload.h's semantics for BEGIN, CHUNK and COMMIT (UPLOAD and FIRMWARE), and ducky.cpp's READ over `flash`
 * (slot 0's image); hooks may drop or replace a CHUNK's reply, or drop a READ's DATA.
 */
class FakePedal implements Transport {
  staged = new Uint8Array(0);
  chunks = 0;
  flash = new Uint8Array(0);
  /** The offset of every READ received. */
  reads: number[] = [];
  /** True drops the DATA of READ number n (0-based). */
  dropData: (n: number) => boolean = () => false;
  /** FIRMWARE COMMIT's replies in turn (the last repeats): ok, or a [code, value]. */
  fwCommitCodes: ("ok" | [number, number])[] = ["ok"];
  /** Every command received, in order. */
  received: number[] = [];
  private next = 0;
  private active = false;
  private replies: Uint8Array[] = [];
  /** The CHUNK reply for chunk number n (0-based): "drop", a forced [code, value], or undefined for the real one. */
  constructor(private readonly hook: (n: number) => "drop" | [number, number] | undefined = () => undefined) {}

  async send(msg: Uint8Array): Promise<void> {
    if (HEADER.some((b, i) => msg[i] !== b)) return;
    const cmd = msg[4]!;
    const p = msg.subarray(5, msg.length - 1);
    this.received.push(cmd);
    if (cmd === CMD.uploadBegin || cmd === CMD.fwBegin) {
      this.staged = new Uint8Array(fromSeptets(p, cmd === CMD.fwBegin ? 0 : 1, 3));
      this.next = 0;
      this.active = true;
      this.reply(cmd, RESULT.ok, 0);
    } else if (cmd === CMD.uploadChunk || cmd === CMD.fwChunk) {
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
    } else if (cmd === CMD.fwCommit) {
      const reply = this.fwCommitCodes.length > 1 ? this.fwCommitCodes.shift()! : this.fwCommitCodes[0]!;
      if (reply === "ok") {
        this.active = false;
        this.reply(cmd, RESULT.ok, 654);
      } else {
        this.reply(cmd, reply[0], reply[1]);
      }
    } else if (cmd === CMD.read) {
      const off = fromSeptets(p, 1, 3);
      const n = this.reads.push(off) - 1;
      if (p[0] !== 0 || off >= this.flash.length) return this.reply(cmd, RESULT.badRequest, 0);
      const data = this.flash.subarray(off, Math.min(off + READ_BYTES, this.flash.length));
      if (!this.dropData(n))
        this.replies.push(pedalMsg(CMD.data, [0, ...septets(off, 3), ...septets(crc32(data), 5), ...pack7(data)]));
      this.reply(cmd, RESULT.ok, off + data.length);
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

const image = Uint8Array.from({ length: 16288 }, (_, i) => (i * 7 + 3) & 0xff);

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

test("read collects a run across READs", async () => {
  const pedal = new FakePedal();
  pedal.flash = image;
  const s = new PedalSession(pedal, { replyTimeoutMs: 10 });
  expect(await s.read(0, 150, 200)).toEqual(image.subarray(150, 350));
  expect(pedal.reads).toEqual([150, 310]);
});

test("a dropped DATA resends its READ", async () => {
  const pedal = new FakePedal();
  pedal.flash = image;
  pedal.dropData = (n) => n === 0;
  const s = new PedalSession(pedal, { replyTimeoutMs: 10 });
  expect(await s.read(0, 150, 200)).toEqual(image.subarray(150, 350));
  expect(pedal.reads).toEqual([150, 150, 310]);
});

test("updateFirmware stages the image byte-exact", async () => {
  const pedal = new FakePedal();
  const img = firmwareImage(5000);
  const s = new PedalSession(pedal, { replyTimeoutMs: 10 });
  expect(await s.updateFirmware(img)).toEqual({ flashMs: 654, retries: 0 });
  expect(pedal.staged).toEqual(img);
  expect(pedal.received.at(-1)).toBe(CMD.fwCommit);
});

test("a `flash` FIRMWARE COMMIT is resent and succeeds", async () => {
  const pedal = new FakePedal();
  pedal.fwCommitCodes = [[RESULT.flash, 0], "ok"];
  const s = new PedalSession(pedal, { replyTimeoutMs: 10 });
  expect(await s.updateFirmware(firmwareImage())).toEqual({ flashMs: 654, retries: 1 });
  expect(pedal.received.filter((c) => c === CMD.fwCommit).length).toBe(2);
});

test("an `invalid` FIRMWARE COMMIT names the failed check", async () => {
  const pedal = new FakePedal();
  pedal.fwCommitCodes = [[RESULT.invalid, 3]];
  const s = new PedalSession(pedal, { replyTimeoutMs: 10 });
  const err = await s.updateFirmware(firmwareImage()).catch((e: unknown) => e);
  expect(err).toBeInstanceOf(LinkError);
  expect((err as LinkError).message).toBe("FIRMWARE COMMIT: invalid (tag)");
  expect(pedal.received.filter((c) => c === CMD.fwCommit).length).toBe(1);
});

test("a file that is not a firmware image is refused before anything is sent", async () => {
  const pedal = new FakePedal();
  const s = new PedalSession(pedal, { replyTimeoutMs: 10 });
  await expect(s.updateFirmware(image)).rejects.toThrow("FIRMWARE: not a Ducky firmware image");
  expect(pedal.received).toEqual([]);
});
