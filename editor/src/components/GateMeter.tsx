import type { StatusMessage } from "../link/protocol";
import { GATE_HYSTERESIS_DB, type GateSettings } from "../preset/gate";
import { Lamp } from "./Lamp";

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
const dbText = (db: number): string => (db < 0 ? `\u2212${(-db).toFixed(1)}` : db.toFixed(1));

/**
 * The gate's input level against its thresholds: the level at the jack as the fill on −90..0 dBFS, markers where the
 * gate closes and opens, and the gate's state from its gain reduction while this preset plays.
 */
export function GateMeter({ status, gate, playing }: Props) {
  const level = status?.gateLevelDb ?? SILENCE_DB;
  const levelText = level <= SILENCE_DB ? `in < \u2212${-SILENCE_DB} dBFS` : `in ${dbText(level)} dBFS`;
  const cut = status?.gateCutDb ?? 0;
  let lamp: "green" | "amber" | null = null;
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
    <div class="gate-meter">
      {/* biome-ignore lint/a11y/useSemanticElements: <meter> cannot show the threshold markers */}
      <div
        class="gate-bar"
        role="meter"
        aria-label="Gate input level"
        aria-valuemin={FLOOR_DB}
        aria-valuemax={0}
        aria-valuenow={status ? Math.max(level, FLOOR_DB) : FLOOR_DB}
        aria-valuetext={status ? levelText : "no data yet"}
      >
        <div class="gate-fill" style={{ width: `${pos(level) * 100}%` }} />
        <div class="gate-mark" title="Closes below" style={{ left: `calc(${pos(close) * 100}% - 0.0625rem)` }} />
        <div
          class="gate-mark"
          data-kind="open"
          title="Opens above"
          style={{ left: `calc(${pos(open) * 100}% - 0.0625rem)` }}
        />
      </div>
      <div class="gate-line">
        <Lamp color={lamp} />
        <span>{state}</span>
        <span class="gate-readout">{readout}</span>
      </div>
    </div>
  );
}
