// Writes tests/fixtures/preset/A2_decay.bin: the preset of tests/fixtures/nam/A2.bin with tests/fixtures/ir/decay.bin,
// named "A2 + decay". The host tests (tests/preset_test.cpp) and the editor (editor/src/preset/bank.test.ts) both check
// it, which pins the slot format across the two languages.
//
// Usage: bun tools/make_preset_fixture.ts

import { mkdir } from "node:fs/promises";
import { buildSlot } from "../editor/src/preset/bank";

const fixtures = `${import.meta.dir}/../tests/fixtures`;
const out = `${fixtures}/preset/A2_decay.bin`;

const model = new Uint8Array(await Bun.file(`${fixtures}/nam/A2.bin`).arrayBuffer());
const ir = new Uint8Array(await Bun.file(`${fixtures}/ir/decay.bin`).arrayBuffer());
const slot = buildSlot(model, ir, "A2 + decay");
await mkdir(`${fixtures}/preset`, { recursive: true });
await Bun.write(out, slot);
const crc = new DataView(slot.buffer).getUint32(slot.length - 4, true);
console.log(`${out}: ${slot.length} bytes crc 0x${crc.toString(16).padStart(8, "0")}`);
