// .nam (JSON) → Ducky model binary, format 2. Spec: docs/NAM_FORMAT.md "Ducky model binary (format 2)".
// Pure TypeScript (no DOM, no Bun APIs): shared by the editor and tools/nam_convert.ts.

import { crc32 } from "../crc32";
import { SAMPLE_RATE, sanitiseName, writeAscii } from "../device";

export const MODEL_MAGIC = "DKNM";
const MODEL_FORMAT_VERSION = 2;
export const MODEL_BIN_SIZE = 7908;
const LITE_WEIGHT_COUNT = 1871; // NAM stream incl. trailing head_scale
const NETWORK_WEIGHT_COUNT = 1870; // stored weights, head_scale split out
export const FLAG_HAS_LOUDNESS = 0x0001;
export const FLAG_HAS_INPUT_LEVEL = 0x0002;

const CHANNELS = 3;
const NUM_LAYERS = 23;
const HEAD_KERNEL = 16;

export const KERNEL_SIZES: readonly number[] = [...Array(14).fill(6), 15, 15, ...Array(7).fill(6)];
const DIL7 = [1, 3, 7, 17, 41, 101, 239];
export const DILATIONS: readonly number[] = [...DIL7, ...DIL7, 1, 13, ...DIL7];

// Byte offsets in the binary.
export const OFF_FLAGS = 6;
const OFF_SIZE = 8;
const OFF_DESC = 12;
const OFF_KERNELS = 16;
const OFF_DILATIONS = 40;
const OFF_WEIGHT_COUNT = 88;
const OFF_WEIGHTS = 92;
const OFF_HEAD_SCALE = OFF_WEIGHTS + 4 * NETWORK_WEIGHT_COUNT; // 7572
export const OFF_LOUDNESS = OFF_HEAD_SCALE + 4;
export const OFF_INPUT_LEVEL = OFF_LOUDNESS + 4;
export const OFF_NAME = OFF_INPUT_LEVEL + 4; // 7584
export const OFF_GEAR_TYPE = OFF_NAME + 64; // 7648
export const OFF_GEAR_MAKE = OFF_GEAR_TYPE + 64;
export const OFF_GEAR_MODEL = OFF_GEAR_MAKE + 64;
export const OFF_MODELED_BY = OFF_GEAR_MODEL + 64;
export const OFF_CRC = OFF_MODELED_BY + 64; // 7904

const FILM_KEYS = [
  "conv_pre_film",
  "conv_post_film",
  "input_mixin_pre_film",
  "input_mixin_post_film",
  "activation_pre_film",
  "activation_post_film",
  "layer1x1_post_film",
  "head1x1_post_film",
] as const;

export type RejectReason =
  | "not-json"
  | "not-container"
  | "no-lite-submodel"
  | "not-wavenet"
  | "sample-rate"
  | "shape"
  | "weights";

export class ModelRejected extends Error {
  constructor(
    readonly reason: RejectReason,
    readonly detail: string,
  ) {
    super(`${reason}: ${detail}`);
    this.name = "ModelRejected";
  }
}

export interface ModelInfo {
  name: string; // exactly the stored (sanitised) name
  gearType: string | null; // exactly the stored (sanitised) text; null when absent or blank
  gearMake: string | null; // likewise
  gearModel: string | null; // likewise
  modeledBy: string | null; // likewise
  loudness: number | null; // A2-Lite submodel metadata.loudness (dB)
  inputLevelDbu: number | null; // top-level metadata
  outputLevelDbu: number | null; // top-level metadata
  headScale: number; // the Lite stream's last float
}

export interface ConvertedModel {
  bin: Uint8Array;
  info: ModelInfo;
}

type Obj = Record<string, unknown>;

function isObj(v: unknown): v is Obj {
  return typeof v === "object" && v !== null && !Array.isArray(v);
}

function show(v: unknown): string {
  return v === undefined ? "absent" : JSON.stringify(v);
}

function shape(path: string, found: unknown, need: string): never {
  throw new ModelRejected("shape", `${path} is ${show(found)}, need ${need}`);
}

function absentOr(v: unknown, ok: unknown): boolean {
  return v === undefined || v === ok;
}

