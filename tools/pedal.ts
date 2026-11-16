// Manage the pedal's preset slots over its USB MIDI link (docs/MIDI.md) while it plays: list, upload, erase, rename
// and swap slots, and stream its status. Slots are 1-8. MODEL and IR are read as tools/slot_source.ts does (.nam and
// .wav converted, anything else read as a binary); the name defaults to the model's. The Seed is found by its USB
// path (--port, else DK_USB_PORT, else the only Seed attached); its Identity must report a Ducky (device 64).
// Linux (ALSA rawmidi, tools/rawmidi.ts); the MIDI port opens exclusively, so close other MIDI programs first.
//
// Usage: bun tools/pedal.ts [--port BUS-PORT] COMMAND
//   list                                     the 8 slots (* marks the playing one)
//   upload N MODEL [--ir IR] [--name NAME]   write a preset to slot N
//   erase N | rename N NAME | swap A B
//   info N                                   the preset in slot N: its name, model metadata and IR (read back)
//   status [SECONDS]                         STATUS and LOG lines for SECONDS (default 5) or until Ctrl-C

import { SLOT_STATUS_NAMES, type StatusMessage } from "../editor/src/link/protocol";
import { LinkError, PedalSession } from "../editor/src/link/session";
import { findSeeds, openRawMidi } from "./rawmidi";
import { loadSlotSource, SlotSourceError } from "./slot_source";

const USAGE = `usage: bun tools/pedal.ts [--port BUS-PORT] COMMAND
  list
  upload N MODEL [--ir IR] [--name NAME]
  erase N
  info N
  rename N NAME
  swap A B
  status [SECONDS]`;

function usage(problem: string): never {
  console.error(`pedal: ${problem}`);
  console.error(USAGE);
  process.exit(2);
}

function fail(message: string): never {
  console.error(`pedal: ${message}`);
  process.exit(1);
}

// Arguments: --port anywhere; upload's --ir and --name anywhere after the command.
const args = process.argv.slice(2);
let port = process.env.DK_USB_PORT ?? null;
let irArg: string | null = null;
let nameArg: string | null = null;
const words: string[] = [];
for (let i = 0; i < args.length; i++) {
  const a = args[i]!;
  const value = (what: string): string => {
    const v = args[++i];
    if (v === undefined) usage(`${a} needs ${what}`);
    return v;
  };
  if (a === "--port") port = value("BUS-PORT");
  else if (a === "--ir") irArg = value("IR");
  else if (a === "--name") nameArg = value("NAME");
  else if (a.startsWith("--")) usage(`unknown option ${a}`);
  else words.push(a);
}
const [command, ...rest] = words;
if (command === undefined) usage("no command");
if ((irArg !== null || nameArg !== null) && command !== "upload") usage("--ir and --name are for upload");

const arity: Record<string, [number, number]> = {
  list: [0, 0],
  upload: [2, 2],
  erase: [1, 1],
  info: [1, 1],
  rename: [2, 2],
  swap: [2, 2],
  status: [0, 1],
};
const counts = arity[command];
if (counts === undefined) usage(`unknown command ${command}`);
if (rest.length < counts[0] || rest.length > counts[1]) usage(`${command}: wrong number of arguments`);

/** A 1-based slot argument as 0-based. */
function slotArg(text: string): number {
  const n = Number(text);
  if (!Number.isInteger(n) || n < 1 || n > 8) usage(`slot ${text}: must be 1..8`);
  return n - 1;
}

let seconds = 5;
if (command === "status" && rest[0] !== undefined) {
  seconds = Number(rest[0]);
  if (!Number.isFinite(seconds) || seconds <= 0) usage(`status: ${rest[0]}: must be a positive number of seconds`);
}
const slots =
  command === "upload" || command === "erase" || command === "rename" || command === "info" ? [slotArg(rest[0]!)] : [];
if (command === "swap") slots.push(slotArg(rest[0]!), slotArg(rest[1]!));

// The upload's image, before touching the pedal.
const source =
  command === "upload"
    ? await loadSlotSource(rest[1]!, irArg, nameArg).catch((e: unknown) => {
        if (e instanceof SlotSourceError) fail(`${e.path ?? `slot ${slots[0]! + 1}`}: ${e.message}`);
        throw e;
      })
    : null;
if (source?.warning) console.error(`pedal: slot ${slots[0]! + 1}: warning: ${source.warning}`);

const seeds = findSeeds();
if (port === null && seeds.length > 1)
  usage(`${seeds.length} Seeds attached (${seeds.map((s) => s.usbPath).join(", ")}); pass --port`);
