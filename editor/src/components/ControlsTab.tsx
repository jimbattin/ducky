import { resonanceInactive } from "../eqModel";
import { typographic } from "../format";
import { CODE_MAX, CONTROLS, choiceLabels, factoryAssign, formatParam } from "../link/params";
import type { ParamInfo, ValuesMessage } from "../link/protocol";
import { AssignSelect } from "./AssignSelect";
import { GroupBox } from "./GroupBox";
import { HeldBadge } from "./HeldBadge";
import { Segmented } from "./Segmented";
import { Slider } from "./Slider";

interface Props {
  /** The pedal's parameter table; null until it is read. */
  params: readonly ParamInfo[] | null;
  /** Why the table could not be read. */
  paramsError: string | null;
  /** The pedal's live assignments and held controls. */
  values: ValuesMessage | null;
  /** The code to show for a parameter (useParamEdits). */
  shownCode: (id: number) => number | undefined;
  /** The pedal takes requests. */
  enabled: boolean;
  /** An assignment or a value set here is not saved yet. */
  edited: boolean;
  onAssign: (control: number, id: number | null) => void;
  onSet: (id: number, code: number) => void;
  onFactoryLayout: () => void;
  onSave: () => void;
}

/**
 * The Controls tab: which parameter each of Knobs 3-6 and SW1-SW3 drives, every parameter's value (set here, heard at
 * once), and Save, which keeps both for the next power-up.
 */
export function ControlsTab({
  params,
  paramsError,
  values,
  shownCode,
  enabled,
  edited,
  onAssign,
  onSet,
  onFactoryLayout,
  onSave,
}: Props) {
  if (params === null || values === null) return <p class="tab-empty">{paramsError ?? "Reading the pedal…"}</p>;
  const codes = params.map((p) => shownCode(p.id) ?? p.defCode);
  const factory = factoryAssign(params);
  return (
    <>
      <GroupBox title="Controls">
        <div class="group-body">
          {CONTROLS.map((control, c) => {
            const p = params.find((x) => x.id === values.assign[c]);
            return (
              <div class="control-row" key={control.id}>
                <span class="pedal-id">{control.id}</span>
                <AssignSelect
                  label={`${control.id} parameter`}
                  params={params}
                  control={c}
                  value={values.assign[c] ?? null}
                  disabled={!enabled}
                  onSelect={(id) => onAssign(c, id)}
                />
                <output>{p ? typographic(formatParam(p, codes[p.id]!)) : "—"}</output>
                {values.held[c] && <HeldBadge knob={control.knob} />}
              </div>
            );
          })}
        </div>
      </GroupBox>
      <GroupBox title="Parameters">
        <div class="group-body">
          {params.map((p) => {
            const code = codes[p.id]!;
            const text = typographic(formatParam(p, code));
            const on = CONTROLS.filter((_, c) => values.assign[c] === p.id).map((control) => control.id);
            return (
              <div class="param-row" key={p.id} data-inactive={resonanceInactive(params, codes, p.name)}>
                <span>{p.label}</span>
                <span class="pedal-id">{on.join(" ") || "—"}</span>
                {p.taper === "choice" ? (
                  <Segmented
                    label={p.label}
                    options={choiceLabels(p).map(typographic)}
                    value={code}
                    disabled={!enabled}
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
          })}
        </div>
        <div class="edit-actions">
          <span class="edit-count">{edited ? "Not saved on the pedal" : "No unsaved changes"}</span>
          <button
            type="button"
            class="btn"
            disabled={!enabled || factory.every((id, c) => id === values.assign[c])}
            onClick={onFactoryLayout}
          >
            Factory layout
          </button>
          <button type="button" class="btn" disabled={!enabled || !edited} onClick={onSave}>
            Save
          </button>
        </div>
        <p class="hint">
          Knobs 1 and 2 are fixed. A value set here holds until its control moves: a knob more than 3 % of its travel, a
          switch to another position. Save keeps the layout and the values for the next power-up.
        </p>
      </GroupBox>
    </>
  );
}
