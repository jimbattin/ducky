// The repo's tests/fixtures and the pedal's parameter table, for the editor's tests.
import type { ParamInfo } from "./link/protocol";

export const FIXTURES = `${import.meta.dir}/../../tests/fixtures`;

export const readFixture = async (rel: string): Promise<Uint8Array> =>
  new Uint8Array(await Bun.file(`${FIXTURES}/${rel}`).arrayBuffer());

export const readFixtureText = (rel: string): Promise<string> => Bun.file(`${FIXTURES}/${rel}`).text();

const SLOPES = "24 dB/oct,12 dB/oct,6 dB/oct";
const Q = { taper: "log", off: "none", defCode: 8192, min: 0.5, max: 2, unit: "Q", group: "EQ" } as const;
const CHOICE = { taper: "choice", off: "none", defCode: 1, min: 0, max: 2, group: "EQ" } as const;

/** The pedal's parameter table as DUMP sends it (daisy/src/params.h kParams; docs/MIDI.md "Parameters"). */
export const FACTORY_PARAMS: ParamInfo[] = [
  {
    id: 0,
    name: "low_cut_freq",
    label: "Low-cut frequency",
    factory: 2,
    taper: "log",
    off: "min",
    defCode: 0,
    min: 20,
    max: 500,
    unit: "Hz",
    group: "EQ",
  },
  { id: 1, name: "low_cut_res", label: "Low-cut resonance", factory: 3, ...Q },
  { id: 2, name: "low_cut_slope", label: "Low-cut slope", factory: 4, unit: SLOPES, ...CHOICE },
  {
    id: 3,
    name: "high_cut_freq",
    label: "High-cut frequency",
    factory: 1,
    taper: "log",
    off: "max",
    defCode: 16383,
    min: 1500,
    max: 20000,
    unit: "Hz",
    group: "EQ",
  },
  { id: 4, name: "high_cut_res", label: "High-cut resonance", factory: 0, ...Q },
  { id: 5, name: "high_cut_slope", label: "High-cut slope", factory: 5, unit: SLOPES, ...CHOICE },
  { id: 6, name: "mid", label: "Mid 515 Hz", factory: 6, unit: "-7 dB,off,+5 dB", ...CHOICE },
];
