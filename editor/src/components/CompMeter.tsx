import type { StatusMessage } from "../link/protocol";
import type { LampColor } from "./Lamp";
import { Meter } from "./Meter";

interface Props {
  /** The latest STATUS; null before the first. */
  status: StatusMessage | null;
  /** The compressor is on (comp_enable). */
  on: boolean;
}

/** The bar's full scale, dB of gain reduction. */
const FULL_DB = 24;

/**
 * The compressor's gain reduction: how far its low-frequency gain fell below Sustain's since the previous STATUS, as
 * the fill on 0..24 dB, and its state.
 */
export function CompMeter({ status, on }: Props) {
  const cut = status?.compCutDb ?? 0;
  const cutText = `cut ${cut.toFixed(1)} dB`;
  let lamp: LampColor | null = null;
  let state: string;
  if (status === null) {
    state = "—";
  } else if (!on) {
    state = "Compressor off";
  } else if (cut < 1) {
    lamp = "green";
    state = "Not compressing";
  } else {
    lamp = "amber";
    state = "Compressing";
  }
  return (
    <Meter
      class="comp-meter"
      label="Compressor gain reduction"
      min={0}
      max={FULL_DB}
      now={cut}
      valueText={status ? cutText : "no data yet"}
      fill={Math.min(cut / FULL_DB, 1)}
      marks={[]}
      lamp={lamp}
      state={<span>{state}</span>}
      readout={status ? cutText : ""}
    />
  );
}
