import { typographic } from "../format";
import { CODE_MAX, choiceLabels, formatParam, togglePositions } from "../link/params";
import type { ParamInfo } from "../link/protocol";
import { Segmented } from "./Segmented";
import { Slider } from "./Slider";

interface Props {
  p: ParamInfo;
  /** The code shown (useParamEdits). */
  code: number;
  /** The controls (Knobs 3-6, SW1-SW3) assigned to it. */
  controlsOn: readonly string[];
  /** Dimmed: the parameter does nothing at the other parameters' values. */
  inactive: boolean;
  /** The pedal takes requests. */
  enabled: boolean;
  onSet: (id: number, code: number) => void;
}

/**
 * One parameter: its label, the controls on it, a slider (continuous) or a button group (choice, each value captioned
 * with the toggle position that selects it on a switch), its readout.
 */
export function ParamRow({ p, code, controlsOn, inactive, enabled, onSet }: Props) {
  const text = typographic(formatParam(p, code));
  return (
    <div class="param-row" data-inactive={inactive}>
      <span>{p.label}</span>
      <span class="pedal-id">{controlsOn.join(" ") || "—"}</span>
      {p.taper === "choice" ? (
        <Segmented
          label={p.label}
          options={choiceLabels(p).map(typographic)}
          value={code}
          disabled={!enabled}
          captions={togglePositions(p)}
          onSelect={(i) => onSet(p.id, i)}
        />
      ) : (
        <Slider
          label={p.label}
          norm={code / CODE_MAX}
          text={text}
          disabled={!enabled}
          onNorm={(n) => onSet(p.id, Math.round(n * CODE_MAX))}
          onReset={() => onSet(p.id, p.defCode)}
        />
      )}
      <output>{text}</output>
    </div>
  );
}
