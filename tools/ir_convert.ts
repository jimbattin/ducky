// Convert .wav IRs to Ducky IR binaries (docs/FILTERS.md "IR binary (format 1)").
// Each IN.wav is written to IN.bin next to it.
//
// Usage: bun tools/ir_convert.ts IN.wav [IN2.wav ...]

import { type ConvertedIr, convertIr, IrRejected } from "../editor/src/ir/convert";

const inputs = process.argv.slice(2);
if (inputs.length === 0) {
  console.error("usage: bun tools/ir_convert.ts IN.wav...");
  process.exit(2);
}

let failed = false;
for (const path of inputs) {
  const out = /\.wav$/i.test(path) ? `${path.slice(0, -4)}.bin` : `${path}.bin`;
  const fallbackName = (path.split("/").pop() ?? path).replace(/\.wav$/i, "");
  let bytes: Uint8Array;
  try {
    bytes = new Uint8Array(await Bun.file(path).arrayBuffer());
  } catch (e) {
    console.error(`ir_convert: ${path}: ${(e as Error).message}`);
    failed = true;
    continue;
  }
  let converted: ConvertedIr;
  try {
    converted = convertIr(bytes, fallbackName);
  } catch (e) {
    if (!(e instanceof IrRejected)) throw e;
    console.error(`ir_convert: ${path}: rejected (${e.reason}): ${e.detail}`);
    failed = true;
    continue;
  }
  const { bin, info } = converted;
  await Bun.write(out, bin);
  const crc = new DataView(bin.buffer).getUint32(bin.length - 4, true);
  console.log(
    `${out}: "${info.name}" src ${info.sourceRate} Hz ${info.sourceChannels} ch ${info.sourceFrames} frames ` +
      `taps ${info.taps}${info.truncated ? " (truncated)" : ""} gain ${info.gainDb.toFixed(2)} dB ` +
      `${bin.length} bytes crc 0x${crc.toString(16).padStart(8, "0")}`,
  );
  if (info.sourceChannels > 1)
    console.error(`ir_convert: ${path}: warning: ${info.sourceChannels} channels, using channel 0`);
}
process.exit(failed ? 1 : 0);
