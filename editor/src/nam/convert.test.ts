import { describe, expect, test } from "bun:test";
import { convertNam, DILATIONS, KERNEL_SIZES, ModelRejected, type RejectReason } from "./convert";
import { crc32 } from "./crc32";

const FIXTURES = `${import.meta.dir}/../../../tests/fixtures/nam`;
const MODELS = `${import.meta.dir}/../../../models`;

const a2Text = await Bun.file(`${FIXTURES}/A2.nam`).text();

// The A2.nam fields the mutations touch (the fixture is known-good, so JSON.parse is cast to this view).
interface Layer {
  channels: number;
  bottleneck: number;
  kernel_sizes: number[];
  activation: { type: string }[];
  gating_mode: string[];
  conv_pre_film: { active: boolean };
}
interface Lite {
  sample_rate: number;
  weights: number[];
  metadata: { loudness?: number };
  config: { layers: Layer[] };
}
interface Root {
  metadata: { name?: string; loudness: number };
  config: { submodels: { model: Lite }[] };
}

function mutateA2(edit: (root: Root, lite: Lite, layer: Layer) => void): string {
  const root = JSON.parse(a2Text) as Root;
  const lite = root.config.submodels[0]!.model;
  edit(root, lite, lite.config.layers[0]!);
  return JSON.stringify(root);
}

function rejection(text: string): ModelRejected {
  try {
    convertNam(text, "x");
  } catch (e) {
    expect(e).toBeInstanceOf(ModelRejected);
    return e as ModelRejected;
  }
  throw new Error("model was accepted");
}

function storedName(bin: Uint8Array): string {
  const field = bin.subarray(7584, 7648);
  return new TextDecoder().decode(field.subarray(0, field.indexOf(0)));
}

test("crc32 check value", () => {
  expect(crc32(new TextEncoder().encode("123456789"))).toBe(0xcbf43926);
});

test("A2.nam converts to the format-1 layout", () => {
  const lite = (JSON.parse(a2Text) as Root).config.submodels[0]!.model;
  const { bin, info } = convertNam(a2Text, "A2");
  const dv = new DataView(bin.buffer);
  expect(bin.length).toBe(7652);
  expect(new TextDecoder().decode(bin.subarray(0, 4))).toBe("DKNM");
  expect(dv.getUint16(4, true)).toBe(1);
  expect(dv.getUint16(6, true)).toBe(3);
  expect(dv.getUint32(8, true)).toBe(7652);
  expect([...bin.subarray(12, 16)]).toEqual([3, 3, 23, 16]);
  expect([...bin.subarray(16, 39)]).toEqual([...KERNEL_SIZES]);
  expect(bin[39]).toBe(0);
  expect(Array.from({ length: 23 }, (_, i) => dv.getUint16(40 + 2 * i, true))).toEqual([...DILATIONS]);
  expect(dv.getUint16(86, true)).toBe(0);
  expect(dv.getUint32(88, true)).toBe(1870);
  expect(dv.getFloat32(92, true)).toBe(Math.fround(lite.weights[0]!));
  expect(dv.getFloat32(92 + 4 * 1869, true)).toBe(Math.fround(lite.weights[1869]!));
  expect(dv.getFloat32(7572, true)).toBe(Math.fround(lite.weights[1870]!));
  expect(dv.getFloat32(7576, true)).toBe(Math.fround(-24.038164138793945));
  expect(dv.getFloat32(7580, true)).toBe(Math.fround(14.76));
  expect(storedName(bin)).toBe("A2");
  expect(bin.subarray(7586, 7648).every((b) => b === 0)).toBe(true);
  expect(dv.getUint32(7648, true)).toBe(crc32(bin.subarray(0, 7648)));
  expect(info.name).toBe("A2");
  expect(info.headScale).toBe(lite.weights[1870]!);
});

test("A2.nam converts to the committed A2.bin", async () => {
  const committed = new Uint8Array(await Bun.file(`${FIXTURES}/A2.bin`).arrayBuffer());
  expect(convertNam(a2Text, "A2").bin).toEqual(committed);
});

