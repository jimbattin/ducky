import type { ParamInfo, StatusMessage } from "../link/protocol";
import { CompMeter } from "./CompMeter";
import { ParamGroup } from "./ParamRow";
import { SettingsActions, type SettingsProps } from "./SettingsActions";

interface Props extends SettingsProps {
  /** The pedal's last STATUS (the gain reduction); null until one arrives. */
  status: StatusMessage | null;
}

/**
 * The Compressor tab: its gain-reduction meter, then its mode (Off, Blend, Direct: one three-way switch) and its type
 * (OTC, OTA: which model runs; Sustain reads in dB on the OTC, in % of the Sensitivity pot on the OTA), its settings
 * (set here, heard at once), and Save, which keeps the values (and the Controls tab's assignments) for the next
 * power-up.
 */
export function CompTab({ status, table, enabled, edited, onSet, onSave }: Props) {
  const mode = table.params.find((p) => p.name === "comp_mode");
  const off = mode !== undefined && table.codes[mode.id] === 0;
  const group = { table, inactive: (p: ParamInfo) => off && p !== mode, enabled, onSet };
  return (
    <>
      <CompMeter status={status} on={!off} />
      <ParamGroup title="Mode" names={["comp_mode", "comp_type"]} {...group} />
      <ParamGroup title="Compressor" names={["comp_sustain", "comp_level"]} {...group} />
      <ParamGroup title="Output" names={["comp_balance"]} {...group} />
      <SettingsActions enabled={enabled} edited={edited} onSave={onSave} />
    </>
  );
}
