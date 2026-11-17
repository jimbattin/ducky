import { useRef } from "preact/hooks";

interface Props {
  /** The accessible name. */
  label: string;
  /** Position 0..1 along the parameter's taper. */
  norm: number;
  /** The readout, as the slider's value text. */
  text: string;
  disabled?: boolean;
  /** 11 ticks under the track (the pedal's knobs). */
  ticks?: boolean;
  onNorm: (n: number) => void;
  /** Double-click: back to the default. */
  onReset?: () => void;
}

const FINE = 10; // Shift divides the speed by this
const KEY_STEP = 0.01;
const TICKS = Array.from({ length: 11 }, (_, i) => i);
const clamp01 = (n: number): number => Math.min(Math.max(n, 0), 1);

/** The position a key moves to from norm, or null for a key the slider ignores. */
function keyTarget(key: string, norm: number): number | null {
  switch (key) {
    case "ArrowUp":
    case "ArrowRight":
      return norm + KEY_STEP;
    case "ArrowDown":
    case "ArrowLeft":
      return norm - KEY_STEP;
    case "PageUp":
      return norm + 10 * KEY_STEP;
    case "PageDown":
      return norm - 10 * KEY_STEP;
    case "Home":
      return 0;
    case "End":
      return 1;
    default:
      return null;
  }
}

/**
 * A horizontal Platinum slider. Dragging the thumb moves it from where it is; pressing the track jumps there first.
 * Shift drags 10× finer; arrows step 1 %, Page keys 10 %, Home/End go to the ends; double-click resets.
 */
export function Slider({ label, norm, text, disabled, ticks, onNorm, onReset }: Props) {
  const drag = useRef<{ x: number; norm: number; fine: boolean; span: number } | null>(null);
  const endDrag = () => {
    drag.current = null;
  };
  return (
    <div
      class={ticks ? "slider has-ticks" : "slider"}
      role="slider"
      aria-label={label}
      aria-valuemin={0}
      aria-valuemax={100}
      aria-valuenow={Math.round(norm * 100)}
      aria-valuetext={text}
      aria-disabled={disabled ? "true" : undefined}
      tabIndex={disabled ? -1 : 0}
      onPointerDown={(e) => {
        if (disabled) return;
        const el = e.currentTarget;
        el.setPointerCapture(e.pointerId);
        const track = el.querySelector(".slider-track")?.getBoundingClientRect();
        const thumb = el.querySelector(".slider-thumb")?.getBoundingClientRect().width ?? 0;
        if (!track) return;
        const span = Math.max(track.width - thumb, 1);
        let start = norm;
        if (!(e.target instanceof Element && e.target.closest(".slider-thumb"))) {
          start = clamp01((e.clientX - track.left - thumb / 2) / span);
          onNorm(start);
        }
        drag.current = { x: e.clientX, norm: start, fine: e.shiftKey, span };
      }}
      onPointerMove={(e) => {
        const d = drag.current;
        if (!d || disabled) return;
        const at = clamp01(d.norm + (e.clientX - d.x) / (d.span * (d.fine ? FINE : 1)));
        // Shift pressed or released mid-drag: continue from here at the new speed.
        if (e.shiftKey !== d.fine) drag.current = { ...d, x: e.clientX, norm: at, fine: e.shiftKey };
        else onNorm(at);
      }}
      onPointerUp={endDrag}
      onPointerCancel={endDrag}
      onDblClick={() => {
        if (!disabled) onReset?.();
      }}
      onKeyDown={(e) => {
        if (disabled) return;
        const target = keyTarget(e.key, norm);
        if (target === null) return;
        e.preventDefault();
        onNorm(clamp01(target));
      }}
    >
      <div class="slider-track">
        <div class="slider-fill" style={{ width: `${norm * 100}%` }} />
        <div class="slider-thumb" style={{ left: `calc(${norm} * (100% - var(--thumb-w)))` }} />
      </div>
      {ticks && (
        <div class="slider-ticks" aria-hidden="true">
          {TICKS.map((i) => (
            <i key={i} style={{ left: `${i * 10}%` }} />
          ))}
        </div>
      )}
    </div>
  );
}
