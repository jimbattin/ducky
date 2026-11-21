import { Fragment } from "preact";
import { GATE_PARAMS, gateNorm, gateStep, gateValue } from "../gateControls";
import type { StatusMessage } from "../link/protocol";
import { GATE_DEFAULT, GATE_HYSTERESIS_DB, type GateSettings } from "../preset/gate";
import { GateMeter } from "./GateMeter";
import { GroupBox } from "./GroupBox";
import { Slider } from "./Slider";

interface Props {
  slotNumber: number;
  /** The gate as it will be saved; null while the stored one is read. */
  gate: GateSettings | null;
  /** The draft changes the gate. */
  edited: boolean;
  /** The slot holds (or will hold) a preset. */
  enabled: boolean;
  /** The slot is the one the pedal plays: changes are heard at once. */
  playing: boolean;
  status: StatusMessage | null;
  onChange: (g: GateSettings) => void;
}

/** The selected preset's noise gate: on/off, its five settings and the meter. */
export function GateEditor({ slotNumber, gate, edited, enabled, playing, status, onChange }: Props) {
  return (
    <GroupBox title={edited ? "Noise gate (unsaved)" : "Noise gate"}>
      {gate === null ? (
        <p class="tab-empty">Reading…</p>
      ) : (
        <>
          <div class="edit-form">
            <span>Gate</span>
            <div>
              <button
                type="button"
                class="btn small"
                aria-pressed={gate.enabled}
                aria-label={`Slot ${slotNumber} noise gate`}
                disabled={!enabled}
                onClick={() => onChange({ ...gate, enabled: !gate.enabled })}
              >
                {gate.enabled ? "On" : "Off"}
              </button>
            </div>
            {GATE_PARAMS.map((p) => (
              <Fragment key={p.key}>
                <span>{p.label}</span>
                <div>
                  <Slider
                    label={p.label}
                    norm={gateNorm(p, gate[p.key])}
                    text={p.format(gate[p.key])}
                    disabled={!enabled}
                    onNorm={(n) => onChange({ ...gate, [p.key]: gateValue(p, n) })}
                    onArrow={(dir) => onChange({ ...gate, [p.key]: gateStep(p, gate[p.key], dir) })}
                    onReset={() => onChange({ ...gate, [p.key]: GATE_DEFAULT[p.key] })}
                  />
                  <output class="gate-value">{p.format(gate[p.key])}</output>
                </div>
              </Fragment>
            ))}
          </div>
          <GateMeter status={status} gate={gate} playing={playing} />
          <p class="hint">
            Listens to the input jack before the compressor and turns the output down: it closes below the threshold and
            opens {GATE_HYSTERESIS_DB} dB above it.{playing && " Changes are heard at once; Save stores them."}
          </p>
        </>
      )}
    </GroupBox>
  );
}
