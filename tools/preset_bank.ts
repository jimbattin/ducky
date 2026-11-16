// Build a preset bank image (docs/FIRMWARE.md "Preset store"): up to 8 presets, each a model and an optional IR, as
// the 512 KB image DFU writes at 0x90400000. The bank replaces all 8 slots: slots not named are left empty.
// A MODEL ending .nam is converted (as tools/nam_convert.ts); any other path is read as a model binary. An IR ending
// .wav is converted (as tools/ir_convert.ts); any other path is read as an IR binary. --ir and --name apply to the
// most recent --slot; the name defaults to the model's.
//
// Usage: bun tools/preset_bank.ts -o OUT.bin --slot N MODEL [--ir IR] [--name NAME] [--slot N MODEL ...]

import { convertIr, IrRejected } from "../editor/src/ir/convert";
import { convertNam, ModelRejected } from "../editor/src/nam/convert";
import { BANK_ADDRESS, buildBank, buildSlot, SLOT_COUNT } from "../editor/src/preset/bank";

const USAGE = "usage: bun tools/preset_bank.ts -o OUT.bin --slot N MODEL [--ir IR] [--name NAME] [--slot N MODEL ...]";
const MODEL_OFF_NAME = 7584; // the model binary's name field (docs/NAM_FORMAT.md)

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

const basename = (path: string, ext: RegExp) => (path.split("/").pop() ?? path).replace(ext, "");
const readBytes = async (path: string) => new Uint8Array(await Bun.file(path).arrayBuffer());
const binName = (bin: Uint8Array, at: number) => {
  let s = "";
  for (let i = at; i < at + 64 && bin[i] !== 0 && bin[i] !== undefined; i++) s += String.fromCharCode(bin[i]!);
  return s;
};

// Runs step for path; on a rejection, a PresetError or a read error prints it and exits 1 (nothing written yet).
async function load<T>(path: string, step: () => Promise<T>): Promise<T> {
  try {
    return await step();
  } catch (e) {
    if (e instanceof ModelRejected || e instanceof IrRejected)
      console.error(`preset_bank: ${path}: rejected (${e.reason}): ${e.detail}`);
    else console.error(`preset_bank: ${path}: ${(e as Error).message}`);
    process.exit(1);
  }
}

const images: (Uint8Array | null)[] = Array(SLOT_COUNT).fill(null);
const lines: string[] = [];
for (const s of slots) {
  const { model, modelName, gearType } = await load(s.model, async () => {
    if (s.model.endsWith(".nam")) {
      const { bin, info } = convertNam(await Bun.file(s.model).text(), basename(s.model, /\.nam$/));
      return { model: bin, modelName: info.name, gearType: info.gearType };
    }
    const bin = await readBytes(s.model);
    return { model: bin, modelName: binName(bin, MODEL_OFF_NAME), gearType: null };
  });
  let ir: Uint8Array | null = null;
  let irName: string | null = null;
  if (s.ir !== null) {
    const path = s.ir;
    ({ ir, irName } = await load(path, async () => {
      if (/\.wav$/i.test(path)) {
        const { bin, info } = convertIr(await readBytes(path), basename(path, /\.wav$/i));
        return { ir: bin, irName: info.name };
      }
      const bin = await readBytes(path);
      return { ir: bin, irName: binName(bin, 32) }; // the IR binary's name field (docs/FILTERS.md)
    }));
    if (gearType?.toLowerCase().includes("cab"))
      console.error(`preset_bank: slot ${s.n}: warning: model gear type "${gearType}" already includes a cab`);
  }
  const slot = await load(`slot ${s.n}`, async () => buildSlot(model, ir, s.name ?? modelName));
  images[s.n - 1] = slot;
  const name = binName(slot, 12);
  const crc = new DataView(slot.buffer).getUint32(slot.length - 4, true);
  lines.push(
    `slot ${s.n}: "${name}" model "${modelName}" ir ${irName === null ? "none" : `"${irName}"`} ` +
      `${slot.length} bytes crc 0x${crc.toString(16).padStart(8, "0")}`,
  );
}
const bank = await load(out, async () => buildBank(images));
await load(out, () => Bun.write(out, bank));
for (const line of lines) console.log(line);
console.log(
  `${out}: ${bank.length} bytes; flash: tools/flash_when_ready.sh ${out} 0x${BANK_ADDRESS.toString(16).padStart(8, "0")}`,
);
