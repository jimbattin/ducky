import { KNOBS, knobPosition, positionIndex, resonanceActive, SWITCHES } from "../controls";
import type { State } from "../state";
import { CpuMeter } from "./CpuMeter";
import { GroupBox } from "./GroupBox";
import { KnobGauge } from "./KnobGauge";
import { Lamp } from "./Lamp";
import { Positions } from "./Positions";

const INACTIVE_TITLE = "No effect: 6 dB/oct slope or the filter is off";

/**
 * The pedal's own controls as its STATUS stream reports them (what you see is what you get): the footswitches, the
 * six knobs in their units, the three toggles and the CPU load. Read-only: the editor never sets a control.
 */
export function FrontPanel({ s }: { s: State }) {
  const st = s.status;
  const k = (i: number): number => (st ? knobPosition(st.knobs, i) : 0);
  // K3 is the high-cut's resonance (K4, SW2), K6 the low-cut's (K5, SW1).
  const inactive = (id: string): boolean =>
    st !== null &&
    ((id === "K3" && !resonanceActive(k(3), st.toggles[1]!, false)) ||
      (id === "K6" && !resonanceActive(k(4), st.toggles[0]!, true)));
  const playing = st?.slot ?? null;
  const preset = !st ? "—" : playing === null ? "Dry" : `${playing + 1} "${s.slots?.[playing]?.name ?? ""}"`;

  return (
    <aside class="pedal" aria-label="Pedal controls">
      <GroupBox title="Footswitches">
        <div class="group-body">
          <div class="pedal-row">
            <span>
              <span class="pedal-id">FS1</span> Mute
            </span>
            <output>
              <Lamp color={st && !st.muted ? "green" : null} />
              {st ? (st.muted ? "Muted" : "Playing") : "—"}
            </output>
          </div>
          <div class="pedal-row">
            <span>
              <span class="pedal-id">FS2</span> Preset
            </span>
            <output class="slot-cell">{preset}</output>
          </div>
        </div>
      </GroupBox>
      <GroupBox title="Knobs">
        <div class="group-body">
          {KNOBS.map((knob, i) => {
            const text = st ? knob.readout(k(i)) : "—";
            const off = inactive(knob.id);
            return (
              <div key={knob.id} data-inactive={off} title={off ? INACTIVE_TITLE : undefined}>
                <div class="pedal-knob-head">
                  <span class="pedal-id">{knob.id}</span>
                  <span class="pedal-knob-name">{knob.label}</span>
                  <output>{text}</output>
                </div>
                <KnobGauge label={`Knob ${i + 1} ${knob.label}`} norm={k(i)} text={text} />
              </div>
            );
          })}
        </div>
      </GroupBox>
      <GroupBox title="Switches">
        <div class="group-body">
          {SWITCHES.map((sw, i) => (
            <div class="pedal-row" key={sw.id}>
              <span>
                <span class="pedal-id">{sw.id}</span> {sw.label}
              </span>
              <Positions
                label={`${sw.id} ${sw.label}${sw.unit ? `, ${sw.unit}` : ""}`}
                options={sw.options}
                value={st ? positionIndex(st.toggles[i]!) : -1}
              />
            </div>
          ))}
        </div>
      </GroupBox>
      <GroupBox title="CPU load">
        <div class="group-body">
          <CpuMeter load={s.load} xrun={s.xrun} />
        </div>
      </GroupBox>
    </aside>
  );
}