function absentOrNull(v: unknown): boolean {
  return v === undefined || v === null;
}

function finiteOrNull(v: unknown): number | null {
  return typeof v === "number" && Number.isFinite(v) ? v : null;
}

// A stored text field: null when absent or blank, else trimmed and sanitised.
function textOrNull(v: unknown): string | null {
  return typeof v === "string" && v.trim() !== "" ? sanitiseName(v.trim()) : null;
}

function checkIntArray(path: string, v: unknown, want: readonly number[]): void {
  if (!Array.isArray(v) || v.length !== want.length) shape(path, v, `an array of ${want.length}`);
  for (let i = 0; i < want.length; i++) {
    if (v[i] !== want[i]) shape(`${path}[${i}]`, v[i], `${want[i]}`);
  }
}

// Optional per-layer array: absent, null, or NUM_LAYERS entries that all equal `each`.
function checkUniformArray(path: string, v: unknown, each: unknown): void {
  if (absentOrNull(v)) return;
  if (!Array.isArray(v) || v.length !== NUM_LAYERS) shape(path, v, `absent, null, or an array of ${NUM_LAYERS}`);
  for (let i = 0; i < NUM_LAYERS; i++) {
    if (v[i] !== each) shape(`${path}[${i}]`, v[i], show(each));
  }
}

// Mirrors upstream is_a2_shape (NAM/wavenet/a2_fast.cpp at d3189ed), plus channels == 3.
function checkShape(c: Obj): void {
  if (!Array.isArray(c.layers) || c.layers.length !== 1) shape("layers", c.layers, "an array of 1");
  if (!absentOrNull(c.head)) shape("head", c.head, "absent or null");
  if (!absentOrNull(c.condition_dsp)) shape("condition_dsp", c.condition_dsp, "absent or null");
  if (typeof c.head_scale !== "number") shape("head_scale", c.head_scale, "a number");
  if (!absentOr(c.in_channels, 1)) shape("in_channels", c.in_channels, "absent or 1");

  const la: unknown = c.layers[0];
  const p = "layers[0]";
  if (!isObj(la)) shape(p, la, "an object");
  if (la.input_size !== 1) shape(`${p}.input_size`, la.input_size, "1");
  if (la.condition_size !== 1) shape(`${p}.condition_size`, la.condition_size, "1");
  if (la.channels !== CHANNELS) shape(`${p}.channels`, la.channels, la.channels === 8 ? "3 (A2-Full)" : "3");
  if (la.bottleneck !== CHANNELS) shape(`${p}.bottleneck`, la.bottleneck, "3");
  checkIntArray(`${p}.kernel_sizes`, la.kernel_sizes, KERNEL_SIZES);
  checkIntArray(`${p}.dilations`, la.dilations, DILATIONS);

  const act = la.activation;
  if (!Array.isArray(act) || act.length !== NUM_LAYERS) shape(`${p}.activation`, act, `an array of ${NUM_LAYERS}`);
  for (let i = 0; i < NUM_LAYERS; i++) {
    const a: unknown = act[i];
    const ap = `${p}.activation[${i}]`;
    if (!isObj(a)) shape(ap, a, "an object");
    if (a.type !== "LeakyReLU") shape(`${ap}.type`, a.type, '"LeakyReLU"');
    const s = a.negative_slope;
    if (typeof s !== "number" || !(Math.abs(Math.fround(s) - Math.fround(0.01)) <= 1e-7))
      shape(`${ap}.negative_slope`, s, "0.01");
  }

  checkUniformArray(`${p}.gating_mode`, la.gating_mode, "none");
  if (la.gated === true) shape(`${p}.gated`, la.gated, "not true");
  checkUniformArray(`${p}.secondary_activation`, la.secondary_activation, null);

  if (isObj(la.head1x1) && la.head1x1.active) shape(`${p}.head1x1.active`, la.head1x1.active, "falsy");

  const l1 = la.layer1x1;
  if (!isObj(l1)) shape(`${p}.layer1x1`, l1, "an object");
  if (l1.active !== true) shape(`${p}.layer1x1.active`, l1.active, "true");
  if (!absentOr(l1.groups, 1)) shape(`${p}.layer1x1.groups`, l1.groups, "absent or 1");

  const h = la.head;
  if (!isObj(h)) shape(`${p}.head`, h, "an object");
  if (h.out_channels !== 1) shape(`${p}.head.out_channels`, h.out_channels, "1");
  if (h.kernel_size !== HEAD_KERNEL) shape(`${p}.head.kernel_size`, h.kernel_size, `${HEAD_KERNEL}`);
  if (!absentOr(h.head_dilation, 1)) shape(`${p}.head.head_dilation`, h.head_dilation, "absent or 1");
  if (h.bias !== true) shape(`${p}.head.bias`, h.bias, "true");

  for (const key of FILM_KEYS) {
    const f = la[key];
    if (f === undefined || f === null || f === false) continue;
    if (isObj(f) && !f.active) continue;
    shape(`${p}.${key}`, f, "inactive");
  }

  if (!absentOr(la.groups_input, 1)) shape(`${p}.groups_input`, la.groups_input, "absent or 1");
  if (!absentOr(la.groups_input_mixin, 1)) shape(`${p}.groups_input_mixin`, la.groups_input_mixin, "absent or 1");
  if (!absentOrNull(la.slimmable)) shape(`${p}.slimmable`, la.slimmable, "absent or null");
}

