import { CONTROLS } from "../link/params";
import type { ParamInfo, StatusMessage, ValuesMessage } from "../link/protocol";
import { CompMeter } from "./CompMeter";
import { GroupBox } from "./GroupBox";
import { ParamRow } from "./ParamRow";
import { SettingsActions } from "./SettingsActions";

interface Props {
  /** The pedal's last STATUS (the gain reduction); null until one arrives. */
  status: StatusMessage | null;
  /** The pedal's parameter table; null until it is read. */
  params: readonly ParamInfo[] | null;
  /** Why the table could not be read. */
  paramsError: string | null;
  /** The pedal's live assignments and values. */
  values: ValuesMessage | null;
  /** The code to show for a parameter (useParamEdits). */
  shownCode: (id: number) => number | undefined;
  /** The pedal takes requests. */
  enabled: boolean;
  /** An assignment or a value set on the pedal is not saved yet. */
  edited: boolean;
  onSet: (id: number, code: number) => void;
  onSave: () => void;
}

/**
 * The Compressor tab: its gain-reduction meter, then its mode (Off, Blend, Direct: one three-way switch) and its settings
 * (set here, heard at once), and Save, which keeps the values (and the Controls tab's assignments) for the next
 * power-up.
 */
export function CompTab({ status, params, paramsError, values, shownCode, enabled, edited, onSet, onSave }: Props) {
  if (params === null || values === null) return <p class="tab-empty">{paramsError ?? "Reading the pedal…"}</p>;
  const codes = params.map((p) => shownCode(p.id) ?? p.defCode);
  const mode = params.find((p) => p.name === "comp_mode");
  const off = mode !== undefined && codes[mode.id] === 0;
  const rows = (names: readonly string[]) =>
    names.flatMap((name) => {
      const p = params.find((x) => x.name === name);
      if (p === undefined) return [];
      const on = CONTROLS.filter((_, c) => values.assign[c] === p.id).map((control) => control.id);
      return [
        <ParamRow
          key={p.id}
          p={p}
          code={codes[p.id]!}
          controlsOn={on}
          inactive={off && p !== mode}
          enabled={enabled}
          onSet={onSet}
        />,
      ];
    });
  return (
    <>
      <CompMeter status={status} on={!off} />
      <GroupBox title="Mode">
        <div class="group-body">{rows(["comp_mode"])}</div>
      </GroupBox>
      <GroupBox title="Compressor">
        <div class="group-body">{rows(["comp_sustain", "comp_level"])}</div>
      </GroupBox>
      <GroupBox title="Output">
        <div class="group-body">{rows(["comp_balance"])}</div>
      </GroupBox>
      <SettingsActions enabled={enabled} edited={edited} onSave={onSave} />
    </>
  );
}
