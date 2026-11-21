// Build a preset bank image (docs/FIRMWARE.md "Preset store"): up to 8 presets, each a model and an optional IR, as
// the 512 KB image DFU writes at 0x90400000. The bank replaces all 8 slots: slots not named are left empty.
// MODEL and IR are read as tools/slot_source.ts does (.nam and .wav converted, anything else read as a binary). --ir
// and --name apply to the most recent --slot; the name defaults to the model's.
//
// Usage: bun tools/preset_bank.ts -o OUT.bin --slot N MODEL [--ir IR] [--name NAME] [--slot N MODEL ...]

import { BANK_ADDRESS, buildBank, SLOT_COUNT } from "../editor/src/preset/bank";
import { loadSlotSource, SlotSourceError } from "./slot_source";

const USAGE = "usage: bun tools/preset_bank.ts -o OUT.bin --slot N MODEL [--ir IR] [--name NAME] [--slot N MODEL ...]";

function usage(problem: string): never {
  console.error(`preset_bank: ${problem}`);
  console.error(USAGE);
  process.exit(2);
}

interface SlotArgs {
  n: number; // 1-based
  model: string;
  ir: string | null;
  name: string | null;
}

const args = process.argv.slice(2);
let outArg: string | null = null;
const slots: SlotArgs[] = [];
for (let i = 0; i < args.length; i++) {
  const a = args[i]!;
  const value = (what: string): string => {
    const v = args[++i];
    if (v === undefined) usage(`${a} needs ${what}`);
    return v;
  };
  if (a === "-o") {
    outArg = value("OUT.bin");
  } else if (a === "--slot") {
    const nText = value("N");
    const n = Number(nText);
    if (!Number.isInteger(n) || n < 1 || n > SLOT_COUNT) usage(`slot ${nText}: must be 1..${SLOT_COUNT}`);
    if (slots.some((s) => s.n === n)) usage(`slot ${n} given twice`);
    slots.push({ n, model: value("MODEL"), ir: null, name: null });
  } else if (a === "--ir" || a === "--name") {
    const last = slots.at(-1);
    if (last === undefined) usage(`${a} before any --slot`);
    if (a === "--ir") last.ir = value("IR");
    else last.name = value("NAME");
  } else {
    usage(`unknown argument ${a}`);
  }
}
if (outArg === null) usage("missing -o OUT.bin");
const out = outArg;
if (slots.length === 0) usage("no --slot");

// Runs step for path; an error prints it and exits 1 (nothing written yet).
async function load<T>(path: string, step: () => Promise<T>): Promise<T> {
  try {
    return await step();
  } catch (e) {
    console.error(`preset_bank: ${path}: ${(e as Error).message}`);
    process.exit(1);
  }
}

const images: (Uint8Array | null)[] = Array(SLOT_COUNT).fill(null);
const lines: string[] = [];
for (const s of slots) {
  try {
    const { image, summary, warning } = await loadSlotSource(s.model, s.ir, s.name);
    if (warning !== null) console.error(`preset_bank: slot ${s.n}: warning: ${warning}`);
    images[s.n - 1] = image;
    lines.push(`slot ${s.n}: ${summary}`);
  } catch (e) {
    if (!(e instanceof SlotSourceError)) throw e;
    console.error(`preset_bank: ${e.path ?? `slot ${s.n}`}: ${e.message}`);
    process.exit(1);
  }
}
const bank = await load(out, async () => buildBank(images));
await load(out, () => Bun.write(out, bank));
for (const line of lines) console.log(line);
console.log(
  `${out}: ${bank.length} bytes; flash: tools/flash_when_ready.sh ${out} 0x${BANK_ADDRESS.toString(16).padStart(8, "0")}`,
);
