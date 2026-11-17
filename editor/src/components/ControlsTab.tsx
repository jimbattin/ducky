import { typographic } from "../format";
import { CONTROLS, formatParam } from "../link/params";
import type { ParamInfo, ValuesMessage } from "../link/protocol";
import { AssignSelect } from "./AssignSelect";
import { GroupBox } from "./GroupBox";
import { HeldBadge } from "./HeldBadge";
import { SettingsActions } from "./SettingsActions";

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
  /** An assignment or a value set on the pedal is not saved yet. */
  edited: boolean;
  onAssign: (control: number, id: number | null) => void;
  onSave: () => void;
}

/**
 * The Controls tab: which parameter each of Knobs 3-6 and SW1-SW3 drives, with its value, and Save, which keeps the
 * assignments and the values for the next power-up. The values are set on the EQ and Compressor tabs.
 */
export function ControlsTab({ params, paramsError, values, shownCode, enabled, edited, onAssign, onSave }: Props) {
  if (params === null || values === null) return <p class="tab-empty">{paramsError ?? "Reading the pedal…"}</p>;
  return (
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
              <output>{p ? typographic(formatParam(p, shownCode(p.id) ?? p.defCode)) : "—"}</output>
              {values.held[c] && <HeldBadge knob={control.knob} />}
            </div>
          );
        })}
      </div>
      <SettingsActions enabled={enabled} edited={edited} onSave={onSave} />
      <p class="hint">
        Knobs 1 and 2 are fixed. Every control starts unassigned. A value set on the EQ and Compressor tabs holds until
        its control moves: a knob more than 3 % of its travel, a switch to another position. Save keeps the assignments
        and the values for the next power-up.
      </p>
    </GroupBox>
  );
}
