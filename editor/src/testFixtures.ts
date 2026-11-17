// The repo's tests/fixtures and the pedal's parameter table, for the editor's tests.
import type { ParamInfo } from "./link/protocol";

export const FIXTURES = `${import.meta.dir}/../../tests/fixtures`;

export const readFixture = async (rel: string): Promise<Uint8Array> =>
  new Uint8Array(await Bun.file(`${FIXTURES}/${rel}`).arrayBuffer());

export const readFixtureText = (rel: string): Promise<string> => Bun.file(`${FIXTURES}/${rel}`).text();

const SLOPES = "24 dB/oct,12 dB/oct,6 dB/oct";
const EQ = { off: "none", group: "EQ" } as const;
const RES = { ...EQ, taper: "log", defCode: 8192, min: 0.5, max: 2, unit: "Q" } as const;
const SLOPE = { ...EQ, taper: "choice", defCode: 1, min: 0, max: 2, unit: SLOPES } as const;
const GAIN = { ...EQ, taper: "linear", defCode: 8192, min: -12, max: 12, unit: "dB" } as const;
/** The bells' Q: 0.4 as the pedal's float32 sends it. */
const BELL_Q = { ...EQ, taper: "log", defCode: 5011, min: Math.fround(0.4), max: 8, unit: "Q" } as const;
const HZ = { ...EQ, taper: "log", unit: "Hz" } as const;
const COMP = { off: "none", group: "Compressor" } as const;
const LOOP = { off: "none", group: "Loop" } as const;

/** The pedal's parameter table as DUMP sends it (daisy/src/params.h kParams; docs/MIDI.md "Parameters"). */
export const PARAMS: ParamInfo[] = [
  { ...EQ, id: 0, name: "eq_enable", label: "EQ", taper: "choice", defCode: 1, min: 0, max: 1, unit: "off,on" },
  { ...HZ, id: 1, name: "low_cut_freq", label: "Low-cut frequency", off: "min", defCode: 0, min: 20, max: 500 },
  { ...RES, id: 2, name: "low_cut_res", label: "Low-cut resonance" },
  { ...SLOPE, id: 3, name: "low_cut_slope", label: "Low-cut slope" },
  { ...GAIN, id: 4, name: "bell1_gain", label: "Bell 1 gain" },
  { ...HZ, id: 5, name: "bell1_freq", label: "Bell 1 frequency", defCode: 10022, min: 40, max: 800 },
  { ...BELL_Q, id: 6, name: "bell1_q", label: "Bell 1 Q" },
  { ...GAIN, id: 7, name: "bell2_gain", label: "Bell 2 gain" },
  { ...HZ, id: 8, name: "bell2_freq", label: "Bell 2 frequency", defCode: 8192, min: 200, max: 5000 },
  { ...BELL_Q, id: 9, name: "bell2_q", label: "Bell 2 Q" },
  { ...GAIN, id: 10, name: "bell3_gain", label: "Bell 3 gain" },
  { ...HZ, id: 11, name: "bell3_freq", label: "Bell 3 frequency", defCode: 7996, min: 800, max: 12000 },
  { ...BELL_Q, id: 12, name: "bell3_q", label: "Bell 3 Q" },
  {
    ...HZ,
    id: 13,
    name: "high_cut_freq",
    label: "High-cut frequency",
    off: "max",
    defCode: 16383,
    min: 1500,
    max: 20000,
  },
  { ...RES, id: 14, name: "high_cut_res", label: "High-cut resonance" },
  { ...SLOPE, id: 15, name: "high_cut_slope", label: "High-cut slope" },
  {
    ...COMP,
    id: 16,
    name: "comp_mode",
    label: "Compressor mode",
    taper: "choice",
    defCode: 0,
    min: 0,
    max: 2,
    unit: "Off,Blend,Direct",
  },
  {
    ...COMP,
    id: 17,
    name: "comp_sustain",
    label: "Sustain",
    taper: "linear",
    defCode: 12287,
    min: 0,
    max: Math.fround(20.827854),
    unit: "dB",
  },
  {
    ...COMP,
    id: 18,
    name: "comp_level",
    label: "Level",
    taper: "choice",
    defCode: 1,
    min: 0,
    max: 2,
    unit: "-3 dB,0 dB,+3 dB",
  },
  {
    ...COMP,
    id: 19,
    name: "comp_balance",
    label: "Balance",
    taper: "linear",
    defCode: 16383,
    min: 0,
    max: 100,
    unit: "%",
  },
  {
    ...LOOP,
    id: 20,
    name: "loop_mode",
    label: "Loop mode",
    taper: "choice",
    defCode: 0,
    min: 0,
    max: 2,
    unit: "MISO,pre-model,post-model",
  },
  {
    ...LOOP,
    id: 21,
    name: "loop_send",
    label: "Send level",
    taper: "linear",
    defCode: 8192,
    min: -12,
    max: 12,
    unit: "dB",
  },
  {
    ...LOOP,
    id: 22,
    name: "loop_return",
    label: "Return level",
    taper: "linear",
    defCode: 8192,
    min: -12,
    max: 12,
    unit: "dB",
  },
];
