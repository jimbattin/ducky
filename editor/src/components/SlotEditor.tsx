import { useState } from "preact/hooks";
import { NAME_MAX } from "../device";
import { type Draft, irName, isEdited, modelName, withModel } from "../draft";
import { type IrMeta, importWarnings, loadIr, loadModel, rejectionText } from "../importer";
import { type MorphKind, SLOT_STATUS, type SlotMessage } from "../link/protocol";
import type { SlotInfo, StoredModel } from "../preset/info";
import { resolveMorph } from "../preset/morph";
import { Details } from "./Details";
import { FileButton } from "./FileButton";
import { GroupBox } from "./GroupBox";
import { Lamp } from "./Lamp";

interface Props {
  slot: SlotMessage;
  /** The pedal's 8 slots: the morph targets to choose from. */
  slots: readonly SlotMessage[];
  /** Each slot's stored metadata, null until read (or where the slot is not ok). */
  infos: readonly (SlotInfo | null)[];
  draft: Draft;
  /** The morph the firmware plays (its Identity Reply); null until identified. */
  morphKind: MorphKind | null;
  onChange: (d: Draft) => void;
  onRevert: () => void;
}

/**
 * The selected slot as it will be saved: its name, model and IR, each kept, replaced from a .nam/.wav or removed, and
 * the preset it morphs with, with the metadata and warnings of the result. Changes stay in the draft until the Slots
 * box's Save.
 */
