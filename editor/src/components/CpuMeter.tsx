import type { PedalLoad } from "../state";
import { Lamp } from "./Lamp";

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
    <div class="cpu">
      {/* biome-ignore lint/a11y/useSemanticElements: <meter> cannot show the peak marker */}
      <div
        class="cpu-bar"
        role="meter"
        aria-label="Pedal CPU load"
        aria-valuemin={0}
        aria-valuemax={100}
        aria-valuenow={load ? Math.round(avg * 100) : 0}
        aria-valuetext={load ? `${percent(load.avg)}% average, ${percent(load.max)}% peak` : "no data yet"}
      >
        <div class="cpu-fill" data-hot={avg >= 0.8} style={{ width: `${avg * 100}%` }} />
        {load && <div class="cpu-peak" style={{ left: `calc(${max * 100}% - 0.125rem)` }} />}
      </div>
      <div class="cpu-line">
        <Lamp color={overload ? "red" : null} />
        <span>Overload</span>
        {overload && <span class="visually-hidden"> (dropouts)</span>}
        <span class="cpu-readout">
          {load ? `avg ${percent(load.avg)} %  max ${percent(load.max)} %  underruns ${load.underruns}` : "—"}
        </span>
      </div>
    </div>
  );
}
