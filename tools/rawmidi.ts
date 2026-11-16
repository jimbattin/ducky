// The pedal's USB MIDI port as an ALSA rawmidi device (/dev/snd/midiC<N>D0), as a Transport for
// editor/src/link/session.ts. Linux only. The device opens exclusively: one program at a time.

import {
  closeSync,
  constants,
  existsSync,
  openSync,
  readdirSync,
  readFileSync,
  readSync,
  realpathSync,
  writeSync,
} from "node:fs";
import { basename, dirname, join } from "node:path";
import { SysExAssembler } from "../editor/src/link/protocol";
import type { Transport } from "../editor/src/link/session";

const SEED_VENDOR = "0483";
const SEED_PRODUCT = "5740";

export interface Seed {
  /** The USB path, e.g. 3-4 (bus 3, port 4). */
  usbPath: string;
  card: number;
  dev: string;
}

/** Every attached Daisy Seed with a rawmidi device, by sound card. */
export function findSeeds(): Seed[] {
  const root = "/sys/class/sound";
  const seeds: Seed[] = [];
  let entries: string[];
  try {
    entries = readdirSync(root);
  } catch {
    return seeds;
  }
  for (const entry of entries) {
    const m = /^card(\d+)$/.exec(entry);
    if (m === null) continue;
    try {
      const usbDevice = dirname(realpathSync(join(root, entry, "device"))); // device -> the USB interface dir
      const id = (f: string) => readFileSync(join(usbDevice, f), "utf8").trim();
      if (id("idVendor") !== SEED_VENDOR || id("idProduct") !== SEED_PRODUCT) continue;
      const dev = `/dev/snd/midiC${m[1]}D0`;
      if (existsSync(dev)) seeds.push({ usbPath: basename(usbDevice), card: Number(m[1]), dev });
    } catch {}
  }
  return seeds.sort((a, b) => a.card - b.card);
}

export interface RawMidi extends Transport {
  close(): void;
}

const code = (e: unknown) => (e as NodeJS.ErrnoException).code;
const sleep = (ms: number) => Bun.sleep(ms);

/**
 * Opens dev nonblocking and drops pending input (stale replies from earlier runs): the pedal's queued messages arrive
 * in the first milliseconds after the open, so input is read until it has been quiet for 20 ms (at most 500 ms).
 */
export async function openRawMidi(dev: string): Promise<RawMidi> {
  let fd: number;
  try {
    fd = openSync(dev, constants.O_RDWR | constants.O_NONBLOCK);
  } catch (e) {
    if (code(e) === "EBUSY")
      throw new Error(`${dev} busy: close other MIDI programs (browser Web MIDI, amidi, tools/usb_log.py)`);
    throw e;
  }
  const buf = new Uint8Array(1024);
  // The bytes read now, or 0 when none are pending.
  const readNow = (): number => {
    try {
      return readSync(fd, buf, 0, buf.length, null);
    } catch (e) {
      if (code(e) === "EAGAIN") return 0;
      throw e;
    }
  };
  const drainEnd = Date.now() + 500;
  for (let quietAt = Date.now() + 20; Date.now() < Math.min(quietAt, drainEnd); ) {
    if (readNow() > 0) quietAt = Date.now() + 20;
    else await sleep(1);
  }
  const assembler = new SysExAssembler();
  const pending: Uint8Array[] = [];

  return {
    async send(msg: Uint8Array): Promise<void> {
      const deadline = Date.now() + 1000;
      let off = 0;
      while (off < msg.length) {
        try {
          off += writeSync(fd, msg, off, msg.length - off);
        } catch (e) {
          if (code(e) !== "EAGAIN") throw e;
          if (Date.now() > deadline) throw new Error(`${dev}: write timed out`);
          await sleep(1);
        }
      }
    },
    async receive(timeoutMs: number): Promise<Uint8Array | null> {
      const deadline = Date.now() + timeoutMs;
      for (;;) {
        const queued = pending.shift();
        if (queued !== undefined) return queued;
        const n = readNow();
        if (n > 0) {
          pending.push(...assembler.feed(buf.subarray(0, n)));
          continue;
        }
        if (Date.now() >= deadline) return null;
        await sleep(1);
      }
    },
    close(): void {
      closeSync(fd);
    },
  };
}
