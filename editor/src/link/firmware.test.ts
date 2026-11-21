import { expect, test } from "bun:test";
import { firmwareImage } from "../testFixtures";
import { firmwareKind, firmwareProblem } from "./firmware";

test("a tagged SRAM image is accepted", () => {
  expect(firmwareProblem(firmwareImage())).toBeNull();
  expect(firmwareProblem(firmwareImage(1040))).toBeNull();
  expect(firmwareProblem(firmwareImage(491520))).toBeNull();
});

test("an image outside 1040-491520 bytes is refused by size", () => {
  expect(firmwareProblem(new Uint8Array(1039))).toBe("1039 bytes: a firmware image is 1040-491520 bytes");
  expect(firmwareProblem(firmwareImage(491521))).toBe("491521 bytes: a firmware image is 1040-491520 bytes");
});

test("a vector table that is not an SRAM image's is refused", () => {
  const img = firmwareImage();
  new DataView(img.buffer).setUint32(0, 0x30000000, true);
  expect(firmwareProblem(img)).toBe("not a Ducky firmware image (its vector table is not an SRAM image's)");
});

test("an image without the DKFW tag is refused", () => {
  const img = firmwareImage();
  img[0x400] = img[0x400]! ^ 0xff;
  expect(firmwareProblem(img)).toContain("no DKFW tag at 0x400");
});

test("the tag's flags give the image's morph", () => {
  expect(firmwareKind(firmwareImage(4096, 0))).toBe("crossfade");
  expect(firmwareKind(firmwareImage(4096, 1))).toBe("weight");
});
