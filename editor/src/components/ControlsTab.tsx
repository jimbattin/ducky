import { CONTROLS } from "../link/params";
import { MAX_CHANNEL, OMNI } from "../link/protocol";
import { AssignSelect } from "./AssignSelect";
import { GroupBox } from "./GroupBox";
import { HeldBadge } from "./HeldBadge";
import { paramText } from "./ParamRow";
import { SettingsActions, type SettingsProps } from "./SettingsActions";

/** EXP CHANNEL's channels past omni: 1-16. */
const CHANNELS = Array.from({ length: MAX_CHANNEL }, (_, i) => i + 1);

interface Props extends Omit<SettingsProps, "onSet"> {
  onAssign: (control: number, id: number | null) => void;
  /** Sets the MIDI channel EXP's CC 11 and the preset CC 102 are taken from: 0 any (omni), 1-16. */
  onExpChannel: (channel: number) => void;
}

/**
 * The Controls tab: which parameter each of Knobs 3-6, SW1-SW3 and EXP (an expression pedal sending MIDI CC 11 to the
 * pedal's USB port, on the channel chosen below) drives, with its value, and Save, which keeps the assignments, the
 * channel and the values for the next power-up. The values are set on the Compressor, EQ and Loop tabs and in the
 * Morph box.
 */
export function ControlsTab({ table, enabled, edited, onAssign, onExpChannel, onSave }: Props) {
  const { params, values, codes } = table;
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
              <output>{p ? paramText(params, codes, p) : "—"}</output>
              {values.held[c] && <HeldBadge kind={control.kind} />}
            </div>
          );
        })}
        <div class="control-row">
          <span class="pedal-id">CH</span>
          <select
            aria-label="EXP MIDI channel"
            value={String(values.expChannel)}
            disabled={!enabled}
            onChange={(e) => onExpChannel(Number(e.currentTarget.value))}
          >
            <option value={String(OMNI)}>Any (omni)</option>
            {CHANNELS.map((ch) => (
              <option key={ch} value={String(ch)}>
                Channel {ch}
              </option>
            ))}
          </select>
          <output>CC 11, 102</output>
        </div>
      </div>
      <SettingsActions enabled={enabled} edited={edited} onSave={onSave} />
      <p class="hint">
        Knobs 1 and 2 are fixed. With nothing saved, the pedal starts in its factory layout. A value set in the editor
        holds until its control moves: a knob or EXP more than 3 % of its travel, a switch to another position. Save
        keeps the assignments, the EXP channel and the values for the next power-up. CC 102 with value 0–7 on that
        channel selects slot 1–8.
      </p>
    </GroupBox>
  );
}
