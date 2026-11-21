import type { ComponentChildren } from "preact";
import { Lamp, type LampColor } from "./Lamp";

export interface MeterMark {
  /** 0..1 */
  at: number;
  title: string;
  /** Half opacity. */
  faint?: boolean;
}

interface Props {
  /** The bar's accessible name and value. */
  label: string;
  min: number;
  max: number;
  now: number;
  valueText: string;
  /** 0..1 */
  fill: number;
  /** Red fill. */
  hot?: boolean;
  marks: readonly MeterMark[];
  lamp: LampColor | null;
  /** Beside the lamp. */
  state: ComponentChildren;
  readout: string;
  /** Extra class on the meter. */
  class?: string;
}

/** A recessed bar with a fill and markers, over a line with a lamp, a state and a readout (CPU load, gate input). */
export function Meter(p: Props) {
  return (
    <div class={p.class ? `meter ${p.class}` : "meter"}>
      {/* biome-ignore lint/a11y/useSemanticElements: <meter> cannot show the markers */}
      <div
        class="meter-bar"
        role="meter"
        aria-label={p.label}
        aria-valuemin={p.min}
        aria-valuemax={p.max}
        aria-valuenow={p.now}
        aria-valuetext={p.valueText}
      >
        <div class="meter-fill" data-hot={p.hot} style={{ width: `${p.fill * 100}%` }} />
        {p.marks.map((m) => (
          <div
            key={m.title}
            class="meter-mark"
            data-faint={m.faint}
            title={m.title}
            style={{ left: `calc(${m.at * 100}% - 0.0625rem)` }}
          />
        ))}
      </div>
      <div class="meter-line">
        <Lamp color={p.lamp} />
        {p.state}
        <span class="meter-readout">{p.readout}</span>
      </div>
    </div>
  );
}
