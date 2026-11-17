import { Fragment } from "preact";
import type { StatusMessage } from "../link/protocol";
import { GATE_DEFAULT, GATE_PARAMS, type GateSettings, gateNorm, gateStep, gateValue } from "../preset/gate";
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

/** Arrow keys step a setting by its resolution: Slider's 1 % position step rounds back to the same value on a log taper. */
const ARROWS: Readonly<Record<string, 1 | -1>> = { ArrowUp: 1, ArrowRight: 1, ArrowDown: -1, ArrowLeft: -1 };

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
                {/* biome-ignore lint/a11y/noStaticElementInteractions: catches the slider's arrow keys before it handles them */}
                <div
                  onKeyDownCapture={(e) => {
                    const dir = ARROWS[e.key];
                    if (dir === undefined || !enabled) return;
                    e.preventDefault();
                    e.stopPropagation();
                    onChange({ ...gate, [p.key]: gateStep(p, gate[p.key], dir) });
                  }}
                >
                  <Slider
                    label={p.label}
                    norm={gateNorm(p, gate[p.key])}
                    text={p.format(gate[p.key])}
                    disabled={!enabled}
                    onNorm={(n) => onChange({ ...gate, [p.key]: gateValue(p, n) })}
                    onReset={() => onChange({ ...gate, [p.key]: GATE_DEFAULT[p.key] })}
                  />
                  <output class="gate-value">{p.format(gate[p.key])}</output>
                </div>
              </Fragment>
            ))}
          </div>
          <GateMeter status={status} gate={gate} playing={playing} />
          <p class="gate-hint">
            Listens to the guitar input before the model and closes the output. It closes below the threshold and opens
            6 dB above it.{playing && " Changes are heard at once; Save stores them."}
          </p>
        </>
      )}
    </GroupBox>
  );
}
