// A firmware image for FIRMWARE BEGIN / CHUNK / COMMIT (daisy/src/fw_image.h; docs/MIDI.md "Firmware update"): the
// host's checks before anything is sent. The pedal repeats them (and checks the CRC and the board) at COMMIT. Pure
// TypeScript: shared by the editor and tools/pedal.ts.

import type { MorphKind } from "./protocol";

/** fw_image.h's constants. */
const FIRMWARE_MIN_BYTES = 0x410;
const FIRMWARE_MAX_BYTES = 0x78000;
const SRAM_BASE = 0x24000000;
const TAG_OFFSET = 0x400;
const TAG_MAGIC = 0x57464b44; // "DKFW"
const TAG_VERSION = 1;
const STACK_LO = 0x20000000;
const STACK_HI = 0x20020000;
/** The tag's flags, bit 0: the Identity Reply's weight-morph flag (link.h kIdentityWeightMorph). */
const FLAG_WEIGHT_MORPH = 0x01;

const le32 = (b: Uint8Array, at: number): number => new DataView(b.buffer, b.byteOffset + at, 4).getUint32(0, true);

/** Why image is not a Ducky firmware image the pedal would take, or null. The pedal checks the board. */
export function firmwareProblem(image: Uint8Array): string | null {
  const n = image.length;
  if (n < FIRMWARE_MIN_BYTES || n > FIRMWARE_MAX_BYTES)
    return `${n} bytes: a firmware image is ${FIRMWARE_MIN_BYTES}-${FIRMWARE_MAX_BYTES} bytes`;
  const sp = le32(image, 0);
  const reset = le32(image, 4);
  const entry = (reset & ~1) >>> 0;
  if (sp < STACK_LO || sp > STACK_HI || (reset & 1) === 0 || entry < SRAM_BASE || entry >= SRAM_BASE + n)
    return "not a Ducky firmware image (its vector table is not an SRAM image's)";
  if (le32(image, TAG_OFFSET) !== TAG_MAGIC || le32(image, TAG_OFFSET + 4) !== TAG_VERSION)
    return "not a Ducky firmware image (no DKFW tag at 0x400; images built before firmware updates have none)";
  return null;
}

/** The morph the image plays, from its tag's flags. Only for an image firmwareProblem accepts. */
export function firmwareKind(image: Uint8Array): MorphKind {
  return (le32(image, TAG_OFFSET + 12) & FLAG_WEIGHT_MORPH) !== 0 ? "weight" : "crossfade";
}