function checkWeights(w: unknown): number[] {
  if (!Array.isArray(w)) throw new ModelRejected("weights", `weights is ${show(w)}, need an array`);
  if (w.length !== LITE_WEIGHT_COUNT)
    throw new ModelRejected("weights", `count is ${w.length}, need ${LITE_WEIGHT_COUNT}`);
  for (let i = 0; i < w.length; i++) {
    const x: unknown = w[i];
    if (typeof x !== "number" || !Number.isFinite(x))
      throw new ModelRejected("weights", `weights[${i}] is not a finite number`);
  }
  return w as number[];
}

export function convertNam(text: string, fallbackName: string): ConvertedModel {
  let root: unknown;
  try {
    root = JSON.parse(text);
  } catch (e) {
    throw new ModelRejected("not-json", (e as Error).message);
  }
  if (!isObj(root)) throw new ModelRejected("not-json", "top level is not an object");
  if (root.architecture !== "SlimmableContainer")
    throw new ModelRejected(
      "not-container",
      `architecture is ${typeof root.architecture === "string" ? root.architecture : show(root.architecture)}`,
    );

  const subs = isObj(root.config) ? root.config.submodels : undefined;
  if (!Array.isArray(subs)) throw new ModelRejected("no-lite-submodel", `config.submodels is ${show(subs)}`);
  const entry: unknown = subs.find((s) => isObj(s) && s.max_value === 0.5);
  if (!isObj(entry)) {
    const maxes = subs.map((s) => (isObj(s) ? s.max_value : undefined));
    throw new ModelRejected("no-lite-submodel", `config.submodels[].max_value is ${show(maxes)}, need one 0.5`);
  }
  const lite = entry.model;
  if (!isObj(lite) || lite.architecture !== "WaveNet")
    throw new ModelRejected(
      "not-wavenet",
      `lite model architecture is ${show(isObj(lite) ? lite.architecture : lite)}, need "WaveNet"`,
    );
  if (lite.sample_rate !== SAMPLE_RATE)
    throw new ModelRejected("sample-rate", `sample_rate is ${show(lite.sample_rate)}, need ${SAMPLE_RATE}`);
  if (!isObj(lite.config)) shape("config", lite.config, "an object");
  checkShape(lite.config);
  const weights = checkWeights(lite.weights);

  const md = isObj(root.metadata) ? root.metadata : {};
  const lmd = isObj(lite.metadata) ? lite.metadata : {};
  const rawName = typeof md.name === "string" && md.name.trim() !== "" ? md.name.trim() : fallbackName;
  const info: ModelInfo = {
    name: sanitiseName(rawName),
    gearType: textOrNull(md.gear_type),
    gearMake: textOrNull(md.gear_make),
    gearModel: textOrNull(md.gear_model),
    modeledBy: textOrNull(md.modeled_by),
    loudness: finiteOrNull(lmd.loudness),
    inputLevelDbu: finiteOrNull(md.input_level_dbu),
    outputLevelDbu: finiteOrNull(md.output_level_dbu),
    headScale: weights[NETWORK_WEIGHT_COUNT]!,
  };

  const bin = new Uint8Array(MODEL_BIN_SIZE);
  const dv = new DataView(bin.buffer);
  writeAscii(bin, 0, MODEL_MAGIC);
  dv.setUint16(4, MODEL_FORMAT_VERSION, true);
  let flags = 0;
  if (info.loudness !== null) flags |= FLAG_HAS_LOUDNESS;
  if (info.inputLevelDbu !== null) flags |= FLAG_HAS_INPUT_LEVEL;
  dv.setUint16(OFF_FLAGS, flags, true);
  dv.setUint32(OFF_SIZE, MODEL_BIN_SIZE, true);
  bin.set([CHANNELS, CHANNELS, NUM_LAYERS, HEAD_KERNEL], OFF_DESC);
  bin.set(KERNEL_SIZES, OFF_KERNELS);
  DILATIONS.forEach((d, i) => {
    dv.setUint16(OFF_DILATIONS + 2 * i, d, true);
  });
  dv.setUint32(OFF_WEIGHT_COUNT, NETWORK_WEIGHT_COUNT, true);
  for (let i = 0; i < NETWORK_WEIGHT_COUNT; i++) dv.setFloat32(OFF_WEIGHTS + 4 * i, weights[i]!, true);
  dv.setFloat32(OFF_HEAD_SCALE, info.headScale, true);
  dv.setFloat32(OFF_LOUDNESS, info.loudness ?? 0, true);
  dv.setFloat32(OFF_INPUT_LEVEL, info.inputLevelDbu ?? 0, true);
  const putText = (at: number, s: string | null): void => {
    if (s !== null) writeAscii(bin, at, s);
  };
  putText(OFF_NAME, info.name);
  putText(OFF_GEAR_TYPE, info.gearType);
  putText(OFF_GEAR_MAKE, info.gearMake);
  putText(OFF_GEAR_MODEL, info.gearModel);
  putText(OFF_MODELED_BY, info.modeledBy);
  dv.setUint32(OFF_CRC, crc32(bin.subarray(0, OFF_CRC)), true);
  return { bin, info };
}

