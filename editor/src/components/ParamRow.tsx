import { typographic } from "../format";
import { CODE_MAX, choiceLabels, controlsOn, formatParam, shownParam, togglePositions } from "../link/params";
import type { ParamInfo } from "../link/protocol";
import type { ParamTable } from "../useParamEdits";
import { GroupBox } from "./GroupBox";
import { Segmented } from "./Segmented";
import { Slider } from "./Slider";

/** p's readout at codes as hosts show it (shownParam), with the typographic minus. */
export const paramText = (params: readonly ParamInfo[], codes: readonly number[], p: ParamInfo): string =>
  typographic(formatParam(shownParam(params, codes, p), codes[p.id] ?? p.defCode));

interface Props {
  p: ParamInfo;
  /** The code shown (useParamEdits). */
  code: number;
  /** The controls (Knobs 3-6, SW1-SW3, EXP) assigned to it. */
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

interface GroupProps {
  title: string;
  /** The parameters' names, in row order; a name the table lacks has no row. */
  names: readonly string[];
  table: ParamTable;
  /** Whether a parameter's row is dimmed. */
  inactive: (p: ParamInfo) => boolean;
  enabled: boolean;
  onSet: (id: number, code: number) => void;
}

/** A group box of parameter rows, each parameter as hosts show it (shownParam). */
export function ParamGroup({ title, names, table, inactive, enabled, onSet }: GroupProps) {
  const { params, values, codes } = table;
  return (
    <GroupBox title={title}>
      <div class="group-body">
        {names.map((name) => {
          const p = params.find((x) => x.name === name);
          return (
            p && (
              <ParamRow
                key={p.id}
                p={shownParam(params, codes, p)}
                code={codes[p.id]!}
                controlsOn={controlsOn(values.assign, p.id)}
                inactive={inactive(p)}
                enabled={enabled}
                onSet={onSet}
              />
            )
          );
        })}
      </div>
    </GroupBox>
  );
}
