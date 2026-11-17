import { useState } from "preact/hooks";
import { NAME_MAX } from "../device";
import { type Draft, irName, isEdited, modelName, withModel } from "../draft";
import { type IrMeta, importWarnings, loadIr, loadModel, rejectionText } from "../importer";
import type { SlotMessage } from "../link/protocol";
import type { SlotInfo, StoredModel } from "../preset/info";
import { Details } from "./Details";
import { GroupBox } from "./GroupBox";
import { Lamp } from "./Lamp";

interface Props {
  slot: SlotMessage;
  /** The stored preset's metadata, null until read (or when the slot is not ok). */
  info: SlotInfo | null;
  draft: Draft;
  onChange: (d: Draft) => void;
  onRevert: () => void;
}

/** A file picker styled as a button; resets after each pick so the same file can be picked again. */
function FileButton({ label, accept, onFile }: { label: string; accept: string; onFile: (f: File) => void }) {
  return (
    <label class="btn small file-btn">
      {label}
      <input
        type="file"
        accept={accept}
        class="visually-hidden"
        onChange={(e) => {
          const f = e.currentTarget.files?.[0];
          e.currentTarget.value = "";
          if (f) onFile(f);
        }}
      />
    </label>
  );
}

/**
 * The selected slot as it will be saved: its name, model and IR, each kept, replaced from a .nam/.wav or removed, with
 * the metadata and warnings of the result. Changes stay in the draft until the Slots box's Save.
 */
export function SlotEditor({ slot, info, draft, onChange, onRevert }: Props) {
  const [error, setError] = useState<string | null>(null);
  const n = slot.slot + 1;
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
          <FileButton label={draft.model.kind === "none" ? "Add .nam…" : "Replace…"} accept=".nam" onFile={onModel} />
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
          <FileButton label={draft.ir.kind === "none" ? "Add .wav…" : "Replace…"} accept=".wav" onFile={onIr} />
          <button
            type="button"
            class="btn small"
            disabled={draft.ir.kind === "none"}
            onClick={() => onChange({ ...draft, ir: { kind: "none" } })}
          >
            Remove
          </button>
        </div>
      </div>
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
