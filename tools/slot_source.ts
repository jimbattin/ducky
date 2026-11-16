// One preset slot image from a model and an optional IR, for tools/preset_bank.ts and tools/pedal.ts. A MODEL ending
// .nam is converted (as tools/nam_convert.ts); any other path is read as a model binary. An IR ending .wav is converted
// (as tools/ir_convert.ts); any other path is read as an IR binary. The name defaults to the model's.

import { convertIr, IrRejected } from "../editor/src/ir/convert";
import { convertNam, ModelRejected } from "../editor/src/nam/convert";
import { buildSlot } from "../editor/src/preset/bank";

const MODEL_OFF_NAME = 7584; // the model binary's name field (docs/NAM_FORMAT.md)
const IR_OFF_NAME = 32; // the IR binary's name field (docs/FILTERS.md)
const SLOT_OFF_NAME = 12; // the slot's name field (docs/FIRMWARE.md "Preset store")

/** A source that could not be read, converted or built; path is null for the slot build itself. */
export class SlotSourceError extends Error {
  constructor(
    readonly path: string | null,
    message: string,
  ) {
    super(message);
    this.name = "SlotSourceError";
  }
}

export interface SlotSource {
  image: Uint8Array;
  /** `"NAME" model "M" ir "I"|none N bytes crc 0x…`. */
  summary: string;
  /** Set when the model's gear type already includes a cab and an IR was given. */
  warning: string | null;
}

const basename = (path: string, ext: RegExp) => (path.split("/").pop() ?? path).replace(ext, "");
const readBytes = async (path: string) => new Uint8Array(await Bun.file(path).arrayBuffer());
const binName = (bin: Uint8Array, at: number) => {
  let s = "";
  for (let i = at; i < at + 64 && bin[i] !== 0 && bin[i] !== undefined; i++) s += String.fromCharCode(bin[i]!);
  return s;
};

// Runs step; a rejection or any other error becomes a SlotSourceError for path.
async function load<T>(path: string | null, step: () => Promise<T>): Promise<T> {
  try {
    return await step();
  } catch (e) {
    if (e instanceof ModelRejected || e instanceof IrRejected)
      throw new SlotSourceError(path, `rejected (${e.reason}): ${e.detail}`);
    throw new SlotSourceError(path, (e as Error).message);
  }
}

export async function loadSlotSource(model: string, ir: string | null, name: string | null): Promise<SlotSource> {
  const m = await load(model, async () => {
    if (model.endsWith(".nam")) {
      const { bin, info } = convertNam(await Bun.file(model).text(), basename(model, /\.nam$/));
      return { bin, modelName: info.name, gearType: info.gearType };
    }
    const bin = await readBytes(model);
    return { bin, modelName: binName(bin, MODEL_OFF_NAME), gearType: null };
  });
  let irBin: Uint8Array | null = null;
  let irName: string | null = null;
  let warning: string | null = null;
  if (ir !== null) {
    ({ irBin, irName } = await load(ir, async () => {
      if (/\.wav$/i.test(ir)) {
        const { bin, info } = convertIr(await readBytes(ir), basename(ir, /\.wav$/i));
        return { irBin: bin, irName: info.name };
      }
      const bin = await readBytes(ir);
      return { irBin: bin, irName: binName(bin, IR_OFF_NAME) };
    }));
    if (m.gearType?.toLowerCase().includes("cab")) warning = `model gear type "${m.gearType}" already includes a cab`;
  }
  const image = await load(null, async () => buildSlot(m.bin, irBin, name ?? m.modelName));
  const crc = new DataView(image.buffer).getUint32(image.length - 4, true);
  const summary =
    `"${binName(image, SLOT_OFF_NAME)}" model "${m.modelName}" ir ${irName === null ? "none" : `"${irName}"`} ` +
    `${image.length} bytes crc 0x${crc.toString(16).padStart(8, "0")}`;
  return { image, summary, warning };
}
