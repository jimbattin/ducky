import type { StatusMessage } from "../link/protocol";
import { Meter } from "./Meter";

interface Props {
  /** The latest STATUS (its load and underruns); null before the first. */
  status: StatusMessage | null;
  /** The underrun total grew at the last STATUS. */
  xrun: boolean;
}

const FULL_PERMILLE = 1000;
const percent = (perMille: number): string => (perMille / 10).toFixed(1);

/** The CPU load as a bar: the average as the fill, the peak as a marker, an overload lamp for dropouts. */
export function CpuMeter({ status, xrun }: Props) {
  const avg = Math.min((status?.loadAvg ?? 0) / FULL_PERMILLE, 1);
  const max = Math.min((status?.loadMax ?? 0) / FULL_PERMILLE, 1);
  const overload = xrun || (status?.loadMax ?? 0) >= FULL_PERMILLE;
  return (
    <Meter
      class="cpu"
      label="Pedal CPU load"
      min={0}
      max={100}
      now={status ? Math.round(avg * 100) : 0}
      valueText={status ? `${percent(status.loadAvg)} % average, ${percent(status.loadMax)} % peak` : "no data yet"}
      fill={avg}
      hot={avg >= 0.8}
      marks={status ? [{ at: max, title: "Peak" }] : []}
      lamp={overload ? "red" : null}
      state={
        <>
          <span>Overload</span>
          {overload && <span class="visually-hidden"> (dropouts)</span>}
        </>
      }
      readout={
        status
          ? `avg ${percent(status.loadAvg)} %  max ${percent(status.loadMax)} %  underruns ${status.underruns}`
          : "—"
      }
    />
  );
}
