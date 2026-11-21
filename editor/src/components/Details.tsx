import { Fragment } from "preact";
import { DEFAULT_MODEL_INPUT_DBU, inputCalDb, levelMatchDb } from "../controls";
import { signedDb, tapsMs } from "../format";
import type { StoredIr, StoredModel } from "../preset/info";

interface Props {
  model: StoredModel;
  ir: StoredIr | null;
}

const orDash = (s: string | null): string => s ?? "—";

/**
 * A model's and an IR's metadata, with what the pedal makes of them (level match, input calibration). Two columns of
 * rows once the list is 45rem (900 px) wide (ducky.css `details` container).
 */
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
    [
      "Input level",
      model.inputLevelDbu === null
        ? `none (assumed ${signedDb(DEFAULT_MODEL_INPUT_DBU, "dBu")})`
        : signedDb(model.inputLevelDbu, "dBu"),
    ],
    ["Input calibration", signedDb(inputCalDb(model.inputLevelDbu))],
    ["IR", ir === null ? "none" : ir.name],
  ];
  if (ir !== null)
    rows.push(
      ["IR taps", `${ir.taps} (${tapsMs(ir.taps)} ms)`],
      ["IR source", `${ir.sourceRate} Hz, ${ir.sourceChannels} ch, ${ir.sourceFrames} frames`],
      ["IR gain", signedDb(ir.gainDb)],
    );
  return (
    <div class="details-box">
      <dl class="details">
        {rows.map(([k, v]) => (
          <Fragment key={k}>
            <dt>{k}</dt>
            <dd>{v}</dd>
          </Fragment>
        ))}
      </dl>
    </div>
  );
}
