import { Fragment } from "preact";
import { inputCalDb, levelMatchDb, signedDb } from "../controls";
import type { StoredIr, StoredModel } from "../preset/info";

interface Props {
  model: StoredModel;
  ir: StoredIr | null;
}

const orDash = (s: string | null): string => s ?? "—";
const signedDbu = (x: number): string => `${x >= 0 ? "+" : "−"}${Math.abs(x).toFixed(1)} dBu`;

/** A model's and an IR's metadata, with what the pedal makes of them (level match, input calibration). */
export function Details({ model, ir }: Props) {
  const match = levelMatchDb(model.loudness);
  const rows: [string, string][] = [
    ["Model", model.name],
    ["Gear type", orDash(model.gearType)],
    ["Gear make", orDash(model.gearMake)],
    ["Gear model", orDash(model.gearModel)],
    ["Modeled by", orDash(model.modeledBy)],
    ["Loudness", model.loudness === null ? "none" : signedDb(model.loudness)],
    ["Level match", match === null ? "unity (no loudness)" : signedDb(match)],
    ["Input level", model.inputLevelDbu === null ? "none (assumed +12.0 dBu)" : signedDbu(model.inputLevelDbu)],
    ["Input calibration", signedDb(inputCalDb(model.inputLevelDbu))],
    ["IR", ir === null ? "none" : ir.name],
  ];
  if (ir !== null)
    rows.push(
      ["IR taps", `${ir.taps} (${(ir.taps / 48).toFixed(1)} ms)`],
      ["IR source", `${ir.sourceRate} Hz, ${ir.sourceChannels} ch, ${ir.sourceFrames} frames`],
      ["IR gain", signedDb(ir.gainDb)],
    );
  return (
    <dl class="details">
      {rows.map(([k, v]) => (
        <Fragment key={k}>
          <dt>{k}</dt>
          <dd>{v}</dd>
        </Fragment>
      ))}
    </dl>
  );
}
