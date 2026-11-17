import { CONTROLS } from "../link/params";
import type { ParamInfo, ValuesMessage } from "../link/protocol";
import { GroupBox } from "./GroupBox";
import { LoopDiagram } from "./LoopDiagram";
import { ParamRow } from "./ParamRow";
import { SettingsActions } from "./SettingsActions";

interface Props {
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
export function LoopTab({ params, paramsError, values, shownCode, enabled, edited, onSet, onSave }: Props) {
  if (params === null || values === null) return <p class="tab-empty">{paramsError ?? "Reading the pedal…"}</p>;
  const codes = params.map((p) => shownCode(p.id) ?? p.defCode);
  const modeParam = params.find((p) => p.name === "loop_mode");
  const mode = modeParam === undefined ? 0 : (codes[modeParam.id] ?? 0);
  const rows = (names: readonly string[], inactive: boolean) =>
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
          inactive={inactive}
          enabled={enabled}
          onSet={onSet}
        />,
      ];
    });
  return (
    <>
      <GroupBox class="loop-diagram" title="Signal flow">
        <LoopDiagram mode={mode} />
        <p class="loop-caption">{CAPTIONS[mode] ?? CAPTIONS[0]}</p>
        {mode !== 0 && <p class="loop-caption">{CABLING}</p>}
      </GroupBox>
      <GroupBox title="Mode">
        <div class="group-body">{rows(["loop_mode"], false)}</div>
      </GroupBox>
      <GroupBox title="Levels">
        <div class="group-body">{rows(["loop_send", "loop_return"], mode === 0)}</div>
      </GroupBox>
      <SettingsActions enabled={enabled} edited={edited} onSave={onSave} />
    </>
  );
}