export function SlotEditor({ slot, slots, infos, draft, morphKind, onChange, onRevert }: Props) {
  const [error, setError] = useState<string | null>(null);
  const n = slot.slot + 1;
  const info = infos[slot.slot] ?? null;
  const edited = isEdited(draft, slot);

  const pick = async <T,>(load: () => Promise<T>, apply: (v: T) => void) => {
    setError(null);
    try {
      apply(await load());
    } catch (e) {
      setError(rejectionText(e));
    }
  };
  const onModel = (f: File) =>
    void pick(
      () => loadModel(f),
      (m) => onChange(withModel(draft, slot, m)),
    );
  const onIr = (f: File) =>
    void pick(
      () => loadIr(f),
      (ir) => onChange({ ...draft, ir: { kind: "new", value: ir } }),
    );

  const model: StoredModel | null =
    draft.model.kind === "new" ? draft.model.value.info : draft.model.kind === "stored" ? (info?.model ?? null) : null;
  const ir: IrMeta | null =
    draft.ir.kind === "new" ? draft.ir.value.info : draft.ir.kind === "stored" ? (info?.ir ?? null) : null;
  const reading = (draft.model.kind === "stored" || draft.ir.kind === "stored") && info === null;
  const warnings = model ? importWarnings(model, ir) : [];
  const partText = (name: string | null, kind: string) =>
    name === null ? "None" : kind === "new" ? `${name} (new)` : name;

  // The stored morph: the slot its block resolves to. A block that resolves to none is stale (its target's model was
  // replaced or erased), which is only certain once every ok slot's info has been read.
  const block = info?.morph ?? null;
  const storedMorph = resolveMorph(infos, slot.slot);
  const infosRead = slots.every((x, k) => x.status !== SLOT_STATUS.ok || (infos[k] ?? null) !== null);
  if (block !== null && storedMorph === null && draft.morph === null && draft.model.kind !== "none" && infosRead)
    warnings.push(
      `Slot ${block.target + 1}'s model changed since the morph was analysed: pick it again and save to re-analyse`,
    );
  const percent = (fit: number) => (100 * fit).toFixed(1);

  return (
    <GroupBox title={edited ? `Slot ${n} (unsaved)` : `Slot ${n}`}>
      <div class="edit-form">
        <label for="edit-name">Name</label>
        <div>
          <input
            id="edit-name"
            type="text"
            maxLength={NAME_MAX}
            value={draft.name}
            disabled={draft.model.kind === "none"}
            onInput={(e) => onChange({ ...draft, name: e.currentTarget.value })}
          />
        </div>
        <span>Model</span>
        <div>
          <span class="edit-part" data-kind={draft.model.kind}>
            {partText(modelName(draft, slot), draft.model.kind)}
          </span>
          <FileButton
            label={draft.model.kind === "none" ? "Add .nam…" : "Replace…"}
            accept=".nam"
            small
            onFile={onModel}
          />
          <button
            type="button"
            class="btn small"
            disabled={draft.model.kind === "none"}
            title="Saving without a model empties the slot"
            onClick={() => onChange({ ...draft, model: { kind: "none" }, ir: { kind: "none" } })}
          >
            Remove
          </button>
        </div>
        <span>IR</span>
        <div>
          <span class="edit-part" data-kind={draft.ir.kind}>
            {partText(irName(draft, slot), draft.ir.kind)}
          </span>
          <FileButton label={draft.ir.kind === "none" ? "Add .wav…" : "Replace…"} accept=".wav" small onFile={onIr} />
          <button
            type="button"
            class="btn small"
            disabled={draft.ir.kind === "none"}
            onClick={() => onChange({ ...draft, ir: { kind: "none" } })}
          >
            Remove
          </button>
        </div>
        <label for="edit-morph">Morph to</label>
        <div>
          <select
            id="edit-morph"
            value={String(draft.morph ?? storedMorph ?? "none")}
            disabled={draft.model.kind === "none"}
            onChange={(e) => {
              const v = e.currentTarget.value === "none" ? "none" : Number(e.currentTarget.value);
              onChange({ ...draft, morph: v === (storedMorph ?? "none") ? null : v });
            }}
          >
            <option value="none">None</option>
            {slots
              .filter((x) => x.status === SLOT_STATUS.ok && x.slot !== slot.slot)
              .map((x) => (
                <option key={x.slot} value={String(x.slot)}>
                  {x.slot + 1}: {x.name}
                </option>
              ))}
          </select>
        </div>
      </div>
      {block !== null &&
        storedMorph !== null &&
        draft.morph === null &&
        draft.model.kind !== "none" &&
        (morphKind === "weight" ? (
          <p class="hint">
            {block.waypoints > 0
              ? `Trained path, ${block.waypoints} waypoint${block.waypoints === 1 ? "" : "s"}: fit ${percent(block.fit)} % ` +
                `(${percent(block.unalignedFit)} % as a straight blend)`
              : `Fit ${percent(block.fit)} %${
                  block.fit > block.unalignedFit ? `, aligned (${percent(block.unalignedFit)} % unaligned)` : ""
                }`}
            : how closely the blends follow a crossfade of the two models.
            {block.waypoints > 0
              ? ` Picking a target here stores a straight blend; tools/pedal.ts morph ${n} ${storedMorph + 1} --train trains a path.`
              : ` tools/pedal.ts morph ${n} ${storedMorph + 1} --train trains a path through waypoints instead.`}
          </p>
        ) : morphKind === "crossfade" && block.waypoints > 0 ? (
          <p class="hint">
            A trained path ({block.waypoints} waypoint{block.waypoints === 1 ? "" : "s"}) is stored for the weight morph
            firmware (APP=weight); this firmware crossfades the two models instead. Picking a target here replaces the
            path with a straight blend.
          </p>
        ) : null)}
      {error !== null && (
        <p class="edit-error" role="alert">
          {error}
        </p>
      )}
      {model ? (
        <Details model={model} ir={ir} />
      ) : reading ? (
        <p class="tab-empty">Reading…</p>
      ) : (
        <p class="tab-empty">{edited ? "Saving will empty this slot." : "Empty: add a .nam to make a preset."}</p>
      )}
      {warnings.length > 0 && (
        <ul class="edit-warnings">
          {warnings.map((w) => (
            <li key={w}>
              <Lamp color="amber" />
              {w}
            </li>
          ))}
        </ul>
      )}
      <div class="edit-actions">
        <button type="button" class="btn" disabled={!edited} onClick={onRevert}>
          Revert slot {n}
        </button>
      </div>
    </GroupBox>
  );
}