describe("rejections", () => {
  const cases: [string, () => string | Promise<string>, RejectReason, string?][] = [
    ["truncated JSON", () => "{", "not-json"],
    ["slimmable_container.nam", () => Bun.file(`${FIXTURES}/slimmable_container.nam`).text(), "no-lite-submodel"],
    ["wavenet_a2_feature_test.nam", () => Bun.file(`${FIXTURES}/wavenet_a2_feature_test.nam`).text(), "not-container"],
    ["A2-Full only", () => mutateA2((root) => root.config.submodels.shift()), "no-lite-submodel"],
    [
      "bare A2-Full WaveNet",
      () => JSON.stringify((JSON.parse(a2Text) as Root).config.submodels[1]!.model),
      "not-container",
    ],
    [
      "8-channel lite",
      () =>
        mutateA2((_, _lite, layer) => {
          layer.channels = 8;
          layer.bottleneck = 8;
        }),
      "shape",
      "A2-Full",
    ],
    ["44.1 kHz", () => mutateA2((_, lite) => (lite.sample_rate = 44100)), "sample-rate"],
    ["kernel size", () => mutateA2((_, _lite, layer) => (layer.kernel_sizes[14] = 6)), "shape", "kernel_sizes"],
    ["Tanh activation", () => mutateA2((_, _lite, layer) => (layer.activation[3]!.type = "Tanh")), "shape"],
    ["gated layer", () => mutateA2((_, _lite, layer) => (layer.gating_mode[0] = "gated")), "shape"],
    ["active FiLM", () => mutateA2((_, _lite, layer) => (layer.conv_pre_film.active = true)), "shape"],
    ["short weights", () => mutateA2((_, lite) => lite.weights.pop()), "weights"],
    // NaN serialises as null.
    ["null weight", () => mutateA2((_, lite) => (lite.weights[5] = Number.NaN)), "weights"],
  ];
  for (const [label, text, reason, detail] of cases) {
    test(label, async () => {
      const e = rejection(await text());
      expect(e.reason).toBe(reason);
      if (detail) expect(e.detail).toContain(detail);
    });
  }
});

test("missing lite loudness converts at unity and ignores the A2-Full loudness", () => {
  const text = mutateA2((root, lite) => {
    delete lite.metadata.loudness;
    expect(root.metadata.loudness).toBeNumber();
  });
  const { bin, info } = convertNam(text, "A2");
  const dv = new DataView(bin.buffer);
  expect(dv.getUint16(6, true) & 1).toBe(0);
  expect(dv.getFloat32(7576, true)).toBe(0);
  expect(info.loudness).toBeNull();
});

describe("name", () => {
  test("non-ASCII code points become ?", () => {
    const { bin, info } = convertNam(
      mutateA2((root) => (root.metadata.name = "Ünïcode amp")),
      "A2",
    );
    expect(info.name).toBe("?n?code amp");
    expect(storedName(bin)).toBe("?n?code amp");
  });
  test("long names truncate to 63 characters", () => {
    const { bin, info } = convertNam(
      mutateA2((root) => (root.metadata.name = "x".repeat(80))),
      "A2",
    );
    expect(info.name).toBe("x".repeat(63));
    expect(storedName(bin)).toBe("x".repeat(63));
    expect(bin[7584 + 63]).toBe(0);
  });
});

// The development models are git-ignored; their tests skip when absent.
const fender = `${MODELS}/fender_super_eob_with_cab.nam`;
const sss = `${MODELS}/sss_with_cab_clean.nam`;
const haveFender = await Bun.file(fender).exists();
const haveSss = await Bun.file(sss).exists();

describe("development models", () => {
  test.skipIf(!haveFender)("fender_super_eob_with_cab.nam", async () => {
    const { bin, info } = convertNam(await Bun.file(fender).text(), "fender_super_eob_with_cab");
    const dv = new DataView(bin.buffer);
    expect(info.name).toBe("FNDR BFSR VB Edge WRM2 CAB FREE");
    expect(dv.getFloat32(7576, true)).toBe(Math.fround(-15.903149));
    expect(dv.getFloat32(7572, true)).toBe(Math.fround(0.011273000174243054));
  });

  test.skipIf(!haveSss)("sss_with_cab_clean.nam", async () => {
    const { bin, info } = convertNam(await Bun.file(sss).text(), "sss_with_cab_clean");
    expect(info.name).toBe("AATR SX3 Clean BAL3 CAB");
    expect(new DataView(bin.buffer).getFloat32(7576, true)).toBe(Math.fround(-21.709897));
  });
});
