import type { PedalLoad } from "../state";
import { Meter } from "./Meter";

interface Props {
  /** null = no status line yet. */
  load: PedalLoad | null;
  /** A dropout total grew at the last status line. */
  xrun: boolean;
}

const FULL_PERMILLE = 1000;
const percent = (perMille: number): string => (perMille / 10).toFixed(1);

/** The CPU load as a bar: the average as the fill, the peak as a marker, an overload lamp for dropouts. */
export function CpuMeter({ load, xrun }: Props) {
  const avg = Math.min((load?.avg ?? 0) / FULL_PERMILLE, 1);
  const max = Math.min((load?.max ?? 0) / FULL_PERMILLE, 1);
  const overload = xrun || (load?.max ?? 0) >= FULL_PERMILLE;
  return (
    <Meter
      class="cpu"
      label="Pedal CPU load"
      min={0}
      max={100}
      now={load ? Math.round(avg * 100) : 0}
      valueText={load ? `${percent(load.avg)}% average, ${percent(load.max)}% peak` : "no data yet"}
      fill={avg}
      hot={avg >= 0.8}
      marks={load ? [{ at: max, title: "Peak" }] : []}
      lamp={overload ? "red" : null}
      state={
        <>
          <span>Overload</span>
          {overload && <span class="visually-hidden"> (dropouts)</span>}
        </>
      }
      readout={load ? `avg ${percent(load.avg)} %  max ${percent(load.max)} %  underruns ${load.underruns}` : "—"}
    />
  );
}
