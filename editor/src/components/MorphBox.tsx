import { CONTROLS } from "../link/params";
import type { ParamInfo, ValuesMessage } from "../link/protocol";
import type { SlotInfo } from "../preset/info";
import { resolveMorph } from "../preset/morph";
import { GroupBox } from "./GroupBox";
import { ParamRow } from "./ParamRow";
import { SettingsActions } from "./SettingsActions";

interface Props {
  /** The pedal's parameter table; null until it is read. */
  params: readonly ParamInfo[] | null;
  /** The pedal's live assignments and values. */
  values: ValuesMessage | null;
  /** The code to show for a parameter (useParamEdits). */
  shownCode: (id: number) => number | undefined;
  /** Each slot's stored metadata, null where empty or unread. */
  infos: readonly (SlotInfo | null)[];
  /** The selected slot (0-7). */
  selected: number;
  /** The pedal takes requests. */
  enabled: boolean;
  /** An assignment or a value set on the pedal is not saved yet. */
  edited: boolean;
  onSet: (id: number, code: number) => void;
  onSave: () => void;
}

/**
 * The morph parameter (global, like the EQ's: set here, heard at once, kept by Save) beside the selected preset's
 * morph target, which it sweeps towards. Nothing with firmware that has no morph.
 */
export function MorphBox({ params, values, shownCode, infos, selected, enabled, edited, onSet, onSave }: Props) {
  const p = params?.find((x) => x.name === "morph");
  if (p === undefined || values === null) return null;
  const target = resolveMorph(infos, selected);
  const n = selected + 1;
  return (
    <GroupBox title="Morph">
      <div class="group-body">
        <ParamRow
          p={p}
          code={shownCode(p.id) ?? p.defCode}
          controlsOn={CONTROLS.filter((_, c) => values.assign[c] === p.id).map((control) => control.id)}
          inactive={target === null}
          enabled={enabled}
          onSet={onSet}
        />
      </div>
      <p class="hint">
        {target === null
          ? `Slot ${n} has no morph target.`
          : `0 %: slot ${n}'s model. 100 %: slot ${target + 1}'s model. Slot ${n}'s IR and noise gate stay throughout.`}
      </p>
      <SettingsActions enabled={enabled} edited={edited} onSave={onSave} />
    </GroupBox>
  );
}
