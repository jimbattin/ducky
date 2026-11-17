import { FIXED_KNOBS, knobPosition, positionIndex } from "../controls";
import { resonanceInactive } from "../eqModel";
import { typographic } from "../format";
import { CONTROLS, formatParam, switchLabels } from "../link/params";
import type { ParamInfo } from "../link/protocol";
import type { State } from "../state";
import { CpuMeter } from "./CpuMeter";
import { GroupBox } from "./GroupBox";
import { HeldBadge } from "./HeldBadge";
import { KnobGauge } from "./KnobGauge";
import { Lamp } from "./Lamp";
import { Positions } from "./Positions";

const INACTIVE_TITLE = "No effect: 6 dB/oct slope or the filter is off";
/** A switch with no parameter: its positions by name. */
const POSITIONS = ["Up", "Middle", "Down"] as const;
const KNOB_SLOTS = CONTROLS.flatMap((control, c) => (control.knob ? [{ control, c }] : []));
const SWITCH_SLOTS = CONTROLS.flatMap((control, c) => (control.knob ? [] : [{ control, c }]));

/** A switch's position names without the unit they all end in (" dB/oct"), which goes to the accessible name. */
function splitUnit(labels: readonly string[]): { options: readonly string[]; unit: string } {
  const first = labels[0] ?? "";
  const unit = first.includes(" ") ? first.slice(first.indexOf(" ")) : "";
  return unit !== "" && labels.every((l) => l.endsWith(unit))
    ? { options: labels.map((l) => l.slice(0, -unit.length)), unit: `, ${unit.trim()}` }
    : { options: labels, unit: "" };
}

/**
 * The pedal's own controls as its STATUS and VALUES streams report them: the footswitches, Knobs 1 and 2 in their
 * units, what each of Knobs 3-6 and SW1-SW3 drives (the Controls tab assigns them) with that parameter's value, and
 * the CPU load. A control marked Held waits to be moved before it takes its parameter back. Read-only.
 */
export function FrontPanel({ s }: { s: State }) {
  const st = s.status;
  const k = (i: number): number => (st ? knobPosition(st.knobs, i) : 0);
  const playing = st?.slot ?? null;
  const preset = !st ? "—" : playing === null ? "Dry" : `${playing + 1} "${s.slots?.[playing]?.name ?? ""}"`;
  const { params, values } = s;
  /** The parameter on control c; undefined for none or before the table is read. */
  const assigned = (c: number): ParamInfo | undefined =>
    params && values ? params.find((p) => p.id === values.assign[c]) : undefined;
  /** The controls take their names from the table: unknown until it is read. */
  const nameOf = (p: ParamInfo | undefined): string => p?.label ?? (params && values ? "None" : "—");

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
          {FIXED_KNOBS.map((knob, i) => {
            const text = st ? knob.readout(k(i)) : "—";
            return (
              <div key={knob.id}>
                <div class="pedal-knob-head">
                  <span class="pedal-id">{knob.id}</span>
                  <span class="pedal-knob-name">{knob.label}</span>
                  <output>{text}</output>
                </div>
                <KnobGauge label={`Knob ${i + 1} ${knob.label}`} norm={k(i)} text={text} />
              </div>
            );
          })}
          {KNOB_SLOTS.map(({ control, c }) => {
            const p = assigned(c);
            const text = p && values ? typographic(formatParam(p, values.codes[p.id] ?? p.defCode)) : "—";
            const off =
              p !== undefined && params !== null && values !== null && resonanceInactive(params, values.codes, p.name);
            return (
              <div key={control.id} data-inactive={off} title={off ? INACTIVE_TITLE : undefined}>
                <div class="pedal-knob-head">
                  <span class="pedal-id">{control.id}</span>
                  <span class="pedal-knob-name">{nameOf(p)}</span>
                  {values?.held[c] && <HeldBadge knob />}
                  <output>{text}</output>
                </div>
                <KnobGauge
                  label={`Knob ${c + FIXED_KNOBS.length + 1} ${nameOf(p)}`}
                  norm={k(c + FIXED_KNOBS.length)}
                  text={text}
                />
              </div>
            );
          })}
        </div>
      </GroupBox>
      <GroupBox title="Switches">
        <div class="group-body">
          {SWITCH_SLOTS.map(({ control, c }, t) => {
            const p = assigned(c);
            const { options, unit } = splitUnit(p ? switchLabels(p).map(typographic) : POSITIONS);
            return (
              <div class="pedal-row" key={control.id}>
                <span>
                  <span class="pedal-id">{control.id}</span> {nameOf(p)} {values?.held[c] && <HeldBadge knob={false} />}
                </span>
                <Positions
                  label={`${control.id} ${nameOf(p)}${unit}`}
                  options={options}
                  value={st ? positionIndex(st.toggles[t]!) : -1}
                />
              </div>
            );
          })}
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
