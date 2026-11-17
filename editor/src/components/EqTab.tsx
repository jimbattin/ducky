import { BANDS, bandParamNames, resonanceInactive } from "../eqModel";
import { CONTROLS } from "../link/params";
import type { ParamInfo, StatusMessage, ValuesMessage } from "../link/protocol";
import { EqGraph } from "./EqGraph";
import { GroupBox } from "./GroupBox";
import { ParamRow } from "./ParamRow";
import { SettingsActions } from "./SettingsActions";

interface Props {
  /** The pedal's last STATUS (Knob 2, the mute); null until one arrives. */
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
 * The EQ tab: the response graph with a handle per band, then the stage's switch and each band's parameters (set
 * here, heard at once), and Save, which keeps the values (and the Controls tab's assignments) for the next power-up.
 */
export function EqTab({ status, params, paramsError, values, shownCode, enabled, edited, onSet, onSave }: Props) {
  if (params === null || values === null) return <p class="tab-empty">{paramsError ?? "Reading the pedal…"}</p>;
  const codes = params.map((p) => shownCode(p.id) ?? p.defCode);
  const enable = params.find((p) => p.name === "eq_enable");
  const eqOff = enable !== undefined && codes[enable.id] === 0;
  const rows = (names: readonly string[]) =>
    names.flatMap((name) => {
      const p = params.find((x) => x.name === name);
      if (p === undefined) return [];
      const on = CONTROLS.filter((_, c) => values.assign[c] === p.id).map((control) => control.id);
      const inactive = (eqOff && p !== enable) || resonanceInactive(params, codes, p.name);
      return [
        <ParamRow
          key={p.id}
          p={p}
          code={codes[p.id]!}
          controlsOn={on}
          inactive={inactive}
          enabled={enabled}
          onSet={onSet}
        />,
      ];
    });
  return (
    <>
      <EqGraph status={status} params={params} codes={codes} enabled={enabled} onSet={onSet} />
      <GroupBox title="Stage">
        <div class="group-body">{rows(["eq_enable"])}</div>
      </GroupBox>
      {BANDS.map((b) => (
        <GroupBox key={b.id} title={b.label}>
          <div class="group-body">{rows(bandParamNames(b))}</div>
        </GroupBox>
      ))}
      <SettingsActions enabled={enabled} edited={edited} onSave={onSave} />
    </>
  );
}
