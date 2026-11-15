// Convert .nam files to Ducky model binaries (docs/NAM_FORMAT.md "Ducky model binary (format 1)").
// Each IN.nam is written to IN.bin next to it.
//
// Usage: bun tools/nam_convert.ts IN.nam [IN2.nam ...]

import { type ConvertedModel, convertNam, ModelRejected } from "../editor/src/nam/convert";

const inputs = process.argv.slice(2);
if (inputs.length === 0) {
  console.error("usage: bun tools/nam_convert.ts IN.nam...");
  process.exit(2);
}

let failed = false;
for (const path of inputs) {
  const out = path.endsWith(".nam") ? `${path.slice(0, -4)}.bin` : `${path}.bin`;
  const fallbackName = (path.split("/").pop() ?? path).replace(/\.nam$/, "");
  let text: string;
  try {
    text = await Bun.file(path).text();
  } catch (e) {
    console.error(`nam_convert: ${path}: ${(e as Error).message}`);
    failed = true;
    continue;
  }
  let converted: ConvertedModel;
  try {
    converted = convertNam(text, fallbackName);
  } catch (e) {
    if (!(e instanceof ModelRejected)) throw e;
    console.error(`nam_convert: ${path}: rejected (${e.reason}): ${e.detail}`);
    failed = true;
    continue;
  }
  const { bin, info } = converted;
  await Bun.write(out, bin);
  const crc = new DataView(bin.buffer).getUint32(bin.length - 4, true);
  const loud = info.loudness === null ? "unknown" : `${info.loudness.toFixed(2)} dB`;
  const input = info.inputLevelDbu === null ? "unknown" : `${info.inputLevelDbu} dBu`;
  console.log(
    `${out}: "${info.name}" gear ${info.gearType ?? "-"} loudness ${loud} head_scale ${info.headScale} ` +
      `input ${input} ${bin.length} bytes crc 0x${crc.toString(16).padStart(8, "0")}`,
  );
  if (info.loudness === null) console.error(`nam_convert: ${path}: warning: no A2-Lite loudness, plays at unity`);
}
process.exit(failed ? 1 : 0);