/** A model binary's network and what the morph alignment needs beside it (morph/align.ts). */
export interface ModelWeights {
  weights: Float32Array; // the 1870 stream weights, without head_scale
  headScale: number;
  loudness: number | null;
  inputLevelDbu: number | null;
  crc: number; // the binary's stored CRC
}

/** Reads a model binary (MODEL_BIN_SIZE bytes, e.g. a stored preset's); nothing is checked. */
export function modelWeights(bin: Uint8Array): ModelWeights {
  const dv = new DataView(bin.buffer, bin.byteOffset, bin.length);
  const flags = dv.getUint16(OFF_FLAGS, true);
  const weights = new Float32Array(NETWORK_WEIGHT_COUNT);
  for (let i = 0; i < NETWORK_WEIGHT_COUNT; i++) weights[i] = dv.getFloat32(OFF_WEIGHTS + 4 * i, true);
  return {
    weights,
    headScale: dv.getFloat32(OFF_HEAD_SCALE, true),
    loudness: flags & FLAG_HAS_LOUDNESS ? dv.getFloat32(OFF_LOUDNESS, true) : null,
    inputLevelDbu: flags & FLAG_HAS_INPUT_LEVEL ? dv.getFloat32(OFF_INPUT_LEVEL, true) : null,
    crc: dv.getUint32(OFF_CRC, true),
  };
}

/** A copy of a model binary with other weights and head_scale, and its CRC recomputed: for fixtures and tests. */
export function withWeights(bin: Uint8Array, weights: ArrayLike<number>, headScale: number): Uint8Array {
  const out = bin.slice();
  const dv = new DataView(out.buffer);
  for (let i = 0; i < NETWORK_WEIGHT_COUNT; i++) dv.setFloat32(OFF_WEIGHTS + 4 * i, weights[i]!, true);
  dv.setFloat32(OFF_HEAD_SCALE, headScale, true);
  dv.setUint32(OFF_CRC, crc32(out.subarray(0, OFF_CRC)), true);
  return out;
}
