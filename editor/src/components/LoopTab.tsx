import { GroupBox } from "./GroupBox";
import { LoopDiagram } from "./LoopDiagram";
import { ParamGroup } from "./ParamRow";
import { SettingsActions, type SettingsProps } from "./SettingsActions";

/** What each loop_mode index means for the player. */
const CAPTIONS = [
  "Mono in; both outputs carry the same signal. The input's ring (R In) is not used.",
  "The loop sits before the model: drives, boosts, wahs. The noise gate stays at the output.",
  "The loop sits after the model, before the IR: delays, reverbs. The noise gate acts on the send, so their tails come back ungated.",
];
const CABLING =
  "Use an insert cable (TRS to two TS) on each jack: input tip ← guitar, input ring ← loop return; output tip → amp, output ring → loop send. With nothing in the loop there is no sound.";

/**
 * The Loop tab: the signal flow of the pedal's loop mode, the mode and the send and return levels (set here, heard at
 * once), and Save, which keeps them (and the Controls tab's assignments) for the next power-up.
 */
export function LoopTab({ table, enabled, edited, onSet, onSave }: SettingsProps) {
  const modeParam = table.params.find((p) => p.name === "loop_mode");
  const mode = modeParam === undefined ? 0 : (table.codes[modeParam.id] ?? 0);
  return (
    <>
      <GroupBox class="loop-diagram" title="Signal flow">
        <LoopDiagram mode={mode} />
        <p class="loop-caption">{CAPTIONS[mode] ?? CAPTIONS[0]}</p>
        {mode !== 0 && <p class="loop-caption">{CABLING}</p>}
      </GroupBox>
      <ParamGroup
        title="Mode"
        names={["loop_mode"]}
        table={table}
        inactive={() => false}
        enabled={enabled}
        onSet={onSet}
      />
      <ParamGroup
        title="Levels"
        names={["loop_send", "loop_return"]}
        table={table}
        inactive={() => mode === 0}
        enabled={enabled}
        onSet={onSet}
      />
      <SettingsActions enabled={enabled} edited={edited} onSave={onSave} />
    </>
  );
}
