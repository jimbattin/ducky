import { controlsOn } from "../link/params";
import type { MorphKind } from "../link/protocol";
import type { SlotInfo } from "../preset/info";
import { resolveMorph } from "../preset/morph";
import type { ParamTable } from "../useParamEdits";
import { GroupBox } from "./GroupBox";
import { ParamRow } from "./ParamRow";
import { SettingsActions, type SettingsProps } from "./SettingsActions";

interface Props extends Omit<SettingsProps, "table"> {
  /** null until DUMP has been read. */
  table: ParamTable | null;
  /** Each slot's stored metadata, null where empty or unread. */
  infos: readonly (SlotInfo | null)[];
  /** The selected slot (0-7). */
  selected: number;
  /** The morph the firmware plays (its Identity Reply); null until identified. */
  morphKind: MorphKind | null;
}

/**
 * The morph parameter (global, like the EQ's: set here, heard at once, kept by Save) beside the selected preset's
 * morph target, which it sweeps towards. Nothing with firmware that has no morph.
 */
export function MorphBox({ table, infos, selected, morphKind, enabled, edited, onSet, onSave }: Props) {
  const param = table?.params.find((x) => x.name === "morph");
  if (table === null || param === undefined) return null;
  const target = resolveMorph(infos, selected);
  const between =
    morphKind === "crossfade"
      ? " In between, both models run and their outputs crossfade."
      : morphKind === "weight"
        ? " In between, a blend of their weights."
        : "";
  const n = selected + 1;
  return (
    <GroupBox title="Morph">
      <div class="group-body">
        <ParamRow
          p={param}
          code={table.codes[param.id]!}
          controlsOn={controlsOn(table.values.assign, param.id)}
          inactive={target === null}
          enabled={enabled}
          onSet={onSet}
        />
      </div>
      <p class="hint">
        {target === null
          ? `Slot ${n} has no morph target.`
          : `0 %: slot ${n}'s model. 100 %: slot ${target + 1}'s model.${between} ` +
            `Slot ${n}'s IR and noise gate stay throughout.`}
      </p>
      <SettingsActions enabled={enabled} edited={edited} onSave={onSave} />
    </GroupBox>
  );
}
