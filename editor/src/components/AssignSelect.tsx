import { accepts } from "../link/params";
import { NO_PARAM, type ParamInfo } from "../link/protocol";

interface Props {
  label: string;
  /** The pedal's parameter table. */
  params: readonly ParamInfo[];
  /** The control (0-6) the menu assigns. */
  control: number;
  /** The assigned parameter's id; null: none. */
  value: number | null;
  disabled: boolean;
  onSelect: (id: number | null) => void;
}

/** A control's parameter: a menu of None and the parameters the control accepts, by group. */
export function AssignSelect({ label, params, control, value, disabled, onSelect }: Props) {
  const allowed = params.filter((p) => accepts(control, p));
  return (
    <select
      aria-label={label}
      value={String(value ?? NO_PARAM)}
      disabled={disabled}
      onChange={(e) => {
        const id = Number(e.currentTarget.value);
        onSelect(id === NO_PARAM ? null : id);
      }}
    >
      <option value={String(NO_PARAM)}>None</option>
      {[...new Set(allowed.map((p) => p.group))].map((group) => (
        <optgroup key={group} label={group}>
          {allowed
            .filter((p) => p.group === group)
            .map((p) => (
              <option key={p.id} value={String(p.id)}>
                {p.label}
              </option>
            ))}
        </optgroup>
      ))}
    </select>
  );
}
