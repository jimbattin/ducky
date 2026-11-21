import type { IdentityMessage } from "../link/protocol";
import { type LinkSnapshot, NO_RESPONSE, type PortInfo } from "../midi";
import { FileButton } from "./FileButton";
import { Lamp } from "./Lamp";

interface Props {
  link: LinkSnapshot;
  identity: IdentityMessage | null;
  onInput: (id: string) => void;
  onOutput: (id: string) => void;
  /** DFU and a firmware update are possible: linked, identified, nothing busy. */
  firmwareEnabled: boolean;
  onDfu: () => void;
  /** A firmware image (.bin) picked for the update. */
  onFirmware: (f: File) => void;
}

/** The strip under the title bar: the MIDI link and its ports, the firmware build (and its morph when not the default
 *  crossfade), then DFU mode and the firmware update. */
export function Toolbar(p: Props) {
  const { link, identity } = p;
  return (
    <div class="toolbar" role="toolbar" aria-label="Link">
      <Lamp color={link.linked ? "green" : link.status === NO_RESPONSE ? "amber" : null} />
      <span class="link-status" role="status">
        {link.status}
      </span>
      <PortSelect label="In" ports={link.inputs} value={link.inputId} onSelect={p.onInput} />
      <PortSelect label="Out" ports={link.outputs} value={link.outputId} onSelect={p.onOutput} />
      <span class="toolbar-sep" aria-hidden="true" />
      <span class="toolbar-build">
        {identity ? `build ${identity.build}${identity.morph === "weight" ? ", weight morph" : ""}` : ""}
      </span>
      <button type="button" class="btn" disabled={!p.firmwareEnabled} onClick={p.onDfu}>
        DFU mode…
      </button>
      <FileButton label="Firmware…" accept=".bin" disabled={!p.firmwareEnabled} onFile={p.onFirmware} />
    </div>
  );
}

interface PortSelectProps {
  label: string;
  ports: PortInfo[];
  value: string;
  onSelect: (id: string) => void;
}

function PortSelect({ label, ports, value, onSelect }: PortSelectProps) {
  return (
    <label class="port">
      {label}
      <select value={value} onChange={(e) => onSelect(e.currentTarget.value)}>
        {ports.map((p) => (
          <option key={p.id} value={p.id}>
            {p.name}
          </option>
        ))}
      </select>
    </label>
  );
}