const seed = port === null ? seeds[0] : seeds.find((s) => s.usbPath === port);
if (seed === undefined) fail(`no Seed MIDI port${port === null ? "" : ` at ${port}`} (is tools/usb_log.py running?)`);

const nameOf = (image: Uint8Array): string => {
  let s = "";
  for (let i = 12; i < 76 && image[i] !== 0; i++) s += String.fromCharCode(image[i]!);
  return s;
};

const pct = (permille: number) => (permille / 10).toFixed(1);
const statusLine = (s: StatusMessage): string =>
  `status load ${s.loadAvg}/${s.loadMax} permille underruns ${s.underruns} slot ${s.slot === null ? "dry" : s.slot + 1} ` +
  `mute ${s.muted ? "on" : "off"} k ${s.knobs.map(pct).join(" ")} sw ${s.toggles.join(" ")} ` +
  `fs ${s.fs1 ? 1 : 0} ${s.fs2 ? 1 : 0}`;
const signed = (v: number | null, unit: string): string =>
  v === null ? "none" : `${v >= 0 ? "+" : "-"}${Math.abs(v).toFixed(1)} ${unit}`;
const quoted = (s: string | null): string => (s === null ? "none" : `"${s}"`);

const midi = await openRawMidi(seed.dev).catch((e: unknown) => fail((e as Error).message));
try {
  const session = new PedalSession(midi, command === "status" ? { onLog: (text) => console.log(text) } : {});
  await session.identity().catch(() => fail(`no Ducky on ${seed.usbPath}`));
  switch (command) {
    case "list":
      for (const s of await session.list()) {
        const status = SLOT_STATUS_NAMES[s.status] ?? String(s.status);
        const detail = s.status === 0 ? ` "${s.name}" model "${s.model}" ir ${s.hasIr ? `"${s.ir}"` : "none"}` : "";
        console.log(`${s.slot + 1} ${status}${detail}${s.playing ? " *" : ""}`);
      }
      break;
    case "upload": {
      const image = source!.image;
      const t0 = performance.now();
      const r = await session.upload(slots[0]!, image);
      const secs = ((performance.now() - t0) / 1000).toFixed(1);
      console.log(
        `upload slot ${slots[0]! + 1} "${nameOf(image)}" ${image.length} bytes: ok, flash ${r.flashMs} ms, ` +
          `${r.retries} retries, ${secs} s`,
      );
      break;
    }
    case "erase":
      console.log(`erase slot ${slots[0]! + 1}: ok, flash ${await session.erase(slots[0]!)} ms`);
      break;
    case "rename":
      console.log(
        `rename slot ${slots[0]! + 1} "${rest[1]}": ok, flash ${await session.rename(slots[0]!, rest[1]!)} ms`,
      );
      break;
    case "info": {
      const k = slots[0]!;
      const status = (await session.list())[k]!.status;
      if (status !== 0) fail(`slot ${k + 1}: ${SLOT_STATUS_NAMES[status] ?? status}`);
      const { name, model: m, ir } = await session.readInfo(k);
      console.log(`slot ${k + 1} "${name}"`);
      console.log(
        `model "${m.name}" gear type ${m.gearType ?? "none"} make ${quoted(m.gearMake)} model ${quoted(m.gearModel)} ` +
          `modeled by ${quoted(m.modeledBy)}`,
      );
      console.log(`model loudness ${signed(m.loudness, "dB")} input ${signed(m.inputLevelDbu, "dBu")}`);
      console.log(
        ir === null
          ? "ir none"
          : `ir "${ir.name}" taps ${ir.taps} src ${ir.sourceRate} Hz ${ir.sourceChannels} ch ${ir.sourceFrames} ` +
              `frames gain ${signed(ir.gainDb, "dB")}`,
      );
      break;
    }
    case "swap":
      console.log(
        `swap slots ${slots[0]! + 1} ${slots[1]! + 1}: ok, flash ${await session.swap(slots[0]!, slots[1]!)} ms`,
      );
      break;
    case "status": {
      let stop = false;
      process.on("SIGINT", () => {
        stop = true;
      });
      const deadline = Date.now() + seconds * 1000;
      await session.watch(true);
      try {
        while (!stop && Date.now() < deadline) {
          const m = await session.next(Math.min(100, deadline - Date.now()));
          if (m?.type === "status") console.log(statusLine(m));
          else if (m?.type === "log") console.log(m.text);
        }
      } finally {
        await session.watch(false);
      }
      break;
    }
  }
} catch (e) {
  if (e instanceof LinkError) fail(e.message);
  throw e;
} finally {
  midi.close();
}
