import { fixed } from "../format";
import type { StatusMessage } from "../link/protocol";
import { GATE_HYSTERESIS_DB, type GateSettings } from "../preset/gate";
import type { LampColor } from "./Lamp";
import { Meter } from "./Meter";

interface Props {
  /** The latest STATUS; null before the first. */
  status: StatusMessage | null;
  /** The gate shown in the editor (its threshold places the markers). */
  gate: GateSettings;
  /** The edited preset is the one the pedal plays (the cut is only its gate's). */
  playing: boolean;
}

const FLOOR_DB = -90;
const SILENCE_DB = -120;
const CLOSED_CUT_DB = 40;
const pos = (db: number): number => Math.min(Math.max((db - FLOOR_DB) / -FLOOR_DB, 0), 1);

/**
 * The gate's input level against its thresholds: the level at the jack as the fill on −90..0 dBFS, markers where the
 * gate closes and opens, and the gate's state from its gain reduction while this preset plays.
 */
export function GateMeter({ status, gate, playing }: Props) {
  const level = status?.gateLevelDb ?? SILENCE_DB;
  const levelText = level <= SILENCE_DB ? `in < ${fixed(SILENCE_DB, 0)} dBFS` : `in ${fixed(level, 1)} dBFS`;
  const cut = status?.gateCutDb ?? 0;
  let lamp: LampColor | null = null;
  let state: string;
  let readout = levelText;
  if (status === null) {
    state = "—";
    readout = "";
  } else if (!playing) {
    state = "Input level (this preset is not playing)";
  } else if (!gate.enabled) {
    state = "Gate off";
  } else {
    readout = `${levelText} · cut ${cut.toFixed(1)} dB`;
    if (cut < 1) {
      lamp = "green";
      state = "Open";
    } else if (cut >= CLOSED_CUT_DB) {
      state = "Closed";
    } else {
      lamp = "amber";
      state = "Closing";
    }
  }
  const close = gate.thresholdDb;
  const open = close + GATE_HYSTERESIS_DB;
  return (
    <Meter
      class="gate-meter"
      label="Gate input level"
      min={FLOOR_DB}
      max={0}
      now={status ? Math.max(level, FLOOR_DB) : FLOOR_DB}
      valueText={status ? levelText : "no data yet"}
      fill={pos(level)}
      marks={[
        { at: pos(close), title: "Closes below" },
        { at: pos(open), title: "Opens above", faint: true },
      ]}
      lamp={lamp}
      state={<span>{state}</span>}
      readout={readout}
    />
  );
}
