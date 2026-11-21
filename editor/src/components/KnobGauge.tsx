import { SliderTrack } from "./Slider";

interface Props {
  /** The accessible name. */
  label: string;
  /** The knob position 0..1. */
  norm: number;
  /** The readout, as the meter's value text. */
  text: string;
}

const TICKS = Array.from({ length: 11 }, (_, i) => i);

/** A pedal knob's position on a Platinum slider track with 11 ticks; read-only (the pedal's knob sets it). */
export function KnobGauge({ label, norm, text }: Props) {
  return (
    // biome-ignore lint/a11y/useSemanticElements: <meter> cannot draw the slider track, thumb and ticks
    <div
      class="slider has-ticks"
      role="meter"
      aria-label={label}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={Math.round(norm * 100)}
      aria-valuetext={text}
    >
      <SliderTrack norm={norm} />
      <div class="slider-ticks" aria-hidden="true">
        {TICKS.map((i) => (
          <i key={i} style={{ left: `${i * 10}%` }} />
        ))}
      </div>
    </div>
  );
}
