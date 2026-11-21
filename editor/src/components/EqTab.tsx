import { BANDS, bandParamNames, resonanceInactive } from "../eqModel";
import type { ParamInfo, StatusMessage } from "../link/protocol";
import { EqGraph } from "./EqGraph";
import { ParamGroup } from "./ParamRow";
import { SettingsActions, type SettingsProps } from "./SettingsActions";

interface Props extends SettingsProps {
  /** The pedal's last STATUS (Knob 2, the mute); null until one arrives. */
  status: StatusMessage | null;
}

/**
 * The EQ tab: the response graph with a handle per band, then the stage's switch and each band's parameters (set
 * here, heard at once), and Save, which keeps the values (and the Controls tab's assignments) for the next power-up.
 */
export function EqTab({ status, table, enabled, edited, onSet, onSave }: Props) {
  const { params, codes } = table;
  const enable = params.find((p) => p.name === "eq_enable");
  const eqOff = enable !== undefined && codes[enable.id] === 0;
  const inactive = (p: ParamInfo) => (eqOff && p !== enable) || resonanceInactive(params, codes, p.name);
  const group = { table, inactive, enabled, onSet };
  return (
    <>
      <EqGraph status={status} params={params} codes={codes} enabled={enabled} onSet={onSet} />
      <ParamGroup title="Stage" names={["eq_enable"]} {...group} />
      {BANDS.map((b) => (
        <ParamGroup key={b.id} title={b.label} names={bandParamNames(b)} {...group} />
      ))}
      <SettingsActions enabled={enabled} edited={edited} onSave={onSave} />
    </>
  );
}
